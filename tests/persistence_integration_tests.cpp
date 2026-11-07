#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/Log.hpp>
#include <engine/scene/GameDocument.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr entt::entity NullEntity = entt::null;

struct GameRootScope {
    std::filesystem::path previous = engine::AssetPathResolver::GetGameRoot();
    explicit GameRootScope(const std::filesystem::path& root) {
        engine::AssetPathResolver::SetGameRoot(root);
    }
    ~GameRootScope() { engine::AssetPathResolver::SetGameRoot(previous); }
};

struct PersistenceFixture {
    // Keep even the default root.parent_path()/.sri/saves inside this test's sandbox.
    std::filesystem::path sandbox = std::filesystem::temp_directory_path() /
        ("sri-persistence-integration-" + engine::NewUuid());
    std::filesystem::path root = sandbox / "game";
    GameRootScope gameRoot{root};
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::ScriptSubsystem* scripts = host.AddSubsystem<engine::ScriptSubsystem>();
    std::vector<engine::AutoloadDefinition> autoloads;
    std::string error;

    PersistenceFixture() { std::filesystem::create_directories(root / "scenes"); }
    ~PersistenceFixture() {
        host.Shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(sandbox, ignored);
    }

    engine::AssetReference Script(const char* name, std::string_view source) {
        const auto file = root / (std::string(name) + ".lua");
        const engine::AssetMetadata metadata{engine::NewUuid(), engine::CatalogAssetKind::Script};
        EXPECT_TRUE(engine::CreateNewFile(file, source, error)) << error;
        EXPECT_TRUE(engine::WriteAssetMetadata(file, metadata, error)) << error;
        return {metadata.id, "game/" + std::string(name) + ".lua"};
    }

