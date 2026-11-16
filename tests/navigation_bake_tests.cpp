#include "navigation_bake.hpp"

#include <editor/scene_events.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>

#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/navigation/NavigationMesh.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

/// A temporary project content root, set for the test's lifetime.
struct GameRoot {
    GameRoot() {
        fs::create_directories(path / "scenes");
        engine::AssetPathResolver::SetGameRoot(path);
    }
    ~GameRoot() {
        engine::AssetPathResolver::SetGameRoot(previous);
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path = fs::temp_directory_path() / ("sri-nav-bake-" + engine::NewUuid());
    fs::path previous = engine::AssetPathResolver::GetGameRoot();
};

std::optional<engine::EntityUuid> ownerOf(const fs::path& file) {
    std::vector<std::uint8_t> data;
    std::string error;
    engine::EntityUuid owner;
    engine::NavigationMesh mesh;
    if (!engine::ReadBinaryFile(file, data, error) || !mesh.Deserialize(data, owner, error)) return std::nullopt;
    return owner;
}

/// A floor with a NavMesh region over it, saved as scenes/Level.scene.json and opened.
class NavigationBakeTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(host.AddSubsystem<engine::PhysicsSubsystem>(), nullptr);
        ASSERT_TRUE(host.Initialize());
        engine::SceneEntityDefinition floor;
        floor.id = engine::NewUuid();
        floor.name = "Floor";
        floor.transform.position = {0, -0.5f, 0};
        floor.rigidBody = engine::RigidBody{};
        floor.collider = engine::Collider{.halfExtents = {10, 0.5f, 10}};
        floor.navigationSource = engine::NavigationSource{};
        engine::SceneEntityDefinition region;
        region.id = engine::NewUuid();
        region.name = "NavMesh";
        region.transform.position = {0, 1, 0};
        region.navigationRegion = engine::NavigationRegion{};
        engine::SceneDocument document;
        document.entities = {floor, region};

        std::string text;
        ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error)) << error;
        ASSERT_TRUE(engine::WriteFileAtomic(level, text, error)) << error;
        ASSERT_TRUE(engine::WriteAssetMetadata(level, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
        ASSERT_TRUE(host.RescanAssetCatalogs(error)) << error;
        open();
    }
    void TearDown() override { host.Shutdown(); }

    void open() {
        ASSERT_TRUE(host.LoadScene(level, error)) << error;
        history.clear();
    }
    entt::entity region() { return host.GetScene().FindFirstByName("NavMesh"); }
    const engine::NavigationRegion& component(const entt::entity entity) {
        return *std::as_const(host.GetScene()).TryGetComponent<engine::NavigationRegion>(entity);
    }
    std::string status(const entt::entity entity) { return navigationBakeStatus(host, entity).toStdString(); }

    GameRoot game;
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    editor::SceneEvents events{host.GetScene()};
    editor::Selection selection{host.GetScene(), events};
    editor::SceneHistory history{host, selection, events};
    fs::path scenes = game.path / "scenes";
    fs::path level = scenes / "Level.scene.json";
    std::string error;
};

}  // namespace

TEST_F(NavigationBakeTest, NeedsASavedSceneInTheProject) {
    EXPECT_FALSE(bakeNavigation(host, history, {}, region(), error));
    EXPECT_NE(error.find("Save the scene"), std::string::npos);
    EXPECT_FALSE(bakeNavigation(host, history, fs::temp_directory_path() / "Outside.scene.json", region(), error));
    EXPECT_NE(error.find("outside the project"), std::string::npos);
    EXPECT_EQ(history.stack().count(), 0);
    EXPECT_EQ(host.GetNavigationMesh(region()), nullptr);
    EXPECT_EQ(status(region()), "Not baked");
}

