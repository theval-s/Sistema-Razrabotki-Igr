#include <engine/engine.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {
struct ComponentBindingFixture : testing::Test {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-component-bindings-" + engine::NewUuid());
    std::filesystem::path previousRoot = engine::AssetPathResolver::GetGameRoot();
    engine::Engine host;
    engine::LuaRuntime runtime;
    std::string error;

    void SetUp() override {
        std::filesystem::create_directories(root);
        engine::AssetPathResolver::SetGameRoot(root);
        ASSERT_TRUE(runtime.Initialize(root, host, error)) << error;
    }
    void TearDown() override {
        runtime.Shutdown();
        engine::AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void Lua(std::string_view source) {
        ASSERT_TRUE(runtime.ExecuteString(source, "component binding assertion", error)) << error;
    }
    void ScriptAsset() {
        ASSERT_TRUE(engine::CreateNewFile(root / "spawn.lua", "return {}", error)) << error;
        engine::AssetMetadata metadata{engine::NewUuid(), engine::CatalogAssetKind::Script};
        ASSERT_TRUE(engine::WriteAssetMetadata(root / "spawn.lua", metadata, error)) << error;
        // An unrelated engine-root catalog diagnostic must not hide this healthy entry.
        (void)host.GetAssets().RescanCatalog(error);
        ASSERT_NE(host.GetAssets().GetCatalog().FindPath("game/spawn.lua"), nullptr) << error;
    }
};
}

TEST_F(ComponentBindingFixture, CopiedValuesAndTransactionalValidation) {
    Lua(R"(
        local e = Scene.Spawn('values', {components = {
            Camera = {nearPlane=0.2, farPlane=250},
            PointLight = {color=Vector3.new(1, 0.5, 0.25), intensity=3},
            RigidBody = {motionType='Dynamic', mass=4},
            Collider = {shape='Sphere', radius=2, isTrigger=true},
            GamePersistence = {transform=true, scriptState=true},
            Bounds = {min=Vector3.new(-1,-2,-3), max=Vector3.new(1,2,3)}
        }})
        assert(e:HasComponent('Camera') and e:GetComponent('Script') == nil)
        local c = e:GetComponent('Camera'); c.farPlane = 300
        assert(e:GetComponent('Camera').farPlane == 250)
        e:SetComponent('Camera', c)
        assert(e:GetComponent('Camera').farPlane == 300)
        assert(not pcall(function() e:SetComponent('Camera', {nearPlane=400, farPlane=350}) end))
        assert(e:GetComponent('Camera').farPlane == 300)
        assert(not pcall(function() e:SetComponent('Camera', {farPlane=0/0}) end))
        assert(not pcall(function() e:SetComponent('Camera', {farPlane=1e300}) end))
        assert(not pcall(function() e:SetComponent('Camera', {typo=1}) end))
        local light = e:GetComponent('PointLight'); light.color.x = 9
        assert(e:GetComponent('PointLight').color.x == 1)
        assert(e:GetComponent('RigidBody').motionType == 'Dynamic')
        assert(e:GetComponent('Collider').isTrigger)
        local persistence = e:GetComponent('GamePersistence')
        assert(persistence.transform and persistence.scriptState and not persistence.controllerState)
        persistence.scriptState = false
        assert(e:GetComponent('GamePersistence').scriptState)
        e:SetComponent('GamePersistence', {scriptState=false})
        assert(not e:GetComponent('GamePersistence').scriptState)
        assert(e:GetComponent('GamePersistence').transform)
        assert(not pcall(function() e:SetComponent('GamePersistence', {scriptState=1, transform=false}) end))
        assert(e:GetComponent('GamePersistence').transform)
        assert(not pcall(function() e:SetComponent('PointLight', {color=Vector3.new(0/0,1,1)}) end))
        assert(not pcall(function() e:SetComponent('RigidBody', {motionType='Flying'}) end))
        assert(not pcall(function() e:SetComponent('Collider', {radius=-1, isTrigger=false}) end))
        assert(e:GetComponent('Collider').radius == 2 and e:GetComponent('Collider').isTrigger)
        e:SetActiveCamera(); assert(e:IsActiveCamera())
        assert(not pcall(function() e:AddComponent('Camera') end))
        assert(not pcall(function() e:HasComponent('Unknown') end))
    )");
}

