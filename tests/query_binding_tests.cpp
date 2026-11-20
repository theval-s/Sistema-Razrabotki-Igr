#include <gtest/gtest.h>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/SceneQueries.hpp>
#include <engine/scripting/LuaBindings.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <engine/scripting/LuaGameplayBindings.hpp>

namespace {
using namespace engine;
using DirectX::SimpleMath::Vector3;

struct QueryBindingTests : testing::Test {
    Engine host{EngineConfig{.taskThreads = 1}};
    LuaBindingsContext context{.engine = &host};
    sol::state lua;

    void SetUp() override {
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string);
        lua.new_usertype<Vector3>("Vector3", sol::constructors<Vector3(), Vector3(float, float, float)>(),
            "x", &Vector3::x, "y", &Vector3::y, "z", &Vector3::z);
        lua.create_named_table("Scene");
        RegisterLuaBindings(lua, context);
        RegisterGameplayBindings(lua, context);
    }

    void Lua(const char* source) {
        const auto result = lua.safe_script(source, sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            ADD_FAILURE() << error.what();
        }
    }

    entt::entity Box(const char* name, Vector3 position, bool trigger = false, entt::entity parent = entt::null) {
        auto& scene = host.GetScene();
        const auto entity = scene.CreateEntity(name, parent);
        scene.GetTransform(entity).position = position;
        scene.AddComponent<RigidBody>(entity);
        scene.AddComponent<Collider>(entity, Collider{.isTrigger = trigger});
        lua[name] = LuaEntityHandle::FromScene(scene, entity);
        return entity;
    }
};