TEST_F(NavigationBakeTest, SavesTheFileAndReferencesItAsOneUndoStep) {
    const auto file = scenes / "Level.NavMesh.navmesh";
    ASSERT_TRUE(bakeNavigation(host, history, level, region(), error)) << error;
    engine::AssetMetadata metadata;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, metadata, error)) << error;
    EXPECT_EQ(metadata.kind, engine::CatalogAssetKind::NavMesh);
    EXPECT_EQ(ownerOf(file), host.GetScene().GetId(region()));
    const engine::AssetReference reference{metadata.id, "game/scenes/Level.NavMesh.navmesh"};
    EXPECT_EQ(component(region()).navMesh, reference);
    ASSERT_EQ(history.stack().count(), 1);
    EXPECT_EQ(history.stack().undoText().toStdString(), "Bake NavMesh");
    EXPECT_EQ(status(region()), "Level.NavMesh.navmesh");

    // Undo drops the reference; the mesh stays in memory until the scene is reopened.
    history.stack().undo();
    EXPECT_FALSE(component(region()).navMesh.id.IsValid());
    EXPECT_EQ(status(region()), "Not saved: bake again");
    history.stack().redo();

    // Baking again rewrites the same file with the same id: nothing to undo.
    ASSERT_TRUE(bakeNavigation(host, history, level, region(), error)) << error;
    EXPECT_EQ(history.stack().count(), 1);
    engine::AssetMetadata again;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, again, error)) << error;
    EXPECT_EQ(again.id, metadata.id);
    EXPECT_EQ(component(region()).navMesh, reference);

    ASSERT_TRUE(host.SaveScene(level, error)) << error;
    open();
    EXPECT_NE(host.GetNavigationMesh(region()), nullptr);
    EXPECT_EQ(status(region()), "Level.NavMesh.navmesh");
    EXPECT_FALSE(host.FindNavigationPath({-8, 0, -8}, {8, 0, 8}).empty());
}

TEST_F(NavigationBakeTest, StatusSaysWhenToBakeAgain) {
    ASSERT_TRUE(bakeNavigation(host, history, level, region(), error)) << error;
    host.GetScene().Modify<engine::NavigationRegion>(region(), [](auto& changed) { changed.settings.agentRadius = 0.6f; });
    EXPECT_EQ(status(region()), "Level.NavMesh.navmesh is out of date: bake again");

    ASSERT_TRUE(host.SaveScene(level, error)) << error;
    fs::remove(scenes / "Level.NavMesh.navmesh");
    fs::remove(scenes / "Level.NavMesh.navmesh.meta");
    ASSERT_TRUE(host.RescanAssetCatalogs(error)) << error;
    open();
    EXPECT_EQ(host.GetNavigationMesh(region()), nullptr);
    EXPECT_EQ(status(region()), "Level.NavMesh.navmesh is missing: bake again");
}

TEST_F(NavigationBakeTest, CopiesAndOtherScenesGetTheirOwnFiles) {
    auto& scene = host.GetScene();
    const auto first = region();
    const auto firstId = scene.GetId(first);
    ASSERT_TRUE(bakeNavigation(host, history, level, first, error)) << error;

    // A duplicate keeps the name and the reference, but not the owner.
    const auto copy = scene.CreateEntity("NavMesh");
    scene.AddComponent<engine::NavigationRegion>(copy, component(first));
    EXPECT_EQ(status(copy), "Level.NavMesh.navmesh isn't loaded: bake again");
    ASSERT_TRUE(bakeNavigation(host, history, level, copy, error)) << error;
    const auto suffix = scene.GetId(copy).ToString().substr(0, 8);
    EXPECT_EQ(component(copy).navMesh.path, "game/scenes/Level.NavMesh." + suffix + ".navmesh");
    EXPECT_EQ(ownerOf(scenes / "Level.NavMesh.navmesh"), firstId);
    EXPECT_EQ(component(first).navMesh.path, "game/scenes/Level.NavMesh.navmesh");

    // Save As keeps entity ids, so the other scene must not reuse this scene's file.
    const auto other = scenes / "Other.scene.json";
    ASSERT_TRUE(bakeNavigation(host, history, other, first, error)) << error;
    EXPECT_EQ(component(first).navMesh.path, "game/scenes/Other.NavMesh.navmesh");
    EXPECT_EQ(ownerOf(scenes / "Level.NavMesh.navmesh"), firstId);

    // A renamed region keeps its file; a new one is named after itself, made file-safe.
    scene.SetName(first, "Renamed");
    EXPECT_EQ(navigationBakeFile(host, other, first).filename().string(), "Other.NavMesh.navmesh");
    const auto upstairs = scene.CreateEntity("Upstairs: east/2");
    scene.AddComponent<engine::NavigationRegion>(upstairs);
    EXPECT_EQ(navigationBakeFile(host, level, upstairs).filename().string(), "Level.Upstairs__east_2.navmesh");
}