TEST_F(ComponentBindingFixture, RemovalAndDestructionLeaveSnapshotsSafe) {
    Lua(R"(
        local e = Scene.Spawn('removal', {components={Camera={farPlane=200}}})
        snapshot = e:GetComponent('Camera')
        retained = e
        assert(e:RemoveComponent('Camera'))
        assert(e:HasComponent('Camera')) -- queue applies at explicit safe boundary
        snapshot.farPlane = 300
    )");
    runtime.FlushSceneCommands();
    Lua(R"(
        local e = Scene.FindByName('removal')
        assert(not e:HasComponent('Camera') and e:GetComponent('Camera') == nil)
        assert(not e:RemoveComponent('Camera'))
        assert(not pcall(function() e:SetComponent('Camera', {}) end))
        e:AddComponent('Camera')
    )");
    const auto raw = host.GetScene().FindFirstByName("removal");
    const auto stale = engine::LuaEntityHandle::FromScene(host.GetScene(), raw);
    host.GetScene().DestroyEntity(raw);
    EXPECT_FALSE(stale.IsValid());
    EXPECT_THROW((void)stale.GetWorldPosition(), std::runtime_error);
    Lua(R"(
        assert(snapshot.farPlane == 300)
        assert(not retained:IsValid())
        assert(not pcall(function() retained:GetComponent('Camera') end))
        assert(not pcall(function() retained:SetComponent('Camera', snapshot) end))
        assert(not pcall(function() retained:RemoveComponent('Camera') end))
        assert(not pcall(function() Scene.Destroy(retained) end))
    )");
}

TEST_F(ComponentBindingFixture, SpawnAttachesCatalogScriptAndScalarPropertiesAtSafeBoundary) {
    ScriptAsset();
    Lua(R"(
        local e = Scene.Spawn('scripted', {components={
            Script={script='game/spawn.lua', properties={speed=2.5, label='spawned', awake=true}},
            GamePersistence={transform=true, scriptState=true},
            PointLight={intensity=2}
        }})
        assert(e:HasComponent('PointLight'))
        assert(not e:HasComponent('Script'))
    )");
    runtime.FlushSceneCommands();
    const auto entity = host.GetScene().FindFirstByName("scripted");
    const auto* component = host.GetScene().TryGetComponent<engine::ScriptComponent>(entity);
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->script.path, "game/spawn.lua");
    EXPECT_EQ(component->properties.at("speed").Number(), 2.5);
    ASSERT_NE(host.GetScene().TryGetComponent<engine::GamePersistence>(entity), nullptr);
    EXPECT_TRUE(host.GetScene().TryGetComponent<engine::GamePersistence>(entity)->scriptState);
    Lua(R"(
        local e = Scene.FindByName('scripted')
        local s = e:GetComponent('Script')
        assert(s.properties.label == 'spawned' and s.properties.awake)
        s.properties.speed = 99
        assert(e:GetComponent('Script').properties.speed == 2.5)
        assert(not pcall(function() e:SetComponent('Script', {properties={bad={}}}) end))
        assert(not pcall(function() e:SetComponent('Script', {script='game/missing.lua'}) end))
        e:SetComponent('Script', {enabled=false, properties={speed=8}})
        assert(e:GetComponent('Script').enabled)
    )");
    runtime.FlushSceneCommands();
    Lua(R"(
        local e = Scene.FindByName('scripted')
        assert(not e:GetComponent('Script').enabled)
        assert(e:GetComponent('Script').properties.speed == 8)
        local bare = Scene.Spawn('bare')
        bare:AddComponent('GamePersistence', {scriptState=true})
        bare:AddComponent('Script', {script=e:GetComponent('Script').script})
        assert(not pcall(function()
            Scene.Spawn('invalid spawn', {components={Script={script='game/missing.lua'}}})
        end))
        assert(not Scene.FindByName('invalid spawn'):IsValid())
    )");
    runtime.FlushSceneCommands();
    EXPECT_TRUE(host.GetScene().HasComponent<engine::ScriptComponent>(host.GetScene().FindFirstByName("bare")));
    Lua(R"(
        local e = Scene.FindByName('bare')
        assert(e:GetComponent('GamePersistence').scriptState)
        e:SetComponent('Script', {properties={generation=2}, enabled=false})
        assert(e:GetComponent('Script').enabled)
    )");
    runtime.FlushSceneCommands();
    Lua(R"(
        local e = Scene.FindByName('bare')
        assert(e:GetComponent('GamePersistence').scriptState)
        assert(not e:GetComponent('Script').enabled)
        assert(e:GetComponent('Script').properties.generation == 2)
    )");
}

