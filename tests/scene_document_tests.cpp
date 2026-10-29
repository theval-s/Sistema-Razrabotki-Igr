#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/engine.hpp>
#include <engine/assets/AssetCatalog.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(SceneDocument, RoundTripAndStructuralValidation) {
    engine::SceneDocument document;
    const std::string cameraId = engine::NewUuid();
    const std::string meshId = engine::NewUuid();
    document.entities.push_back({.id = cameraId, .name = "Camera", .camera = engine::Camera{}});
    document.entities.push_back({.id = meshId, .name = "Child", .parent = cameraId,
                                 .renderer = engine::SceneEntityDefinition::Mesh{
                                     {engine::NewUuid(), "engine/primitives/cube/model.obj"},
                                     {engine::NewUuid(), "game/materials/default.material-instance.json"}}});
    document.activeCamera = cameraId;
    std::string json, error;
    ASSERT_TRUE(engine::SerializeSceneDocument(document, json, error)) << error;
    engine::SceneDocument parsed;
    ASSERT_TRUE(engine::ParseSceneDocument(json, parsed, error)) << error;
    ASSERT_EQ(parsed.entities.size(), 2u);
    EXPECT_EQ(parsed.entities[1].parent, cameraId);
    EXPECT_EQ(parsed.entities[1].renderer->mesh.path, "engine/primitives/cube/model.obj");
    EXPECT_EQ(parsed.activeCamera, cameraId);

    parsed.entities[0].parent = meshId;
    EXPECT_FALSE(engine::SerializeSceneDocument(parsed, json, error));
    EXPECT_NE(error.find("cycle"), std::string::npos);
    parsed.entities[0].parent.reset();
    parsed.entities[1].id = cameraId;
    EXPECT_FALSE(engine::SerializeSceneDocument(parsed, json, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);
}

TEST(SceneDocument, RejectsUnknownComponentsAndPreservesOutputOnFailure) {
    engine::SceneDocument scene;
    const std::string invalid = R"({"version":1,"entities":[{"id":"invalid","name":"x","components":{}}]})";
    std::string error;
    EXPECT_FALSE(engine::ParseSceneDocument(invalid, scene, error));
    EXPECT_TRUE(scene.entities.empty());
    const std::string unknown = R"({"version":1,"entities":[{"id":"11111111-1111-1111-1111-111111111111","name":"x","components":{"Unknown":{}}}]})";
    EXPECT_FALSE(engine::ParseSceneDocument(unknown, scene, error));
    EXPECT_NE(error.find("Unknown"), std::string::npos);
}

TEST(SceneDocument, SceneOwnsIdentifiersHierarchyAndRequiredComponents) {
    engine::Scene scene;
    const auto parentId = engine::NewUuid();
    const auto parent = scene.CreateEntityWithId(parentId, "Parent");
    const auto child = scene.CreateEntity("Child", parent);
    ASSERT_TRUE(parent != entt::null);
    ASSERT_TRUE(child != entt::null);
    const std::string childId(scene.GetId(child));
    EXPECT_EQ(scene.FindById(parentId), parent);
    EXPECT_EQ(scene.GetParent(child), parent);
    EXPECT_TRUE(scene.CreateEntityWithId(parentId) == entt::null);
    const auto revision = scene.GetRevision();
    EXPECT_NE(scene.AddComponent<engine::Camera>(child), nullptr);
    EXPECT_GT(scene.GetRevision(), revision);
    EXPECT_TRUE(scene.HasComponent<engine::Camera>(child));
    EXPECT_TRUE(scene.RemoveComponent<engine::Camera>(child));
    struct NotSerializable { int value; };
    ASSERT_NE(scene.AddComponent<NotSerializable>(child, 42), nullptr);
    EXPECT_TRUE(scene.HasUnsupportedComponents());
    EXPECT_TRUE(scene.RemoveComponent<NotSerializable>(child));
    EXPECT_FALSE(scene.HasUnsupportedComponents());
    scene.DestroyEntity(parent);
    EXPECT_TRUE(scene.FindById(parentId) == entt::null);
    EXPECT_TRUE(scene.FindById(childId) == entt::null);
}

