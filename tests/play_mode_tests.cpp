#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/core/FrameContext.hpp>
#include <gtest/gtest.h>

namespace {
constexpr entt::entity NullEntity = entt::null;
struct PlayFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-play-" + engine::NewUuid());
    std::filesystem::path previous = engine::AssetPathResolver::GetGameRoot();
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::ScriptSubsystem* scripting = host.AddSubsystem<engine::ScriptSubsystem>();
    PlayFixture() {
        std::filesystem::create_directories(root / "scenes");
        engine::AssetPathResolver::SetGameRoot(root);
        host.SetEditorMode();
    }
    ~PlayFixture() {
        host.Shutdown();
        engine::AssetPathResolver::SetGameRoot(previous);
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    void Level(const char* name) {
        engine::SceneDocument document;
        engine::SceneEntityDefinition entity;
        entity.id = engine::NewUuid(); entity.name = name;
        document.entities.push_back(entity);
        std::string text, error;
        ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error));
        const auto file = root / "scenes" / (std::string(name) + ".scene.json");
        ASSERT_TRUE(engine::CreateNewFile(file, text, error));
        ASSERT_TRUE(engine::WriteAssetMetadata(file, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
    }
};

TEST(PlayMode, StopRestoresUnsavedEntitiesAndRemovesRuntimeChanges) {
    PlayFixture f;
    ASSERT_TRUE(f.host.Initialize());
    auto entity = f.host.GetScene().CreateEntity("Unsaved");
    const std::string id = f.host.GetScene().GetId(entity).ToString();
    f.host.GetScene().GetTransform(entity).position.x = 12;
    std::string error;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
        entity = f.host.GetScene().FindById(id);
        f.host.GetScene().DestroyEntity(entity);
        f.host.GetScene().CreateEntity("Spawned");
        EXPECT_FALSE(f.host.PlayCurrentScene(f.root, {}, error));
        f.host.StopPlay(); f.host.StopPlay();
        EXPECT_FALSE(f.host.IsPlaying());
        EXPECT_FALSE(f.scripting->GetRuntime().IsInitialized());
        entity = f.host.GetScene().FindById(id);
        ASSERT_TRUE(f.host.GetScene().IsValid(entity));
        EXPECT_EQ(f.host.GetScene().GetTransform(entity).position.x, 12);
        EXPECT_EQ(f.host.GetScene().FindFirstByName("Spawned"), NullEntity);
    }
}

TEST(PlayMode, LuaOpenLevelIsDeferredAndAutoloadStateSurvivesAcrossScenes) {
    PlayFixture f;
    f.Level("Next"); f.Level("Last");
    std::string error;
    const auto source = f.root / "Game.lua";
    ASSERT_TRUE(engine::CreateNewFile(source, R"(
        local shared = require('shared')
        function OnStart()
            shared.starts = shared.starts + 1
            starts = (starts or 0) + 1
            assert(Scene.OpenLevel('Next'))
        end
    )", error));
    ASSERT_TRUE(engine::CreateNewFile(f.root / "shared.lua", "return {starts = 0}", error));
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(f.root / "shared.lua", {engine::NewUuid(), engine::CatalogAssetKind::Script}, error));
    const auto behavior = f.root / "Player.lua";
    const engine::AssetMetadata behaviorMeta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(behavior, R"(
        function OnStart()
            Autoloads.Game.player_score = 42
        end
        function OnDestroy()
            assert(Autoloads.Game.player_score == 42)
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(behavior, behaviorMeta, error));
    ASSERT_TRUE(f.host.Initialize());
    f.host.GetScene().AddComponent<engine::ScriptComponent>(f.host.GetScene().CreateEntity("Editor original"),
        engine::ScriptComponent{{behaviorMeta.id, "game/Player.lua"}, true});
    const std::vector<engine::AutoloadDefinition> autoloads{{"Game", {meta.id, "game/Game.lua"}, true}};
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, autoloads, error)) << error;
    EXPECT_NE(f.host.GetScene().FindFirstByName("Editor original"), NullEntity);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Next"), NullEntity);
    ASSERT_TRUE(f.scripting->GetRuntime().ExecuteString("assert(Scene.OpenLevel('Last'))", "switch", error));
    EXPECT_NE(f.host.GetScene().FindFirstByName("Next"), NullEntity);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Last"), NullEntity);
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(
        "assert(require('shared').starts == 1 and Autoloads.Game.starts == 1 and Autoloads.Game.player_score == 42)",
        "check", error));
    EXPECT_FALSE(f.host.RequestOpenLevel("../escape", error));
    EXPECT_FALSE(f.host.RequestOpenLevel("Missing", error));
    EXPECT_NE(f.host.GetScene().FindFirstByName("Last"), NullEntity);
    f.host.StopPlay();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Editor original"), NullEntity);
    EXPECT_EQ(f.host.GetScene().FindFirstByName("Last"), NullEntity);
}

