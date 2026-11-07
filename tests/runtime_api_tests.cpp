#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/Log.hpp>
#include <engine/engine.hpp>
#include <spdlog/sinks/callback_sink.h>
#include <gtest/gtest.h>

namespace {
struct ApiRecordingController final : engine::Controller {
    explicit ApiRecordingController(std::shared_ptr<std::vector<std::string>> eventLog) : events(std::move(eventLog)) {}
    void OnCreate() override { events->push_back("create"); }
    void OnDestroy() override { events->push_back("destroy"); }
    std::shared_ptr<std::vector<std::string>> events;
};
struct ApiSceneObserverScope {
    engine::Scene& scene;
    std::size_t token;
    ~ApiSceneObserverScope() { scene.RemoveLifecycleObserver(token); }
};
struct ApiErrorCapture {
    unsigned receiverFailures = 0;
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink =
        std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& message) {
            const std::string_view text(message.payload.data(), message.payload.size());
            if (message.level == spdlog::level::err && text.find("receiver failure") != std::string_view::npos)
                ++receiverFailures;
        });
    ApiErrorCapture() { engine::log::Sinks()->add_sink(sink); }
    ~ApiErrorCapture() { engine::log::Sinks()->remove_sink(sink); }
};
struct RuntimeApiFixture : testing::Test {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-runtime-api-" + engine::NewUuid());
    engine::Engine host;
    engine::Scene& scene = host.GetScene();
    engine::LuaRuntime runtime;
    std::string error;
    unsigned nextFile = 0;

    void SetUp() override {
        std::filesystem::create_directories(root);
        ASSERT_TRUE(runtime.Initialize(root, host, error)) << error;
    }
    void TearDown() override {
        runtime.Shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    engine::InstanceId Create(std::string_view source, std::string name = "Test", entt::entity owner = entt::null,
                              engine::AssetUuid asset = {}) {
        engine::ScriptInstanceDesc desc;
        desc.source = root / (std::to_string(nextFile++) + ".lua");
        desc.assetId = asset.IsValid() ? asset : engine::AssetUuid(engine::NewUuid());
        desc.autoloadName = std::move(name);
        if (owner != entt::null) {
            desc.owner = engine::LuaEntityHandle::FromScene(scene, owner);
            desc.entityId = scene.GetId(owner).ToString();
            auto* component = scene.TryGetComponent<engine::ScriptComponent>(owner);
            if (!component) component = scene.AddComponent<engine::ScriptComponent>(owner);
            component->script.id = desc.assetId;
            component->enabled = true;
        }
        EXPECT_TRUE(engine::CreateNewFile(desc.source, source, error)) << error;
        engine::InstanceId instance;
        EXPECT_TRUE(runtime.CreateInstance(desc, instance, error)) << error;
        return instance;
    }
    void Lua(std::string_view source) {
        EXPECT_TRUE(runtime.ExecuteString(source, "runtime API assertion", error)) << error;
    }
};
}

TEST_F(RuntimeApiFixture, SelfDestroyWaitsUntilExplicitSafeBoundary) {
    const auto owner = scene.CreateEntity("owner");
    const auto instance = Create(R"(
        return {
            OnStart = function(self)
                Scene.Destroy(entity)
                assert(entity:IsValid())
                self.completed = true
            end,
            OnDestroy = function(self) assert(self.completed) end
        }
    )", "Test", owner);
    const auto token = scene.AddLifecycleObserver([&](engine::Scene::LifecycleEvent event, entt::entity entity, entt::id_type) {
        if (event == engine::Scene::LifecycleEvent::EntityDestroying && entity == owner)
            EXPECT_TRUE(runtime.DestroyInstance(instance, error)) << error;
    });
    const ApiSceneObserverScope observerScope{scene, token};
    ASSERT_TRUE(runtime.StartInstance(instance, error)) << error;
    EXPECT_TRUE(scene.IsValid(owner));
    runtime.FlushSceneCommands();
    EXPECT_FALSE(scene.IsValid(owner));
}