    std::filesystem::path Level(const char* name, const engine::SceneDocument& document) {
        const auto file = root / "scenes" / (std::string(name) + ".scene.json");
        std::string text;
        EXPECT_TRUE(engine::SerializeSceneDocument(document, text, error)) << error;
        EXPECT_TRUE(engine::CreateNewFile(file, text, error)) << error;
        EXPECT_TRUE(engine::WriteAssetMetadata(file,
            {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error)) << error;
        return file;
    }

    bool Start(const std::filesystem::path& level) {
        return host.Initialize() && host.LoadScene(level, error) && host.StartGame(root, autoloads, error);
    }

    bool Lua(std::string_view source) {
        return scripts->GetRuntime().ExecuteString(source, "persistence-integration", error);
    }

    std::string Read(const std::filesystem::path& file) {
        std::string text;
        EXPECT_TRUE(engine::ReadTextFile(file, text, error)) << error;
        return text;
    }

    engine::GameDocument ReadGame(const std::filesystem::path& file) {
        engine::GameDocument game;
        EXPECT_TRUE(engine::ParseGameDocument(Read(file), game, error)) << error;
        return game;
    }

    bool WriteGame(const std::filesystem::path& file, const engine::GameDocument& game) {
        std::string text;
        return engine::SerializeGameDocument(game, text, error) && engine::WriteFileAtomic(file, text, error);
    }

    std::filesystem::path OnlySave(const std::filesystem::path& directory) {
        std::vector<std::filesystem::path> files;
        if (std::filesystem::exists(directory))
            for (const auto& entry : std::filesystem::directory_iterator(directory))
                if (entry.is_regular_file()) files.push_back(entry.path());
        EXPECT_EQ(files.size(), 1u);
        return files.size() == 1 ? files.front() : std::filesystem::path{};
    }
};

engine::SceneEntityDefinition PersistentEntity(const char* name, const engine::AssetReference& script) {
    engine::SceneEntityDefinition entity;
    entity.id = engine::NewUuid();
    entity.name = name;
    entity.script = engine::ScriptComponent{script, true};
    entity.persistence = engine::GamePersistence{false, false, true};
    return entity;
}

void ExpectSuccess(const std::shared_ptr<engine::SaveGameRequest>& request) {
    ASSERT_NE(request, nullptr);
    ASSERT_TRUE(request->IsComplete());
    const auto [success, error] = request->GetResult();
    EXPECT_TRUE(success) << error;
}

void ExpectFailure(const std::shared_ptr<engine::SaveGameRequest>& request) {
    ASSERT_NE(request, nullptr);
    ASSERT_TRUE(request->IsComplete());
    const auto [success, error] = request->GetResult();
    EXPECT_FALSE(success);
    EXPECT_FALSE(error.empty());
}

TEST(PersistenceIntegration, LuaOnlyEntityAndAutoloadStateRestoreBeforeStartWithOlderVersions) {
    PersistenceFixture f;
    const auto service = f.Script("Session", R"(
        local score, loaded = 7, false
        starts = 0
        function SetScore(value) score = value end
        function Score() return score end
        function OnSaveGame() return {score = score, nested = {active = true, items = {2, 'three'}}} end
        function OnLoadGame(data, savedVersion)
            assert(starts == 0 and savedVersion == 1)
            assert(data.nested.active and data.nested.items[2] == 'three')
            score, loaded = data.score, true
        end
        function OnStart()
            starts = starts + 1
            if loaded then assert(score == 91) end
            loadedBeforeStart = loaded
        end
    )");
    f.autoloads.push_back({"Session", service, true});
    const auto behavior = f.Script("Player", R"(
        SaveVersion = 2
        local health, loaded = 10, false
        local started = false
        function OnSaveGame() return {health = health} end
        function OnLoadGame(data, savedVersion)
            assert(not started and savedVersion == 1)
            health, loaded = data.health, true
        end
        function OnStart()
            started = true
            if loaded then assert(health == 73) end
            local p = entity:GetPosition()
            p.x, p.y = health, loaded and 1 or 0
            entity:SetPosition(p)
        end
        function OnUpdate(dt) health = 73 end
    )");
    engine::SceneDocument scene;
    const auto player = PersistentEntity("Player", behavior);
    scene.entities.push_back(player);
    ASSERT_TRUE(f.Start(f.Level("Main", scene))) << f.error;
    ASSERT_TRUE(f.Lua("Autoloads.Session.SetScore(91)")) << f.error;
    engine::FrameContext frame;
    frame.deltaSeconds = 0.25;
    f.scripts->Update(frame);
    const auto file = f.sandbox / "state.json";
    ASSERT_TRUE(f.host.SaveGame(file, f.error)) << f.error;
    auto saved = f.ReadGame(file);
    ASSERT_EQ(saved.entities.size(), 1u);
    const auto& state = saved.entities.front();
    EXPECT_EQ(state.id, player.id);
    EXPECT_FALSE(state.transform);
    EXPECT_FALSE(state.controller);
    ASSERT_TRUE(state.script);
    EXPECT_EQ(state.script->asset, behavior.id);
    EXPECT_EQ(state.script->version, 2);
    EXPECT_EQ(state.script->data.At("health").Number(), 73);
    ASSERT_EQ(saved.autoloads.size(), 1u);
    EXPECT_EQ(saved.autoloads.at("Session").asset, service.id);
    EXPECT_EQ(saved.autoloads.at("Session").version, 1); // No SaveVersion declaration.
    saved.entities.front().script->version = 1;
    ASSERT_TRUE(f.WriteGame(file, saved)) << f.error;
    ASSERT_TRUE(f.Lua("Autoloads.Session.SetScore(-1)")) << f.error;
    ASSERT_TRUE(f.host.LoadGame(file, f.error)) << f.error;
    ASSERT_TRUE(f.Lua(R"(
        assert(Autoloads.Session.Score() == 91)
        assert(Autoloads.Session.starts == 1 and Autoloads.Session.loadedBeforeStart)
    )")) << f.error;
    const auto restored = f.host.GetScene().FindByUuid(player.id);
    ASSERT_NE(restored, NullEntity);
    EXPECT_FLOAT_EQ(f.host.GetScene().GetTransform(restored).position.x, 73);
    EXPECT_FLOAT_EQ(f.host.GetScene().GetTransform(restored).position.y, 1);
}

TEST(PersistenceIntegration, PersistentSpawnRestoresIdentityHierarchyAndLuaState) {
    PersistenceFixture f;
    const auto behavior = f.Script("Spawn", R"(
        local value, loaded = 18, false
        function OnSaveGame() return {value = value} end
        function OnLoadGame(data, version)
            assert(version == 1)
            value, loaded = data.value, true
        end
        function OnStart()
            local p = entity:GetPosition()
            p.y = loaded and value or -1
            entity:SetPosition(p)
        end
    )");
    engine::SceneDocument scene;
    engine::SceneEntityDefinition parent;
    parent.id = engine::NewUuid(); parent.name = "Parent";
    scene.entities.push_back(parent);
    ASSERT_TRUE(f.Start(f.Level("Main", scene))) << f.error;
    auto& world = f.host.GetScene();
    const auto spawned = world.CreateEntity("Persistent spawn");
    const auto id = world.GetId(spawned);
    ASSERT_NE(world.AddComponent<engine::ScriptComponent>(spawned, engine::ScriptComponent{behavior, true}), nullptr);
    ASSERT_NE(world.AddComponent<engine::GamePersistence>(spawned, engine::GamePersistence{true, false, true}), nullptr);
    ASSERT_TRUE(world.SetParent(spawned, world.FindByUuid(parent.id)));
    engine::FrameContext frame; frame.deltaSeconds = 0.1;
    f.scripts->Update(frame); // Reconcile the newly attached script before capture.
    world.GetTransform(spawned).position.x = 44;
    const auto transient = world.CreateEntity("Transient spawn");
    const auto transientId = world.GetId(transient);
    const auto file = f.sandbox / "spawn.json";
    ASSERT_TRUE(f.host.SaveGame(file, f.error)) << f.error;
    world.DestroyEntity(spawned);
    ASSERT_TRUE(f.host.LoadGame(file, f.error)) << f.error;
    const auto restored = world.FindByUuid(id);
    ASSERT_NE(restored, NullEntity);
    EXPECT_EQ(world.FindByUuid(transientId), NullEntity);
    EXPECT_FLOAT_EQ(world.GetTransform(restored).position.x, 44);
    EXPECT_FLOAT_EQ(world.GetTransform(restored).position.y, 18);
    const auto restoredParent = world.FindByUuid(parent.id);
    ASSERT_NE(restoredParent, NullEntity);
    EXPECT_EQ(world.GetParent(restored), restoredParent);
    ASSERT_TRUE(world.HasComponent<engine::ScriptComponent>(restored));
    ASSERT_TRUE(world.HasComponent<engine::GamePersistence>(restored));
}

TEST(PersistenceIntegration, InvalidLoadPreflightPreservesLiveSceneAndLuaSession) {
    PersistenceFixture f;
    const auto service = f.Script("Session", R"(
        token = 12
        function OnSaveGame() return {token = token} end
        function OnLoadGame(data, version) token = data.token end
    )");
    f.autoloads.push_back({"Session", service, true});
    engine::SceneDocument scene;
    engine::SceneEntityDefinition entity;
    entity.id = engine::NewUuid(); entity.name = "Original";
    scene.entities.push_back(entity);
    ASSERT_TRUE(f.Start(f.Level("Main", scene))) << f.error;
    const auto file = f.sandbox / "valid.json";
    ASSERT_TRUE(f.host.SaveGame(file, f.error)) << f.error;
    const auto valid = f.ReadGame(file);
    auto& world = f.host.GetScene();
    const auto original = world.FindByUuid(entity.id);
    const auto runtime = world.CreateEntity("Unsaved runtime entity");
    world.GetTransform(original).position.x = 123;
    ASSERT_TRUE(f.Lua("Autoloads.Session.token = 99; retained = Autoloads.Session")) << f.error;

    // Structural/reference failures must be rejected before any live teardown.
    for (int variant = 0; variant != 3; ++variant) {
        SCOPED_TRACE(variant);
        auto invalid = valid;
        if (variant == 0) invalid.scene.id = engine::NewUuid();
        if (variant == 1) invalid.deleted.push_back(engine::NewUuid());
        if (variant == 2) {
            engine::GameEntityState unknown;
            unknown.id = engine::NewUuid();
            unknown.transform = engine::Transform{};
            invalid.entities.push_back(unknown);
        }
        ASSERT_TRUE(f.WriteGame(file, invalid)) << f.error;
        EXPECT_FALSE(f.host.LoadGame(file, f.error));
        EXPECT_FALSE(f.error.empty());
        EXPECT_TRUE(f.host.IsPlaying());
        EXPECT_EQ(world.FindByUuid(entity.id), original);
        EXPECT_TRUE(world.IsValid(runtime));
        EXPECT_FLOAT_EQ(world.GetTransform(original).position.x, 123);
        ASSERT_TRUE(f.Lua("assert(retained == Autoloads.Session and retained.token == 99)")) << f.error;
    }
    ASSERT_TRUE(engine::WriteFileAtomic(file, "{broken json", f.error)) << f.error;
    EXPECT_FALSE(f.host.LoadGame(file, f.error));
    EXPECT_TRUE(world.IsValid(runtime));
    ASSERT_TRUE(f.Lua("assert(retained == Autoloads.Session and retained.token == 99)")) << f.error;
    ASSERT_TRUE(f.WriteGame(file, valid)) << f.error;
    ASSERT_TRUE(f.host.LoadGame(file, f.error)) << f.error;
    ASSERT_TRUE(f.Lua("assert(Autoloads.Session.token == 12)")) << f.error;
}

TEST(PersistenceIntegration, RequestsAreDeferredAndUseDefaultAndExplicitSaveDirectories) {
    for (const bool explicitDirectory : {false, true}) {
        SCOPED_TRACE(explicitDirectory);
        PersistenceFixture f;
        const auto service = f.Script("Session", R"(
            value = 3
            function OnSaveGame() return {value = value} end
            function OnLoadGame(data, version) value = data.value end
        )");
        f.autoloads.push_back({"Session", service, true});
        const auto directory = explicitDirectory ? f.sandbox / "custom-saves" : f.sandbox / ".sri/saves";
        if (explicitDirectory) f.host.SetSaveDirectory(directory);
        ASSERT_TRUE(f.Start(f.Level("Main", {}))) << f.error;
        const auto save = f.host.RequestSave("checkpoint", f.error);
        ASSERT_NE(save, nullptr) << f.error;
        EXPECT_FALSE(save->IsComplete());
        if (std::filesystem::exists(directory)) EXPECT_TRUE(std::filesystem::is_empty(directory));
        ASSERT_TRUE(f.Lua("Autoloads.Session.value = 47")) << f.error;
        f.host.Tick(); // The first frame may have dt == 0; requests must still drain.
        ExpectSuccess(save);
        const auto file = f.OnlySave(directory);
        ASSERT_FALSE(file.empty());
        EXPECT_EQ(f.ReadGame(file).autoloads.at("Session").data.At("value").Number(), 47);
        ASSERT_TRUE(f.Lua("Autoloads.Session.value = -1")) << f.error;
        const auto load = f.host.RequestLoad("checkpoint", f.error);
        ASSERT_NE(load, nullptr) << f.error;
        EXPECT_FALSE(load->IsComplete());
        ASSERT_TRUE(f.Lua("assert(Autoloads.Session.value == -1)")) << f.error;
        f.host.Tick();
        ExpectSuccess(load);
        ASSERT_TRUE(f.Lua("assert(Autoloads.Session.value == 47)")) << f.error;
    }
}

TEST(PersistenceIntegration, FutureScriptVersionsAndWrongAssetsAreRejectedBeforeRestore) {
    PersistenceFixture f;
    const auto service = f.Script("Session", R"(
        token = 4
        function OnSaveGame() return {token = token} end
        function OnLoadGame(data, version) token = data.token end
    )");
    const auto behavior = f.Script("Player", R"(
        SaveVersion = 2
        function OnSaveGame() return {health = 20} end
        function OnLoadGame(data, version)
            local p = entity:GetPosition()
            p.x = data.health
            entity:SetPosition(p)
        end
    )");
    f.autoloads.push_back({"Session", service, true});
    engine::SceneDocument scene;
    const auto player = PersistentEntity("Player", behavior);
    scene.entities.push_back(player);
    ASSERT_TRUE(f.Start(f.Level("Main", scene))) << f.error;
    const auto file = f.sandbox / "versions.json";
    ASSERT_TRUE(f.host.SaveGame(file, f.error)) << f.error;
    const auto valid = f.ReadGame(file);
    ASSERT_EQ(valid.entities.size(), 1u);
    ASSERT_TRUE(valid.entities.front().script);
    ASSERT_TRUE(f.Lua("Autoloads.Session.token = 99; retained = Autoloads.Session")) << f.error;
    const auto entity = f.host.GetScene().FindByUuid(player.id);
    ASSERT_NE(entity, NullEntity);
    f.host.GetScene().GetTransform(entity).position.x = 71;
    for (const bool autoload : {false, true}) {
        for (const bool wrongAsset : {false, true}) {
            SCOPED_TRACE(autoload);
            SCOPED_TRACE(wrongAsset);
            auto invalid = valid;
            auto& state = autoload ? invalid.autoloads.at("Session") : *invalid.entities.front().script;
            if (wrongAsset) state.asset = engine::NewUuid();
            else ++state.version;
            ASSERT_TRUE(f.WriteGame(file, invalid)) << f.error;
            EXPECT_FALSE(f.host.LoadGame(file, f.error));
            EXPECT_FALSE(f.error.empty());
            EXPECT_TRUE(f.host.IsPlaying());
            EXPECT_EQ(f.host.GetScene().FindByUuid(player.id), entity);
            EXPECT_FLOAT_EQ(f.host.GetScene().GetTransform(entity).position.x, 71);
            ASSERT_TRUE(f.Lua("assert(retained == Autoloads.Session and retained.token == 99)")) << f.error;
        }
    }
}

TEST(PersistenceIntegration, RequestSlotsRejectTraversalAndAbsolutePaths) {
    PersistenceFixture f;
    ASSERT_TRUE(f.Start(f.Level("Main", {}))) << f.error;
    for (const auto* slot : {"", ".", "..", "../escape", "..\\escape", "folder/slot", "folder\\slot",
                             "/absolute", "C:\\absolute", "C:relative"}) {
        SCOPED_TRACE(slot);
        f.error.clear();
        EXPECT_EQ(f.host.RequestSave(slot, f.error), nullptr);
        EXPECT_FALSE(f.error.empty());
        f.error.clear();
        EXPECT_EQ(f.host.RequestLoad(slot, f.error), nullptr);
        EXPECT_FALSE(f.error.empty());
    }
    f.host.Tick();
    const auto directory = f.sandbox / ".sri/saves";
    if (std::filesystem::exists(directory)) EXPECT_TRUE(std::filesystem::is_empty(directory));
    EXPECT_TRUE(f.host.IsPlaying());
}

TEST(PersistenceIntegration, FailedSaveHookPreservesExistingBytesAndScriptKeepsRunning) {
    PersistenceFixture f;
    const auto service = f.Script("Session", R"(
        failSave, updates, value = false, 0, 5
        function OnUpdate(dt) updates = updates + 1 end
        function OnSaveGame()
            if failSave then error('intentional-save-failure') end
            return {value = value}
        end
        function OnLoadGame(data, version) value = data.value end
    )");
    f.autoloads.push_back({"Session", service, true});
    ASSERT_TRUE(f.Start(f.Level("Main", {}))) << f.error;
    const auto initial = f.host.RequestSave("atomic", f.error);
    ASSERT_NE(initial, nullptr) << f.error;
    f.host.Tick();
    ExpectSuccess(initial);
    const auto file = f.OnlySave(f.sandbox / ".sri/saves");
    ASSERT_FALSE(file.empty());
    const auto bytes = f.Read(file);
    ASSERT_TRUE(f.Lua("Autoloads.Session.failSave = true; beforeFailure = Autoloads.Session.updates")) << f.error;
    const auto failed = f.host.RequestSave("atomic", f.error);
    ASSERT_NE(failed, nullptr) << f.error;
    f.host.Tick();
    ExpectFailure(failed);
    EXPECT_NE(failed->GetResult().second.find("intentional-save-failure"), std::string::npos);
    EXPECT_EQ(f.Read(file), bytes);
    f.host.Tick();
    ASSERT_TRUE(f.Lua(R"(
        assert(Autoloads.Session.updates > beforeFailure)
        Autoloads.Session.failSave = false
        Autoloads.Session.value = 88
    )")) << f.error;
    const auto retry = f.host.RequestSave("atomic", f.error);
    ASSERT_NE(retry, nullptr) << f.error;
    f.host.Tick();
    ExpectSuccess(retry);
    EXPECT_EQ(f.ReadGame(file).autoloads.at("Session").data.At("value").Number(), 88);
}

TEST(PersistenceIntegration, StandaloneStartupRequiresInitializationNonEditorAndLoadedScene) {
    PersistenceFixture f;
    const auto level = f.Level("Main", {});
    EXPECT_FALSE(f.host.StartGame(f.root, {}, f.error));
    EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
    ASSERT_TRUE(f.host.Initialize());
    EXPECT_FALSE(f.host.StartGame(f.root, {}, f.error));
    ASSERT_TRUE(f.host.LoadScene(level, f.error)) << f.error;
    ASSERT_TRUE(f.host.StartGame(f.root, {}, f.error)) << f.error;
    EXPECT_TRUE(f.host.IsPlaying());
    EXPECT_FALSE(f.host.StartGame(f.root, {}, f.error));
    f.host.StopGame();
    f.host.StopGame();
    EXPECT_FALSE(f.host.IsPlaying());
    EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
    f.host.SetEditorMode();
    EXPECT_FALSE(f.host.StartGame(f.root, {}, f.error));
    EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
}

TEST(PersistenceIntegration, StandaloneSceneTransitionsRetainAutoloadAndShutdownEndsSession) {
    PersistenceFixture f;
    const auto service = f.Script("Session", "starts = 0; function OnStart() starts = starts + 1 end");
    f.autoloads.push_back({"Session", service, true});
    engine::SceneDocument first, next;
    engine::SceneEntityDefinition a, b;
    a.id = engine::NewUuid(); a.name = "First";
    b.id = engine::NewUuid(); b.name = "Next";
    first.entities.push_back(a); next.entities.push_back(b);
    const auto level = f.Level("First", first);
    f.Level("Next", next);
    ASSERT_TRUE(f.Start(level)) << f.error;
    ASSERT_TRUE(f.Lua("Autoloads.Session.token = 63; retained = Autoloads.Session; assert(Scene.OpenLevel('Next'))")) << f.error;
    EXPECT_NE(f.host.GetScene().FindByUuid(a.id), NullEntity);
    EXPECT_EQ(f.host.GetScene().FindByUuid(b.id), NullEntity);
    f.host.Tick();
    EXPECT_EQ(f.host.GetScene().FindByUuid(a.id), NullEntity);
    EXPECT_NE(f.host.GetScene().FindByUuid(b.id), NullEntity);
    ASSERT_TRUE(f.Lua("assert(retained == Autoloads.Session and retained.token == 63 and retained.starts == 1)")) << f.error;
    f.host.Shutdown();
    EXPECT_FALSE(f.host.IsInitialized());
    EXPECT_FALSE(f.host.IsPlaying());
    EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
}

TEST(PersistenceIntegration, StopCancelsOldRequestsAndTheyCannotAffectRestartedSession) {
    PersistenceFixture f;
    const auto level = f.Level("Main", {});
    ASSERT_TRUE(f.Start(level)) << f.error;
    const auto save = f.host.RequestSave("old-session", f.error);
    ASSERT_NE(save, nullptr) << f.error;
    EXPECT_FALSE(save->IsComplete());
    f.host.StopGame();
    ExpectFailure(save);
    const auto cancelledResult = save->GetResult();
    ASSERT_TRUE(f.host.LoadScene(level, f.error)) << f.error;
    ASSERT_TRUE(f.host.StartGame(f.root, {}, f.error)) << f.error;
    f.host.Tick();
    EXPECT_EQ(save->GetResult(), cancelledResult);
    const auto directory = f.sandbox / ".sri/saves";
    if (std::filesystem::exists(directory)) EXPECT_TRUE(std::filesystem::is_empty(directory));
    const auto fresh = f.host.RequestSave("new-session", f.error);
    ASSERT_NE(fresh, nullptr) << f.error;
    f.host.Tick();
    ExpectSuccess(fresh);
    const auto load = f.host.RequestLoad("new-session", f.error);
    ASSERT_NE(load, nullptr) << f.error;
    EXPECT_FALSE(load->IsComplete());
    f.host.StopGame();
    ExpectFailure(load);
    ASSERT_TRUE(f.host.LoadScene(level, f.error)) << f.error;
    ASSERT_TRUE(f.host.StartGame(f.root, {}, f.error)) << f.error;
    const auto marker = f.host.GetScene().CreateEntity("New session marker");
    f.host.Tick();
    EXPECT_TRUE(f.host.GetScene().IsValid(marker));
    ExpectFailure(load);
}

TEST(PersistenceIntegration, StopRejectsLuaTeardownRequestsAndLeavesNoWorkAcrossRestart) {
    for (const bool editor : {false, true}) {
        SCOPED_TRACE(editor);
        std::vector<std::string> messages;
        const auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&](const spdlog::details::log_msg& message) {
                messages.emplace_back(message.payload.data(), message.payload.size());
            });
        struct SinkScope {
            std::shared_ptr<spdlog::sinks::sink> sink;
            explicit SinkScope(std::shared_ptr<spdlog::sinks::sink> value) : sink(std::move(value)) {
                engine::log::Sinks()->add_sink(sink);
            }
            ~SinkScope() { engine::log::Sinks()->remove_sink(sink); }
        } capture{sink};
        PersistenceFixture f;
        const auto service = f.Script("Teardown", R"(
            function OnDestroy()
                local save, saveError = SaveGame.RequestSave('teardown-save')
                local load, loadError = SaveGame.RequestLoad('checkpoint')
                local opened, openError = Scene.OpenLevel('Forbidden')
                assert(save == nil and type(saveError) == 'string' and #saveError > 0)
                assert(load == nil and type(loadError) == 'string' and #loadError > 0)
                assert(not opened and type(openError) == 'string' and #openError > 0)
                Log.info('persistence-teardown-rejected-all-work')
            end
        )");
        f.autoloads.push_back({"Teardown", service, true});
        engine::SceneDocument scene, forbidden;
        engine::SceneEntityDefinition original, unexpected;
        original.id = engine::NewUuid(); original.name = "Original";
        unexpected.id = engine::NewUuid(); unexpected.name = "Forbidden transition";
        scene.entities.push_back(original);
        forbidden.entities.push_back(unexpected);
        const auto level = f.Level("Main", scene);
        f.Level("Forbidden", forbidden);
        if (editor) f.host.SetEditorMode();
        ASSERT_TRUE(f.host.Initialize());
        ASSERT_TRUE(f.host.LoadScene(level, f.error)) << f.error;
        const auto start = [&] {
            return editor ? f.host.PlayCurrentScene(f.root, f.autoloads, f.error)
                          : f.host.StartGame(f.root, f.autoloads, f.error);
        };
        const auto stop = [&] {
            if (editor) f.host.StopPlay();
            else f.host.StopGame();
        };
        ASSERT_TRUE(start()) << f.error;
        const auto seed = f.host.RequestSave("checkpoint", f.error);
        ASSERT_NE(seed, nullptr) << f.error;
        f.host.Tick();
        ExpectSuccess(seed);
        const auto directory = f.sandbox / ".sri/saves";
        const auto file = f.OnlySave(directory);
        ASSERT_FALSE(file.empty());
        const auto bytes = f.Read(file);
        stop();
        EXPECT_EQ(std::count(messages.begin(), messages.end(), "persistence-teardown-rejected-all-work"), 1);
        EXPECT_FALSE(f.host.IsPlaying());
        EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
        f.host.Tick();
        EXPECT_NE(f.host.GetScene().FindByUuid(original.id), NullEntity);
        EXPECT_EQ(f.host.GetScene().FindByUuid(unexpected.id), NullEntity);
        ASSERT_TRUE(start()) << f.error;
        const auto marker = f.host.GetScene().CreateEntity("Restart must not load checkpoint");
        const auto markerId = f.host.GetScene().GetId(marker);
        f.host.Tick();
        EXPECT_TRUE(f.host.IsPlaying());
        EXPECT_EQ(f.host.GetScene().FindByUuid(markerId), marker);
        EXPECT_EQ(f.host.GetScene().FindByUuid(unexpected.id), NullEntity);
        EXPECT_EQ(f.OnlySave(directory), file);
        EXPECT_EQ(f.Read(file), bytes);
        stop();
        EXPECT_EQ(std::count(messages.begin(), messages.end(), "persistence-teardown-rejected-all-work"), 2);
    }
}

TEST(PersistenceIntegration, NativeRestorePrecedesLuaLoadAndStartAndEnableFollowsBoth) {
    std::vector<std::string> events;
    struct OrderedController final : engine::Controller {
        std::vector<std::string>& events;
        explicit OrderedController(std::vector<std::string>& value) : events(value) {}
        void SaveGameState(engine::StateValue& state) const override { state["value"] = 42; }
        void LoadGameState(const engine::StateValue& state) override {
            EXPECT_EQ(state.At("value").Number(), 42);
            events.push_back("native-restore");
            GetScene().SetName(GetEntity(), "restored");
        }
        void OnEnable() override {
            events.push_back("native-enable");
            if (GetScene().GetName(GetEntity()) == "restored") {
                const auto tags = GetScene().GetTags(GetEntity());
                EXPECT_NE(std::find(tags.begin(), tags.end(), "lua-loaded"), tags.end());
                EXPECT_NE(std::find(tags.begin(), tags.end(), "lua-started"), tags.end());
            }
        }
    };
    PersistenceFixture f;
    ASSERT_TRUE(f.host.GetScene().RegisterController("ordered", {1, {}, [&] {
        return std::make_unique<OrderedController>(events);
    }}));
    const auto behavior = f.Script("Ordered", R"(
        local loaded = false
        function OnSaveGame() return {value = 17} end
        function OnLoadGame(data, version)
            assert(entity:GetName() == 'restored' and data.value == 17 and version == 1)
            loaded = true
            entity:AddTag('lua-loaded')
        end
        function OnStart()
            if loaded then
                assert(entity:GetName() == 'restored' and entity:HasTag('lua-loaded'))
                entity:AddTag('lua-started')
            end
        end
    )");
    engine::SceneDocument scene;
    auto entity = PersistentEntity("Authored", behavior);
    entity.persistence = engine::GamePersistence{false, true, true};
    entity.controller = engine::ControllerDefinition{"ordered", 1, true, {}};
    scene.entities.push_back(entity);
    ASSERT_TRUE(f.Start(f.Level("Main", scene))) << f.error;
    const auto file = f.sandbox / "ordered.json";
    ASSERT_TRUE(f.host.SaveGame(file, f.error)) << f.error;
    const auto saved = f.ReadGame(file);
    ASSERT_EQ(saved.entities.size(), 1u);
    ASSERT_TRUE(saved.entities.front().controller);
    ASSERT_TRUE(saved.entities.front().script);
    events.clear();
    ASSERT_TRUE(f.host.LoadGame(file, f.error)) << f.error;
    EXPECT_EQ(events, (std::vector<std::string>{"native-restore", "native-enable"}));
    const auto restored = f.host.GetScene().FindByUuid(entity.id);
    ASSERT_NE(restored, NullEntity);
    EXPECT_EQ(f.host.GetScene().GetName(restored), "restored");
    EXPECT_EQ(f.host.GetScene().FindWithTag("lua-loaded"), (std::vector<entt::entity>{restored}));
    EXPECT_EQ(f.host.GetScene().FindWithTag("lua-started"), (std::vector<entt::entity>{restored}));
}

TEST(PersistenceIntegration, ThrowingNativeRestoreFailsLoadWithDiagnosticBeforeLuaHooksOrEnable) {
    int restores = 0, enables = 0;
    struct ThrowingController final : engine::Controller {
        int& restores;
        int& enables;
        ThrowingController(int& r, int& e) : restores(r), enables(e) {}
        void SaveGameState(engine::StateValue& state) const override { state["value"] = 42; }
        void LoadGameState(const engine::StateValue& state) override {
            EXPECT_EQ(state.At("value").Number(), 42);
            ++restores;
            throw std::runtime_error("intentional-native-restore-failure");
        }
        void OnEnable() override { ++enables; }
    };
    PersistenceFixture f;
    ASSERT_TRUE(f.host.GetScene().RegisterController("throwing", {1, {}, [&] {
        return std::make_unique<ThrowingController>(restores, enables);
    }}));
    const auto behavior = f.Script("MustNotRestore", R"(
        function OnSaveGame() return {value = 7} end
        function OnLoadGame(data, version) entity:AddTag('unexpected-lua-load') end
        function OnStart() entity:AddTag('lua-start') end
    )");
    engine::SceneDocument scene;
    auto entity = PersistentEntity("Throwing", behavior);
    entity.persistence = engine::GamePersistence{false, true, true};
    entity.controller = engine::ControllerDefinition{"throwing", 1, true, {}};
    scene.entities.push_back(entity);
    ASSERT_TRUE(f.Start(f.Level("Main", scene))) << f.error;
    const auto file = f.sandbox / "throwing.json";
    ASSERT_TRUE(f.host.SaveGame(file, f.error)) << f.error;
    enables = 0;
    EXPECT_FALSE(f.host.LoadGame(file, f.error));
    EXPECT_NE(f.error.find("intentional-native-restore-failure"), std::string::npos) << f.error;
    EXPECT_EQ(restores, 1);
    EXPECT_EQ(enables, 0);
    EXPECT_TRUE(f.host.GetScene().FindWithTag("unexpected-lua-load").empty());
    EXPECT_TRUE(f.host.GetScene().FindWithTag("lua-start").empty());
}

TEST(PersistenceIntegration, StandaloneLoadKeepsNativeControllersDormantUntilLuaStartupFinishes) {
    std::vector<std::string> events;
    struct StartupController final : engine::Controller {
        std::vector<std::string>& events;
        explicit StartupController(std::vector<std::string>& value) : events(value) {}
        void ExpectLuaStarted() {
            const auto tags = GetScene().GetTags(GetEntity());
            EXPECT_NE(std::find(tags.begin(), tags.end(), "autoload-started"), tags.end());
            EXPECT_NE(std::find(tags.begin(), tags.end(), "entity-started"), tags.end());
        }
        void OnCreate() override {
            ExpectLuaStarted();
            events.push_back("native-create");
        }
        void OnEnable() override {
            ExpectLuaStarted();
            events.push_back("native-enable");
        }
        void OnDisable() override { events.push_back("native-disable"); }
    };
    PersistenceFixture f;
    ASSERT_TRUE(f.host.GetScene().RegisterController("startup-order", {1, {}, [&] {
        return std::make_unique<StartupController>(events);
    }}));
    const auto service = f.Script("Startup", R"(
        function OnStart()
            local target = Scene.FindByName('Startup target')
            assert(target and target:IsValid())
            assert(not target:HasTag('entity-started'))
            target:AddTag('autoload-started')
        end
    )");
    f.autoloads.push_back({"Startup", service, true});
    const auto behavior = f.Script("StartupEntity", R"(
        function OnStart()
            assert(entity:HasTag('autoload-started'))
            entity:AddTag('entity-started')
        end
    )");
    engine::SceneDocument scene;
    engine::SceneEntityDefinition entity;
    entity.id = engine::NewUuid();
    entity.name = "Startup target";
    entity.script = engine::ScriptComponent{behavior, true};
    entity.controller = engine::ControllerDefinition{"startup-order", 1, true, {}};
    scene.entities.push_back(entity);
    const auto level = f.Level("Main", scene);
    ASSERT_TRUE(f.host.Initialize());
    ASSERT_TRUE(f.host.LoadScene(level, f.error)) << f.error;
    EXPECT_TRUE(events.empty()); // Neither native OnCreate nor OnEnable at LoadScene.
    EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
    EXPECT_TRUE(f.host.GetScene().FindWithTag("autoload-started").empty());
    EXPECT_TRUE(f.host.GetScene().FindWithTag("entity-started").empty());
    ASSERT_TRUE(f.host.StartGame(f.root, f.autoloads, f.error)) << f.error;
    EXPECT_EQ(events, (std::vector<std::string>{"native-create", "native-enable"}));
    const auto target = f.host.GetScene().FindByUuid(entity.id);
    ASSERT_NE(target, NullEntity);
    EXPECT_EQ(f.host.GetScene().FindWithTag("autoload-started"), (std::vector<entt::entity>{target}));
    EXPECT_EQ(f.host.GetScene().FindWithTag("entity-started"), (std::vector<entt::entity>{target}));
    f.host.StopGame();
    EXPECT_EQ(events, (std::vector<std::string>{"native-create", "native-enable", "native-disable"}));
    EXPECT_FALSE(f.host.IsPlaying());
    EXPECT_FALSE(f.scripts->GetRuntime().IsInitialized());
}

} // namespace
