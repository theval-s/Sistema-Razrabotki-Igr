#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/navigation/NavigationMesh.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <gtest/gtest.h>

namespace {
using DirectX::SimpleMath::Vector3;

const Vector3 From{-8, 0, -8};
const Vector3 To{8, 0, 8};

engine::NavigationMesh Floor() {
    const std::vector<Vector3> vertices{{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}};
    const std::vector<int> indices{0, 2, 1, 0, 3, 2};
    engine::NavigationMesh mesh;
    EXPECT_TRUE(mesh.Build(vertices, indices));
    return mesh;
}
}  // namespace

TEST(NavigationMeshFile, RoundTripKeepsPathsSettingsAndOwner) {
    const auto mesh = Floor();
    const engine::EntityUuid owner(engine::NewUuid());
    const auto file = mesh.Serialize(owner);
    ASSERT_FALSE(file.empty());

    engine::NavigationMesh loaded;
    engine::EntityUuid read;
    std::string error;
    ASSERT_TRUE(loaded.Deserialize(file, read, error)) << error;
    EXPECT_EQ(read, owner);
    EXPECT_FALSE(loaded.FindPath(From, To).empty());
    EXPECT_EQ(loaded.FindPath(From, To), mesh.FindPath(From, To));
    EXPECT_EQ(loaded.Serialize(owner), file);
    EXPECT_TRUE(loaded.RegisterAgent(owner, From, 3));
    EXPECT_TRUE(engine::NavigationMesh{}.Serialize(owner).empty());
}

TEST(NavigationMeshFile, RejectsDamagedFilesAndKeepsTheMesh) {
    auto mesh = Floor();
    const auto file = mesh.Serialize(engine::EntityUuid(engine::NewUuid()));
    const auto path = mesh.FindPath(From, To);
    engine::EntityUuid read;
    std::string error;

    EXPECT_FALSE(mesh.Deserialize({}, read, error));
    EXPECT_EQ(error, "not a .navmesh file");
    auto version = file;
    version[8] = 99;
    EXPECT_FALSE(mesh.Deserialize(version, read, error));
    EXPECT_EQ(error, "unsupported .navmesh version");
    auto truncated = file;
    truncated.pop_back();
    EXPECT_FALSE(mesh.Deserialize(truncated, read, error));
    EXPECT_EQ(error, "damaged .navmesh file");
    auto flipped = file;
    flipped[flipped.size() / 2] ^= 1;
    EXPECT_FALSE(mesh.Deserialize(flipped, read, error));
    EXPECT_EQ(error, "damaged .navmesh file");
    EXPECT_FALSE(read.IsValid());
    EXPECT_EQ(mesh.FindPath(From, To), path);
}

TEST(NavigationScene, BakedMeshesLoadWithTheScene) {
    const auto root = std::filesystem::temp_directory_path() / ("sri-nav-" + engine::NewUuid());
    const auto previous = engine::AssetPathResolver::GetGameRoot();
    std::filesystem::create_directories(root / "scenes");
    engine::AssetPathResolver::SetGameRoot(root);
    std::string error;
    {
        engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
        ASSERT_NE(host.AddSubsystem<engine::PhysicsSubsystem>(), nullptr);
        ASSERT_TRUE(host.Initialize());

        engine::SceneDocument document;
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
        document.entities = {floor, region};

        const auto sceneFile = root / "scenes" / "Level.scene.json";
        const auto write = [&] {
            std::string text;
            ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error)) << error;
            ASSERT_TRUE(engine::WriteFileAtomic(sceneFile, text, error)) << error;
        };
        write();
        ASSERT_TRUE(engine::WriteAssetMetadata(sceneFile, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
        ASSERT_TRUE(host.RescanAssetCatalogs(error)) << error;
        ASSERT_TRUE(host.LoadScene(sceneFile, error)) << error;
        auto& scene = host.GetScene();
        EXPECT_TRUE(host.FindNavigationPath(From, To).empty());

        const auto bakedFile = root / "scenes" / "Level.NavMesh.navmesh";
        EXPECT_FALSE(host.SaveNavigation(scene.FindFirstByName("NavMesh"), bakedFile, error));
        ASSERT_TRUE(host.BuildNavigation(scene.FindFirstByName("NavMesh"), error)) << error;
        ASSERT_TRUE(host.SaveNavigation(scene.FindFirstByName("NavMesh"), bakedFile, error)) << error;
        const engine::AssetMetadata baked{engine::NewUuid(), engine::CatalogAssetKind::NavMesh};
        ASSERT_TRUE(engine::WriteAssetMetadata(bakedFile, baked, error));
        ASSERT_TRUE(host.RescanAssetCatalogs(error)) << error;

        // A copied region keeps the reference but not the owner; another points at nothing.
        document.entities[1].navigationRegion->navMesh = {baked.id, "game/scenes/Level.NavMesh.navmesh"};
        auto copy = document.entities[1];
        copy.id = engine::NewUuid();
        copy.name = "Copy";
        auto missing = document.entities[1];
        missing.id = engine::NewUuid();
        missing.name = "Missing";
        missing.navigationRegion->navMesh = {engine::AssetUuid(engine::NewUuid()), "game/scenes/Gone.navmesh"};
        document.entities.push_back(copy);
        document.entities.push_back(missing);
        write();

        std::string text;
        ASSERT_TRUE(engine::ReadTextFile(sceneFile, text, error)) << error;
        EXPECT_NE(text.find("\"navMesh\""), std::string::npos);
        engine::SceneDocument parsed;
        ASSERT_TRUE(engine::ParseSceneDocument(text, parsed, error)) << error;
        EXPECT_EQ(parsed.entities, document.entities);

        ASSERT_TRUE(host.LoadScene(sceneFile, error)) << error;
        EXPECT_NE(host.GetNavigationMesh(scene.FindFirstByName("NavMesh")), nullptr);
        EXPECT_EQ(host.GetNavigationMesh(scene.FindFirstByName("Copy")), nullptr);
        EXPECT_EQ(host.GetNavigationMesh(scene.FindFirstByName("Missing")), nullptr);
        EXPECT_FALSE(host.FindNavigationPath(From, To).empty());
        host.Shutdown();
    }
    engine::AssetPathResolver::SetGameRoot(previous);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