TEST_F(RuntimeApiFixture, ChunkDestructionAndNestedExternalScopesRemainDeferred) {
    const auto owner = scene.CreateEntity();
    const auto instance = Create("return {owner = entity}", "Test", owner);
    ASSERT_TRUE(runtime.StartInstance(instance, error));
    scene.BeginExternalCallback();
    scene.BeginExternalCallback();
    Lua("Scene.Destroy(Autoloads.Test.owner); assert(Autoloads.Test.owner:IsValid())");
    runtime.FlushSceneCommands();
    EXPECT_TRUE(scene.IsValid(owner));
    scene.EndExternalCallback();
    scene.EndExternalCallback();
    EXPECT_TRUE(scene.IsValid(owner));
    runtime.FlushSceneCommands();
    EXPECT_FALSE(scene.IsValid(owner));
}

TEST_F(RuntimeApiFixture, DestroyAndAttachControllerUseOneSceneFifoInLuaCallOrder) {
    const auto events = std::make_shared<std::vector<std::string>>();
    engine::ControllerRegistration registration;
    registration.create = [events] { return std::make_unique<ApiRecordingController>(events); };
    ASSERT_TRUE(scene.RegisterController("RuntimeApiRecorder", std::move(registration)));
    for (const bool destroyFirst : {true, false}) {
        SCOPED_TRACE(destroyFirst ? "destroy then attach" : "attach then destroy");
        events->clear();
        const auto owner = scene.CreateEntity();
        const auto instance = Create("return {owner=entity}", "Test", owner);
        ASSERT_TRUE(runtime.StartInstance(instance, error)) << error;
        Lua(destroyFirst ? R"(
            local e = Autoloads.Test.owner
            Scene.Destroy(e)
            Scene.AttachController(e, 'RuntimeApiRecorder')
            assert(e:IsValid())
        )" : R"(
            local e = Autoloads.Test.owner
            Scene.AttachController(e, 'RuntimeApiRecorder')
            Scene.Destroy(e)
            assert(e:IsValid())
        )");
        EXPECT_TRUE(events->empty());
        EXPECT_TRUE(scene.IsValid(owner));
        runtime.FlushSceneCommands();
        EXPECT_FALSE(scene.IsValid(owner));
        if (destroyFirst) EXPECT_TRUE(events->empty());
        else EXPECT_EQ(*events, (std::vector<std::string>{"create", "destroy"}));
        ASSERT_TRUE(runtime.DestroyInstance(instance, error)) << error;
    }
}

TEST_F(RuntimeApiFixture, ScriptRemovalAndDestroyRespectLuaCallOrder) {
    for (const bool destroyFirst : {true, false}) {
        SCOPED_TRACE(destroyFirst ? "destroy then remove" : "remove then destroy");
        const auto owner = scene.CreateEntity();
        const auto instance = Create("return {owner=entity}", "Test", owner);
        ASSERT_TRUE(runtime.StartInstance(instance, error)) << error;
        std::vector<std::string> events;
        const auto observer = scene.AddLifecycleObserver([&](engine::Scene::LifecycleEvent event, entt::entity entity, entt::id_type component) {
            if (entity != owner) return;
            if (event == engine::Scene::LifecycleEvent::EntityDestroying) events.push_back("destroy");
            else if (component == entt::type_hash<engine::ScriptComponent>::value()) events.push_back("remove");
        });
        const ApiSceneObserverScope observerScope{scene, observer};
        Lua(destroyFirst ? R"(
            local e = Autoloads.Test.owner
            Scene.Destroy(e)
            assert(e:RemoveComponent('Script'))
        )" : R"(
            local e = Autoloads.Test.owner
            assert(e:RemoveComponent('Script'))
            Scene.Destroy(e)
        )");
        EXPECT_TRUE(events.empty());
        runtime.FlushSceneCommands();
        EXPECT_FALSE(scene.IsValid(owner));
        if (destroyFirst) EXPECT_EQ(events, (std::vector<std::string>{"destroy"}));
        else EXPECT_EQ(events, (std::vector<std::string>{"remove", "destroy"}));
        ASSERT_TRUE(runtime.DestroyInstance(instance, error)) << error;
    }
}

