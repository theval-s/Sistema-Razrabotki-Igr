#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>
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
        return {
            OnStart = function(self) shared.starts = shared.starts + 1; assert(Scene.OpenLevel('Next')) end
        }
    )", error));
    ASSERT_TRUE(engine::CreateNewFile(f.root / "shared.lua", "return {starts = 0}", error));
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(f.root / "shared.lua", {engine::NewUuid(), engine::CatalogAssetKind::Script}, error));
    ASSERT_TRUE(f.host.Initialize());
    f.host.GetScene().CreateEntity("Editor original");
    const std::vector<engine::AutoloadDefinition> autoloads{{"Game", {meta.id, "game/Game.lua"}, true}};
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, autoloads, error)) << error;
    EXPECT_NE(f.host.GetScene().FindFirstByName("Editor original"), NullEntity);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Next"), NullEntity);
    ASSERT_TRUE(f.scripting->GetRuntime().ExecuteString("assert(Scene.OpenLevel('Last'))", "switch", error));
    EXPECT_NE(f.host.GetScene().FindFirstByName("Next"), NullEntity);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Last"), NullEntity);
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString("assert(require('shared').starts == 1)", "check", error));
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
} // namespace