TEST_F(ComponentBindingFixture, QueuedScriptChangesSurviveCallingInstanceLifetimeButNotEntityLifetime) {
    ScriptAsset();
    ASSERT_TRUE(engine::CreateNewFile(root / "caller.lua", "return {owner=entity}", error)) << error;
    const auto owner = host.GetScene().CreateEntity("caller");
    auto createCaller = [&]() {
        engine::ScriptInstanceDesc desc;
        desc.source = root / "caller.lua";
        desc.assetId = engine::AssetUuid(engine::NewUuid());
        desc.owner = engine::LuaEntityHandle::FromScene(host.GetScene(), owner);
        desc.autoloadName = "Caller";
        engine::InstanceId instance;
        EXPECT_TRUE(runtime.CreateInstance(desc, instance, error)) << error;
        EXPECT_TRUE(runtime.StartInstance(instance, error)) << error;
        return instance;
    };
    auto caller = createCaller();
    Lua(R"(
        local e = Autoloads.Caller.owner
        e:AddComponent('GamePersistence', {scriptState=true})
        e:AddComponent('Script', {script='game/spawn.lua', properties={generation=1}})
        assert(not e:HasComponent('Script'))
    )");
    ASSERT_TRUE(runtime.DestroyInstance(caller, error)) << error;
    runtime.FlushSceneCommands();
    ASSERT_TRUE(host.GetScene().HasComponent<engine::ScriptComponent>(owner));
    caller = createCaller();
    Lua("Autoloads.Caller.owner:SetComponent('Script', {properties={generation=2}})");
    ASSERT_TRUE(runtime.DestroyInstance(caller, error)) << error;
    runtime.FlushSceneCommands();
    ASSERT_NE(host.GetScene().TryGetComponent<engine::ScriptComponent>(owner), nullptr);
    EXPECT_EQ(host.GetScene().TryGetComponent<engine::ScriptComponent>(owner)->properties.at("generation").Number(), 2);
    Lua("Scene.FindByName('caller'):SetComponent('Script', {properties={generation=3}})");
    host.GetScene().DestroyEntity(owner);
    const auto replacement = host.GetScene().CreateEntity("replacement");
    runtime.FlushSceneCommands();
    EXPECT_FALSE(host.GetScene().HasComponent<engine::ScriptComponent>(replacement));
}

TEST_F(ComponentBindingFixture, WorldTransformsDirectionsAndNilUnparenting) {
    Lua(R"(
        local p = Scene.Spawn('parent', {position=Vector3.new(10,0,0), scale=Vector3.new(2,2,2), rotation=Quaternion.new(0,0.70710678,0,0.70710678)})
        local e = Scene.Spawn('child', {parent=p, position=Vector3.new(0,0,-1)})
        local w = e:GetWorldPosition()
        assert(math.abs(w.x-8) < 0.001 and math.abs(w.z) < 0.001)
        e:SetWorldPosition(Vector3.new(4,5,6))
        assert(math.abs(e:GetWorldPosition().y-5) < 0.001)
        e:SetWorldScale(Vector3.new(3,3,3))
        e:SetWorldRotation(Quaternion.new(0,0,0,1e30))
        assert(math.abs(e:Forward().z+1) < 0.001)
        assert(math.abs(e:Right().x-1) < 0.001 and math.abs(e:Up().y-1) < 0.001)
        assert(e:SetParentKeepingWorld(nil))
        assert(not e:GetParent():IsValid())
        assert(math.abs(e:GetPosition().x-4) < 0.001)
        assert(e:SetParent(p)); local localPosition = e:GetPosition()
        assert(e:SetParent(nil))
        assert(e:GetPosition().x == localPosition.x)
        assert(not e:SetParent(Scene.FindByName('missing')))
        assert(e == Scene.FindByName('child'))
    )");
}