TEST(PlayMode, StartupFailureRestoresSceneAndCancelsQueuedLevel) {
    PlayFixture f; f.Level("Next");
    const auto file = f.root / "Bad.lua";
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(file, "return {OnStart = function(self) assert(Scene.OpenLevel('Next')); error('fail') end}", error));
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    f.host.GetScene().CreateEntity("Original");
    const std::vector<engine::AutoloadDefinition> definitions{{"Bad", {meta.id, "game/Bad.lua"}, true}};
    EXPECT_FALSE(f.host.PlayCurrentScene(f.root, definitions, error));
    EXPECT_FALSE(f.host.IsPlaying());
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Original"), NullEntity);
    EXPECT_EQ(f.host.GetScene().FindFirstByName("Next"), NullEntity);
}

TEST(PlayMode, NativeControllersStayDormantInEditAndRestartOnPlay) {
    PlayFixture f;
    int created = 0, destroyed = 0;
    struct Counter : engine::Controller {
        int& created; int& destroyed;
        Counter(int& c, int& d) : created(c), destroyed(d) {}
        void OnCreate() override { ++created; }
        void OnDestroy() override { ++destroyed; }
    };
    ASSERT_TRUE(f.host.GetScene().RegisterController("counter", {1, {}, [&] { return std::make_unique<Counter>(created, destroyed); }}));
    const auto entity = f.host.GetScene().CreateEntity("Controlled");
    f.host.GetScene().AttachController(entity, {"counter", 1, true, {}});
    EXPECT_EQ(created, 0);
    ASSERT_TRUE(f.host.Initialize());
    std::string error;
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    EXPECT_EQ(created, 1);
    f.host.SetPaused(true);
    f.host.Tick();
    EXPECT_EQ(f.host.GetFrame().deltaSeconds, 0);
    f.host.StopPlay();
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(created, 1);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    EXPECT_EQ(created, 2);
    f.host.StopPlay();
}
TEST(ScriptComponents, IndependentInstancesMoveOwnersAndStopRestoresAuthoringData) {
    PlayFixture f;
    std::string error;
    const auto source = f.root / "Move.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(source, R"(
        elapsed = 0
        function OnStart() assert(entity:IsValid()) end
        function OnUpdate(dt)
            elapsed = elapsed + dt
            local p = entity:GetPosition()
            p.x = p.x + elapsed
            entity:SetPosition(p)
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    auto first = scene.CreateEntity("First");
    auto second = scene.CreateEntity("Second");
    scene.GetTransform(second).position.x = 10;
    for (const auto entity : {first, second})
        scene.AddComponent<engine::ScriptComponent>(entity, engine::ScriptComponent{{meta.id, "game/Move.lua"}, true});
    EXPECT_EQ(scene.AddComponent<engine::ScriptComponent>(first), nullptr);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    first = scene.FindFirstByName("First"); second = scene.FindFirstByName("Second");
    engine::FrameContext frame; frame.deltaSeconds = 0.5;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 0.5f);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 10.5f);
    scene.TryGetComponent<engine::ScriptComponent>(first)->enabled = false;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 0.5f);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 11.5f);
    scene.TryGetComponent<engine::ScriptComponent>(first)->enabled = true;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 1.0f); // elapsed restarted at zero.
    scene.RemoveComponent<engine::ScriptComponent>(second);
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 13.0f);
    f.host.StopPlay();
    first = scene.FindFirstByName("First"); second = scene.FindFirstByName("Second");
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 0);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 10);
    ASSERT_TRUE(scene.HasComponent<engine::ScriptComponent>(second));
    EXPECT_TRUE(scene.TryGetComponent<engine::ScriptComponent>(first)->enabled);
}