TEST_F(QueryBindingTests, PhysicsQueriesReturnHitsMissesAndFilteredEntityHandles) {
    host.AddSubsystem<PhysicsSubsystem>();
    ASSERT_TRUE(host.Initialize());
    const auto nearEntity = Box("nearEntity", {0, 0, -3});
    Box("farEntity", {0, 0, -6});
    Box("trigger", {0, 0, -1}, true);
    Lua(R"(
        local o, d = Vector3.new(), Vector3.new(0, 0, -7)
        savedHit, err = Physics.Raycast(o, d, 10)
        assert(err == '' and savedHit.entity == nearEntity)
        assert(math.abs(savedHit.distance - 2.5) < 0.0001)
        assert((savedHit.point - Vector3.new(0, 0, -2.5)):Length() < 0.0001)
        assert((savedHit.normal - Vector3.new(0, 0, 1)):Length() < 0.0001)
        assert(savedHit.isTrigger == false)
        local hits, why = Physics.RaycastAll(o, d, 10)
        assert(why == '' and #hits == 2 and hits[1].entity == nearEntity and hits[2].entity == farEntity)
        local hit = Physics.Raycast(o, d, 10, {ignore = {nearEntity}})
        assert(hit.entity == farEntity)
        hit = Physics.Raycast(o, d, 10, {includeTriggers = true})
        assert(hit.entity == trigger and hit.isTrigger)
        hits = Physics.RaycastAll(o, d, 10, {includeTriggers = true})
        assert(#hits == 3 and hits[1].entity == trigger)
        local miss, error = Physics.Raycast(o, d, 0.1)
        assert(miss == nil and error == '')
        hits, error = Physics.RaycastAll(o, d, 0.1)
        assert(type(hits) == 'table' and #hits == 0 and error == '')
        hit, error = Physics.Linecast(o, Vector3.new(0, 0, -3))
        assert(hit.entity == nearEntity and error == '')
        hit, error = Physics.Linecast(o, o)
        assert(hit == nil and error == '')
        hit, error = Physics.Raycast(Vector3.new(0,0,-3), d, 1)
        assert(hit.entity == nearEntity and hit.distance == 0 and hit.normal == nil and error == '')
    )");
    host.GetScene().DestroyEntity(nearEntity);
    Lua("assert(not savedHit.entity:IsValid()); assert(savedHit.distance > 0)");
}

TEST_F(QueryBindingTests, DescendantFilteringAndExplicitSynchronizationWorkBeforeFixedSteps) {
    host.AddSubsystem<PhysicsSubsystem>();
    ASSERT_TRUE(host.Initialize());
    auto& scene = host.GetScene();
    const auto owner = scene.CreateEntity("owner");
    lua["owner"] = LuaEntityHandle::FromScene(scene, owner);
    Box("child", {0, 0, -1}, false, owner);
    const auto wall = Box("wall", {0, 0, -4});
    Lua(R"(
        o, d = Vector3.new(), Vector3.new(0,0,-1)
        local hit = Physics.Raycast(o, d, 10, {ignore = {owner}})
        assert(hit.entity == child)
        hit = Physics.Raycast(o, d, 10, {ignore = {owner}, ignoreDescendants = true})
        assert(hit.entity == wall)
    )");
    scene.GetTransform(wall).position = {10, 0, -4};
    Box("newWall", {0, 0, -7});
    Lua(R"(
        local options = {ignore = {owner}, ignoreDescendants = true}
        assert(Physics.Raycast(o, d, 10, options).entity == wall)
        local ok, error = Physics.SyncQueries()
        assert(ok and error == '')
        assert(Physics.Raycast(o, d, 10, options).entity == newWall)
    )");
}

TEST_F(QueryBindingTests, MouseScreenRayPicksBoundsWithoutPhysicsAndSupportsCustomViewport) {
    auto* input = host.AddSubsystem<InputSubsystem>();
    host.ResizeSurface(800, 600);
    auto& scene = host.GetScene();
    const auto camera = scene.CreateEntity("camera");
    scene.AddComponent<Camera>(camera);
    lua["camera"] = LuaEntityHandle::FromScene(scene, camera);
    const auto target = scene.CreateEntity("target");
    scene.GetTransform(target).position = {0, 0, -5};
    scene.AddComponent<Bounds>(target, Bounds{Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}});
    lua["target"] = LuaEntityHandle::FromScene(scene, target);
    // Input registration refreshes after the subsystem was added.
    Lua("Input.GetContext('Gameplay')");
    input->OnMousePosition(400, 300);
    input->Update(FrameContext{});
    Lua(R"(
        local ray, err = Scene.ScreenPointToRay(Input.MousePosition())
        assert(err == '' and math.abs(ray.origin.z + 0.1) < 0.0001)
        assert((ray.direction - Vector3.new(0,0,-1)):Length() < 0.0001)
        assert(math.abs(ray.maxDistance - 99.9) < 0.001)
        local hit, why = Scene.RaycastBounds(ray.origin, ray.direction, ray.maxDistance)
        assert(why == '' and hit.entity == target)
        assert(math.abs(hit.distance - 4.4) < 0.0001)
        local hits = Scene.RaycastBoundsAll(ray.origin, ray.direction, ray.maxDistance)
        assert(#hits == 1 and hits[1].entity == target)
        hit, why = Scene.RaycastBounds(ray.origin, ray.direction, ray.maxDistance, {ignore = {target}})
        assert(hit == nil and why == '')
        hits, why = Scene.RaycastBoundsAll(ray.origin, ray.direction, ray.maxDistance, {ignore = {target}})
        assert(#hits == 0 and why == '')
        local custom = Scene.ScreenPointToRay(Vector2.new(200,150), {
            camera = camera, viewportSize = Vector2.new(400,300)
        })
        assert((custom.direction - ray.direction):Length() < 0.0001)
        local physicsHit, physicsError = Physics.Raycast(ray.origin, ray.direction, ray.maxDistance)
        assert(physicsHit == nil and #physicsError > 0)
    )");
}

TEST_F(QueryBindingTests, InvalidQueriesReturnDiagnosticsIncludingForeignAndStaleHandles) {
    host.AddSubsystem<PhysicsSubsystem>();
    ASSERT_TRUE(host.Initialize());
    const auto removed = Box("removed", {0, 0, -3});
    host.GetScene().DestroyEntity(removed);
    Scene other;
    lua["foreign"] = LuaEntityHandle::FromScene(other, other.CreateEntity());
    Lua(R"(
        local o, d = Vector3.new(), Vector3.new(0,0,-1)
        local function rejected(value, error)
            assert(value == nil and type(error) == 'string' and #error > 0)
        end
        for _, cast in ipairs({Physics.Raycast, Physics.RaycastAll, Scene.RaycastBounds, Scene.RaycastBoundsAll}) do
            rejected(cast({}, d, 10))
            rejected(cast(o, d, '10'))
            rejected(cast(o, d, 0))
            rejected(cast(o, d, -1))
            rejected(cast(o, d, math.huge))
            rejected(cast(o, d, 1e300))
            rejected(cast(o, Vector3.new(), 10))
            rejected(cast(Vector3.new(0/0,0,0), d, 10))
            rejected(cast(o, Vector3.new(math.huge,0,0), 10))
            rejected(cast(o, d, 10, false))
            rejected(cast(o, d, 10, {unknown = true}))
            rejected(cast(o, d, 10, {ignoreDescendants = 1}))
            rejected(cast(o, d, 10, {ignore = {foreign}}))
            rejected(cast(o, d, 10, {ignore = {removed}}))
            rejected(cast(o, d, 10, {ignore = {[2] = foreign}}))
        end
        rejected(Physics.Raycast(o, d, 10, {includeTriggers = 'yes'}))
        rejected(Physics.Raycast(o, d, 10, {hitBackFaces = 1}))
        rejected(Scene.RaycastBounds(o, d, 10, {includeTriggers = true}))
        rejected(Physics.Linecast(o, nil))
        rejected(Physics.Linecast(o, Vector3.new(0/0,0,0)))
        rejected(Physics.Linecast(o, o, {ignore = {foreign}}))
        rejected(Scene.ScreenPointToRay(Vector2.new(1,1))) -- no camera/surface
        rejected(Scene.ScreenPointToRay({x=1,y=1}))
        rejected(Scene.ScreenPointToRay(Vector2.new(), {camera = foreign}))
        rejected(Scene.ScreenPointToRay(Vector2.new(), {viewportSize = {800,600}}))
    )");
}

TEST_F(QueryBindingTests, ScreenRayRejectsOutsideViewportAndInvalidCamera) {
    host.ResizeSurface(800, 600);
    auto& scene = host.GetScene();
    const auto camera = scene.CreateEntity("camera");
    scene.AddComponent<Camera>(camera);
    const auto nonCamera = scene.CreateEntity("nonCamera");
    lua["nonCamera"] = LuaEntityHandle::FromScene(scene, nonCamera);
    Lua(R"(
        local function rejected(value, error) assert(value == nil and #error > 0) end
        rejected(Scene.ScreenPointToRay(Vector2.new(-1, 10)))
        rejected(Scene.ScreenPointToRay(Vector2.new(800, 10)))
        rejected(Scene.ScreenPointToRay(Vector2.new(10, 600)))
        rejected(Scene.ScreenPointToRay(Vector2.new(0/0, 10)))
        rejected(Scene.ScreenPointToRay(Vector2.new(), {camera = nonCamera}))
        rejected(Scene.ScreenPointToRay(Vector2.new(), {viewportSize = Vector2.new()}))
        rejected(Scene.ScreenPointToRay(Vector2.new(), {viewPortSize = Vector2.new(800,600)}))
    )");
}

TEST_F(QueryBindingTests, UnavailableSceneOrPhysicsReturnsErrors) {
    Lua(R"(
        local hit, err = Physics.Linecast(Vector3.new(), Vector3.new())
        assert(hit == nil and #err > 0)
        local ok, error = Physics.SyncQueries()
        assert(ok == false and #error > 0)
    )");
    context.engine = nullptr;
    Lua(R"(
        local hit, error = Scene.RaycastBounds(Vector3.new(), Vector3.new(0,0,-1), 10)
        assert(hit == nil and #error > 0)
        local ray, why = Scene.ScreenPointToRay(Vector2.new())
        assert(ray == nil and #why > 0)
    )");
}
} // namespace