TEST_F(RuntimeApiFixture, EntityMethodsReturnMultipleValuesAndRejectInvalidLifecycleAndReentrancy) {
    const auto owner = scene.CreateEntity();
    const auto instance = Create(R"(
        return {
            owner = entity, count = 4,
            Add = function(self, n) self.count = self.count + n; return self.count, nil, 'tail' end,
            Recurse = function(self) local ok, why = entity:CallScript('Recurse'); assert(not ok and type(why) == 'string'); return 7 end
        }
    )", "Test", owner);
    const auto handle = engine::LuaEntityHandle::FromScene(scene, owner);
    EXPECT_FALSE(runtime.HasScript(handle));
    EXPECT_FALSE(runtime.HasScript(engine::LuaEntityHandle{}));
    ASSERT_TRUE(runtime.StartInstance(instance, error));
    EXPECT_TRUE(runtime.HasScript(handle));
    EXPECT_TRUE(runtime.HasScriptMethod(handle, "Add"));
    EXPECT_FALSE(runtime.HasScriptMethod(handle, "OnDestroy"));
    Lua(R"(
        local e = Autoloads.Test.owner
        local ok, value, middle, tail = e:CallScript('Add', 3)
        assert(ok and value == 7 and middle == nil and tail == 'tail')
        assert(select('#', e:CallScript('Add', 0)) == 4)
        local again, seven = e:CallScript('Recurse'); assert(again and seven == 7)
        assert(not e:CallScript('OnStart'))
        assert(not e:CallScript('Missing'))
        local invalid = e:GetParent()
        assert(not invalid:HasScript() and not invalid:HasScriptMethod('Add'))
        local valid, why = invalid:CallScript('Add', 1)
        assert(not valid and type(why) == 'string')
    )");
    ASSERT_TRUE(runtime.DestroyInstance(instance, error));
    EXPECT_FALSE(runtime.HasScript(handle));
}

TEST_F(RuntimeApiFixture, MethodFailureFaultsOnlyReceiver) {
    ApiErrorCapture errors;
    const auto owner = scene.CreateEntity();
    const auto receiver = Create("return {owner=entity, Explode=function(self) error('receiver failure') end}", "Receiver", owner);
    const auto sender = Create(R"(
        return {OnUpdate=function(self, dt)
            local ok, why = Autoloads.Receiver.owner:CallScript('Explode')
            assert(not ok and type(why) == 'string')
            self.ticks = (self.ticks or 0) + 1
        end}
    )", "Sender");
    ASSERT_TRUE(runtime.StartInstance(receiver, error));
    ASSERT_TRUE(runtime.StartInstance(sender, error));
    EXPECT_TRUE(runtime.UpdateInstance(sender, .1, error)) << error;
    EXPECT_FALSE(runtime.HasScript(engine::LuaEntityHandle::FromScene(scene, owner)));
    EXPECT_TRUE(runtime.UpdateInstance(sender, .1, error)) << error;
    Lua("assert(Autoloads.Sender.ticks == 2)");
    EXPECT_EQ(errors.receiverFailures, 1u);
}

TEST_F(RuntimeApiFixture, CommunicationImmediatelyRejectsDisabledChangedAndRemovedAttachments) {
    const auto owner = scene.CreateEntity();
    const auto instance = Create("return {owner=entity, Ping=function(self) return 5 end}", "Test", owner);
    ASSERT_TRUE(runtime.StartInstance(instance, error));
    const auto handle = engine::LuaEntityHandle::FromScene(scene, owner);
    auto* component = scene.TryGetComponent<engine::ScriptComponent>(owner);
    const auto asset = component->script.id;
    component->enabled = false;
    EXPECT_FALSE(runtime.HasScript(handle));
    EXPECT_FALSE(runtime.HasScriptMethod(handle, "Ping"));
    Lua("assert(not Autoloads.Test.owner:CallScript('Ping'))");
    component->enabled = true;
    component->script.id = engine::AssetUuid(engine::NewUuid());
    EXPECT_FALSE(runtime.HasScript(handle));
    Lua("assert(not Autoloads.Test.owner:CallScript('Ping'))");
    component->script.id = asset;
    EXPECT_TRUE(runtime.HasScript(handle));
    ASSERT_TRUE(scene.RemoveComponent<engine::ScriptComponent>(owner));
    EXPECT_FALSE(runtime.HasScript(handle));
    Lua("assert(not Autoloads.Test.owner:CallScript('Ping'))");
}