TEST(ScriptComponents, SceneSwitchStartsNewScriptsAndInvalidatesRetainedOwner) {
    PlayFixture f;
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(f.root / "shared.lua", "return {owners={}, destroys=0}", error));
    const auto source = f.root / "Owner.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(source, R"(
        local shared = require('shared')
        function OnStart() table.insert(shared.owners, entity) end
        function OnDestroy()
            assert(entity:IsValid())
            shared.destroys = shared.destroys + 1
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    engine::SceneDocument document;
    engine::SceneEntityDefinition next; next.id = engine::NewUuid(); next.name = "Next";
    next.script = engine::ScriptComponent{{meta.id, "game/Owner.lua"}, true};
    document.entities.push_back(next);
    std::string text;
    ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error));
    const auto level = f.root / "scenes/Next.scene.json";
    ASSERT_TRUE(engine::CreateNewFile(level, text, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(level, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Old"), *next.script);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    ASSERT_TRUE(f.host.RequestOpenLevel("Next", error));
    f.host.Tick();
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(R"(
        local s = require('shared')
        assert(#s.owners == 2 and s.destroys == 1)
        assert(not s.owners[1]:IsValid() and s.owners[2]:IsValid())
        assert(not pcall(function() s.owners[1]:GetPosition() end))
    )", "owners-after-scene-change", error)) << error;
    scene.DestroyEntity(scene.FindFirstByName("Next"));
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(
        "assert(require('shared').destroys == 2); assert(not require('shared').owners[2]:IsValid())",
        "owners-after-delete", error)) << error;
    f.host.StopPlay();
}

TEST(ScriptComponents, BrokenAndMissingAttachmentsDoNotStopHealthyObjects) {
    PlayFixture f;
    std::string error;
    const auto file = f.root / "Check.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(file, R"(
        function OnUpdate(dt)
            local p = entity:GetPosition()
            if p.x < 0 then error('broken object') end
            p.x = p.x + 1
            entity:SetPosition(p)
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    const auto bad = scene.CreateEntity("Broken");
    scene.GetTransform(bad).position.x = -1;
    const auto good = scene.CreateEntity("Healthy");
    for (auto entity : {bad, good}) scene.AddComponent<engine::ScriptComponent>(entity,
        engine::ScriptComponent{{meta.id, "game/Check.lua"}, true});
    scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Missing"),
        engine::ScriptComponent{{engine::NewUuid(), "game/missing.lua"}, true});
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    engine::FrameContext frame; frame.deltaSeconds = 0.1;
    f.scripting->Update(frame); f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("Healthy")).position.x, 2);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("Broken")).position.x, -1);
    f.host.StopPlay();
}

TEST(ScriptComponents, PropertyOverridesSurvivePlayStopAndSceneSwitch) {
    PlayFixture f;
    std::string error;
    const auto file = f.root / "Horse.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(file, R"(
        Properties = { horseName = "Horse", speed = 2 }
        function OnStart() Log.info(Properties.horseName) end
        function OnUpdate(dt)
            local p = entity:GetPosition()
            p.x = p.x + Properties.speed * dt
            entity:SetPosition(p)
            Properties.horseName = 'runtime only'
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    engine::SceneDocument next;
    engine::SceneEntityDefinition horse; horse.id = engine::NewUuid(); horse.name = "NextHorse";
    horse.script = engine::ScriptComponent{{meta.id, "game/Horse.lua"}, true, {{"speed", 7}, {"horseName", "Next"}}};
    next.entities.push_back(horse);
    std::string text;
    ASSERT_TRUE(engine::SerializeSceneDocument(next, text, error));
    const auto level = f.root / "scenes/Next.scene.json";
    ASSERT_TRUE(engine::CreateNewFile(level, text, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(level, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Original"),
        engine::ScriptComponent{{meta.id, "game/Horse.lua"}, true, {{"speed", 3}, {"horseName", "Original name"}}});
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    engine::FrameContext frame; frame.deltaSeconds = 1;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("Original")).position.x, 3);
    ASSERT_TRUE(f.host.RequestOpenLevel("Next", error));
    f.host.Tick();
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("NextHorse")).position.x, 7);
    f.host.StopPlay();
    const auto* restored = scene.TryGetComponent<engine::ScriptComponent>(scene.FindFirstByName("Original"));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->properties.at("horseName").String(), "Original name");
    EXPECT_EQ(restored->properties.at("speed").Number(), 3);
}
} // namespace