TEST(SceneDocument, CodeOnlySaveLoadAndMalformedFilePreservesScene) {
    engine::EngineConfig config;
    config.taskThreads = 1;
    engine::Engine engine(config);
    ASSERT_TRUE(engine.Initialize());
    const auto id = engine::NewUuid();
    const auto camera = engine.GetScene().CreateEntityWithId(id, "Saved camera");
    engine.GetScene().AddComponent<engine::Camera>(camera);
    engine.GetScene().SetActiveCamera(camera);
    const auto file = std::filesystem::temp_directory_path() / ("sri-scene-" + engine::NewUuid() + ".scene.json");
    std::string error;
    ASSERT_TRUE(engine.SaveScene(file, error)) << error;
    ASSERT_TRUE(engine.SaveScene(file, error)) << "atomic replacement: " << error;
    engine.GetScene().Clear();
    ASSERT_TRUE(engine.LoadScene(file, error)) << error;
    EXPECT_TRUE(engine.GetScene().FindById(id) != entt::null);
    EXPECT_EQ(engine.GetScene().GetActiveCamera(), engine.GetScene().FindById(id));
    std::ofstream(file, std::ios::trunc) << R"({"version":1,"entities":[{"bad":true}]})";
    EXPECT_FALSE(engine.LoadScene(file, error));
    EXPECT_EQ(engine.GetScene().GetName(engine.GetScene().FindById(id)), "Saved camera");
    engine.Shutdown();
    std::filesystem::remove(file);
}

TEST(SceneDocument, MissingAssetsKeepAuthoredReferencesOnSave) {
    engine::EngineConfig config;
    config.taskThreads = 1;
    engine::Engine engine(config);
    ASSERT_TRUE(engine.Initialize());
    engine::SceneDocument document;
    engine::SceneEntityDefinition item;
    item.id = engine::NewUuid();
    item.name = "Missing mesh";
    item.renderer = engine::SceneEntityDefinition::Mesh{
        {engine::NewUuid(), "game/models/lost.obj"},
        {engine::NewUuid(), "game/materials/lost.material-instance.json"}};
    document.entities.push_back(item);
    engine::PopulateScene(document, engine.GetSceneAssets(), engine.GetScene());
    engine::SceneDocument saved;
    std::string error;
    ASSERT_TRUE(engine::CaptureSceneDocument(engine.GetScene(), engine.GetSceneAssets(), saved, error)) << error;
    EXPECT_EQ(saved.entities.at(0).renderer->mesh.id, item.renderer->mesh.id);
    EXPECT_EQ(saved.entities.at(0).renderer->materialInstance.path, item.renderer->materialInstance.path);
    engine.Shutdown();
}

TEST(SceneDocument, ExampleGameSceneResolvesAllAssetReferences) {
    const auto root = std::filesystem::path(SRI_SOURCE_ROOT);
    const auto file = root / "example_game/scenes/demo.scene.json";
    std::ifstream input(file);
    ASSERT_TRUE(input);
    const std::string text(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    engine::SceneDocument document;
    std::string error;
    ASSERT_TRUE(engine::ParseSceneDocument(text, document, error)) << error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(root / "example_game", root / "engine", error)) << error;
    for (const auto& entity : document.entities) {
        if (!entity.renderer) continue;
        EXPECT_NE(catalog.Find(entity.renderer->mesh.id, engine::CatalogAssetKind::Mesh), nullptr) << entity.name;
        EXPECT_NE(catalog.Find(entity.renderer->materialInstance.id, engine::CatalogAssetKind::MaterialInstance), nullptr)
            << entity.name;
    }
}