TEST_F(RuntimeApiFixture, DeferredCommandsDrainFifoIncludingCommandsAppendedByDestroy) {
    const auto owner = scene.CreateEntity();
    ASSERT_NE(scene.AddComponent<engine::GamePersistence>(owner), nullptr);
    ASSERT_NE(scene.AddComponent<engine::Camera>(owner), nullptr);
    const auto instance = Create(R"(
        return {
            OnStart=function(self)
                assert(entity:RemoveComponent('Script'))
                assert(entity:RemoveComponent('GamePersistence'))
                assert(entity:HasComponent('Script') and entity:HasComponent('GamePersistence'))
                self.completed=true
            end,
            OnDestroy=function(self)
                assert(self.completed and entity:HasComponent('GamePersistence'))
                assert(entity:RemoveComponent('Camera'))
            end
        }
    )", "Test", owner);
    std::vector<entt::id_type> removed;
    const auto observer = scene.AddLifecycleObserver([&](engine::Scene::LifecycleEvent event, entt::entity entity, entt::id_type component) {
        if (event != engine::Scene::LifecycleEvent::ComponentRemoving || entity != owner) return;
        removed.push_back(component);
        if (component == entt::type_hash<engine::ScriptComponent>::value())
            EXPECT_TRUE(runtime.DestroyInstance(instance, error)) << error;
    });
    const ApiSceneObserverScope observerScope{scene, observer};
    ASSERT_TRUE(runtime.StartInstance(instance, error)) << error;
    EXPECT_TRUE(removed.empty());
    runtime.FlushSceneCommands();
    EXPECT_EQ(removed, (std::vector<entt::id_type>{entt::type_hash<engine::ScriptComponent>::value(),
        entt::type_hash<engine::GamePersistence>::value(), entt::type_hash<engine::Camera>::value()}));
    EXPECT_TRUE(scene.IsValid(owner));
}