TEST_F(ComponentBindingFixture, SingularAndShearedWorldEditsAreTransactional) {
    Lua(R"(
        local p = Scene.Spawn('singular', {scale=Vector3.new(0,1,1)})
        local e = Scene.Spawn('child', {parent=p})
        assert(not pcall(function() e:SetWorldPosition(Vector3.new(1,2,3)) end))
        assert(e:GetPosition().x == 0)
        p:SetScale(Vector3.new(2,1,1))
        e:SetRotation(Quaternion.new(0,0,0.38268343,0.92387953))
        assert(not pcall(function() e:SetWorldScale(Vector3.new(1,1,1)) end))
        assert(not pcall(function() e:SetParentKeepingWorld(nil) end))
        assert(e:GetParent() == p)
        assert(not pcall(function() e:SetRotation(Quaternion.new(0,0,0,0)) end))
    )");
}

TEST_F(ComponentBindingFixture, EulerAnglesAreInspectorDegrees) {
    Lua(R"(
        local e = Scene.Spawn('turned')
        e:SetEulerAngles(Vector3.new(10, 20, 30))
        local a = e:GetEulerAngles()
        assert(math.abs(a.x - 10) < 1e-3 and math.abs(a.y - 20) < 1e-3 and math.abs(a.z - 30) < 1e-3)
        e:SetEulerAngles(Vector3.new(0, 0, 90))
        assert((e:GetRotation():Rotate(Vector3.new(1, 0, 0)) - Vector3.new(0, 1, 0)):Length() < 1e-5)
        assert(not pcall(function() e:SetEulerAngles(Vector3.new(0/0, 0, 0)) end))
        assert(not pcall(function() e:SetEulerAngles(0, 0, 90) end))
    )");
}

TEST_F(ComponentBindingFixture, DestroyInvalidAndForeignHandlesRaiseCatchableErrors) {
    Lua(R"(
        assert(not pcall(function() Scene.Destroy(Scene.FindByName('missing')) end))
        local e = Scene.Spawn('destroyed')
        Scene.Destroy(e)
        assert(e:IsValid())
    )");
    runtime.FlushSceneCommands();
    Lua("assert(not Scene.FindByName('destroyed'):IsValid())");
    engine::Scene foreign;
    const auto foreignEntity = foreign.CreateEntity("foreign");
    ASSERT_TRUE(engine::CreateNewFile(root / "foreign.lua", "return {owner=entity}", error));
    engine::ScriptInstanceDesc desc;
    desc.source = root / "foreign.lua";
    desc.assetId = engine::AssetUuid(engine::NewUuid());
    desc.autoloadName = "Foreign";
    desc.owner = engine::LuaEntityHandle::FromScene(foreign, foreignEntity);
    engine::InstanceId instance;
    ASSERT_TRUE(runtime.CreateInstance(desc, instance, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(instance, error)) << error;
    Lua(R"(
        assert(not pcall(function() Scene.Destroy(Autoloads.Foreign.owner) end))
        assert(not pcall(function() Autoloads.Foreign.owner:AddComponent('Camera') end))
    )");
    EXPECT_TRUE(foreign.IsValid(foreignEntity));
    ASSERT_TRUE(runtime.DestroyInstance(instance, error)) << error;
    // The C++ handle itself also rechecks lifetime on each operation.
    engine::LuaEntityHandle stale;
    {
        engine::Scene temporary;
        stale = engine::LuaEntityHandle::FromScene(temporary, temporary.CreateEntity());
    }
    EXPECT_FALSE(stale.IsValid());
    EXPECT_THROW(stale.SetPosition({1,2,3}), std::runtime_error);
}