TEST_F(RuntimeApiFixture, PersistenceErrorsDoNotFaultAndRestoreRunsBeforeStart) {
    const auto instance = Create(R"(
        return {
            SaveVersion=3,
            OnSaveGame=function(self) if not self.loaded then error('save rejected') end; return {value=self.value, empty={}} end,
            OnLoadGame=function(self, data, version) assert(version >= 1 and version <= 3); if data.fail then error('load rejected') end; self.value=data.value; self.loaded=true; self.savedVersion=version end,
            OnStart=function(self) assert(self.loaded and self.value == 42) end,
            OnUpdate=function(self, dt) self.updated=true end
        }
    )");
    int version = 0;
    engine::StateValue data;
    EXPECT_TRUE(runtime.HasPersistenceHooks(instance));
    EXPECT_FALSE(runtime.CaptureSaveState(instance, version, data, error));
    engine::StateValue failed; failed["fail"] = true;
    EXPECT_FALSE(runtime.RestoreSaveState(instance, 3, failed, error));
    engine::StateValue saved; saved["value"] = 42;
    EXPECT_FALSE(runtime.RestoreSaveState(instance, 0, saved, error));
    EXPECT_FALSE(runtime.RestoreSaveState(instance, 4, saved, error));
    ASSERT_TRUE(runtime.RestoreSaveState(instance, 2, saved, error)) << error;
    Lua("assert(Autoloads.Test.savedVersion == 2)");
    ASSERT_TRUE(runtime.StartInstance(instance, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(instance, .1, error)) << error;
    ASSERT_TRUE(runtime.CaptureSaveState(instance, version, data, error)) << error;
    EXPECT_EQ(version, 3);
    EXPECT_EQ(data.At("value").Number(), 42);
    EXPECT_TRUE(std::holds_alternative<engine::StateValue::Object>(data.At("empty").value));
}

TEST_F(RuntimeApiFixture, RejectsCyclesSparseMixedAndNonFiniteStateWithoutFaulting) {
    const auto instance = Create(R"(
        return {
            mode=1,
            OnLoadGame=function(self, data, version) self.mode=data.mode end,
            OnSaveGame=function(self)
                if self.mode == 1 then local t={}; t.self=t; return t end
                if self.mode == 2 then return {[2]='sparse'} end
                if self.mode == 3 then return {a=true, [1]=false} end
                if self.mode == 4 then return {number=math.huge} end
                return {values={1,2,3}}
            end
        }
    )");
    for (int mode = 1; mode <= 5; ++mode) {
        engine::StateValue input; input["mode"] = mode;
        ASSERT_TRUE(runtime.RestoreSaveState(instance, 1, input, error)) << error;
        int version = 0;
        engine::StateValue output;
        EXPECT_EQ(runtime.CaptureSaveState(instance, version, output, error), mode == 5) << error;
    }
}

TEST_F(RuntimeApiFixture, PersistenceDeclarationRequiresPairedFunctionsAndPositiveIntegerVersion) {
    for (const std::string_view source : {
        "return {OnSaveGame=function(self) return {} end}",
        "return {OnSaveGame=true, OnLoadGame=function(self) end}",
        "return {SaveVersion=0}", "return {SaveVersion=1.5}", "return {SaveVersion='1'}"}) {
        engine::ScriptInstanceDesc desc;
        desc.source = root / (std::to_string(nextFile++) + ".lua");
        desc.assetId = engine::AssetUuid(engine::NewUuid());
        ASSERT_TRUE(engine::CreateNewFile(desc.source, source, error)) << error;
        engine::InstanceId instance;
        EXPECT_FALSE(runtime.CreateInstance(desc, instance, error));
        EXPECT_FALSE(error.empty());
    }
}

TEST_F(RuntimeApiFixture, AssetVersionPreflightChecksEveryKnownInstanceAndAllowsUnknownAssets) {
    const engine::AssetUuid asset(engine::NewUuid());
    Create("return {SaveVersion=3, OnSaveGame=function() return {} end, OnLoadGame=function() end}", "First", entt::null, asset);
    Create("return {SaveVersion=2, OnSaveGame=function() return {} end, OnLoadGame=function() end}", "Second", entt::null, asset);
    EXPECT_TRUE(runtime.ValidateAssetSaveVersion(asset, 1, error)) << error;
    EXPECT_TRUE(runtime.ValidateAssetSaveVersion(asset, 2, error)) << error;
    EXPECT_FALSE(runtime.ValidateAssetSaveVersion(asset, 3, error));
    EXPECT_FALSE(runtime.ValidateAssetSaveVersion(asset, 0, error));
    EXPECT_TRUE(runtime.ValidateAssetSaveVersion(engine::AssetUuid(engine::NewUuid()), 100, error));
    Create("return {}", "MissingHooks", entt::null, asset);
    EXPECT_FALSE(runtime.ValidateAssetSaveVersion(asset, 1, error));
}

TEST_F(RuntimeApiFixture, LiveSessionPreflightRejectsKnownFutureVersionWithoutStoppingScripts) {
    engine::ScriptSubsystem scripts;
    ASSERT_TRUE(scripts.Initialize());
    ASSERT_TRUE(scripts.BeginPlay(root, error)) << error;
    engine::ScriptInstanceDesc desc;
    desc.source = root / "preflight.lua";
    desc.assetId = engine::AssetUuid(engine::NewUuid());
    ASSERT_TRUE(engine::CreateNewFile(desc.source,
        "return {SaveVersion=2, OnSaveGame=function() return {} end, OnLoadGame=function() end}", error));
    engine::InstanceId instance;
    ASSERT_TRUE(scripts.GetRuntime().CreateInstance(desc, instance, error));
    ASSERT_TRUE(scripts.GetRuntime().StartInstance(instance, error));
    engine::GameDocument game;
    engine::GameEntityState entity;
    entity.script = engine::GameScriptState{desc.assetId, 3, engine::StateValue{}};
    game.entities.push_back(entity);
    EXPECT_FALSE(scripts.PreflightGameState(game, error));
    EXPECT_TRUE(scripts.IsPlaying());
    EXPECT_TRUE(scripts.GetRuntime().UpdateInstance(instance, .1, error)) << error;
    game.entities.front().script->version = 1;
    EXPECT_TRUE(scripts.PreflightGameState(game, error)) << error;
    game.autoloads.emplace("NotConfigured", *game.entities.front().script);
    EXPECT_FALSE(scripts.PreflightGameState(game, error));
    EXPECT_TRUE(scripts.IsPlaying());
    scripts.EndPlay();
}

TEST_F(RuntimeApiFixture, FixedUpdatesUseTheirOwnCallbackAndTimeStep) {
    const auto instance = Create(R"(
        return {
            fixed=0, updates=0,
            OnFixedUpdate=function(self, dt) assert(dt == 0.02); self.fixed=self.fixed+1 end,
            OnUpdate=function(self, dt) self.updates=self.updates+1 end
        }
    )");
    EXPECT_FALSE(runtime.FixedUpdateInstance(instance, .02, error));
    ASSERT_TRUE(runtime.StartInstance(instance, error));
    ASSERT_TRUE(runtime.FixedUpdateInstance(instance, .02, error)) << error;
    ASSERT_TRUE(runtime.FixedUpdateInstance(instance, .02, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(instance, .5, error)) << error;
    Lua("assert(Autoloads.Test.fixed == 2 and Autoloads.Test.updates == 1)");
}

TEST_F(RuntimeApiFixture, EngineLessSubsystemCompatibilityAndDiscardedSceneCommands) {
    engine::ScriptSubsystem scripts;
    ASSERT_TRUE(scripts.Initialize());
    ASSERT_TRUE(scripts.BeginPlay(root, error)) << error;
    scripts.FixedUpdate(engine::FixedUpdateContext{});
    scripts.EndPlay();
    const auto owner = scene.CreateEntity();
    ASSERT_NE(scene.AddComponent<engine::GamePersistence>(owner), nullptr);
    const auto instance = Create(R"(return {OnStart=function(self)
        assert(entity:RemoveComponent('GamePersistence'))
        Scene.Destroy(entity)
    end})", "Test", owner);
    ASSERT_TRUE(runtime.StartInstance(instance, error));
    runtime.DiscardSceneCommands();
    runtime.FlushSceneCommands();
    EXPECT_TRUE(scene.IsValid(owner));
    EXPECT_TRUE(scene.HasComponent<engine::GamePersistence>(owner));
}

TEST_F(RuntimeApiFixture, PreparedSubsystemRestoresBeforeStartingAndComponentRemovalIsDeferred) {
    engine::ScriptSubsystem scripts;
    ASSERT_TRUE(scripts.Initialize());
    ASSERT_TRUE(scripts.BeginPlay(root, std::span<const engine::AutoloadDefinition>{}, error, true)) << error;
    ASSERT_TRUE(scripts.PrepareSceneScripts(scene, error)) << error;
    engine::GameDocument game;
    EXPECT_FALSE(scripts.CaptureGameState(game, error));
    ASSERT_TRUE(scripts.ValidateGameState(game, error)) << error;
    ASSERT_TRUE(scripts.RestoreGameState(game, error)) << error;
    ASSERT_TRUE(scripts.StartPreparedScripts(error)) << error;
    EXPECT_FALSE(scripts.ValidateGameState(game, error));
    scripts.EndPlay();

    const auto owner = scene.CreateEntity();
    ASSERT_NE(scene.AddComponent<engine::ScriptComponent>(owner), nullptr);
    scene.BeginExternalCallback();
    EXPECT_TRUE(scene.RemoveComponent<engine::ScriptComponent>(owner));
    EXPECT_TRUE(scene.HasComponent<engine::ScriptComponent>(owner));
    scene.EndExternalCallback();
    EXPECT_TRUE(scene.HasComponent<engine::ScriptComponent>(owner));
    scene.FlushExternalCommands();
    EXPECT_FALSE(scene.HasComponent<engine::ScriptComponent>(owner));
}
