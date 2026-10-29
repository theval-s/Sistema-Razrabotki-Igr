#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/scene/GameDocument.hpp>
#include <engine/engine.hpp>
#include <engine/assets/AssetCatalog.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace {
struct CountingController final : engine::Controller {
    explicit CountingController(std::vector<std::string>& events) : events(events) {}
    void OnCreate() override { events.push_back("create"); }
    void OnEnable() override { events.push_back("enable"); }
    void Update(double) override { events.push_back("update"); GetScene().DestroyEntity(GetEntity()); }
    void OnDisable() override { events.push_back("disable"); }
    void OnDestroy() override { events.push_back(GetScene().IsValid(GetEntity()) ? "destroy-valid" : "destroy-invalid"); }
    std::vector<std::string>& events;
};
struct RestorableController final : engine::Controller {
    explicit RestorableController(std::vector<std::string>& events) : events(events) {}
    void LoadAuthored(const engine::StateValue& data) override { speed = data.Number(); events.push_back("authored"); }
    void SaveAuthored(engine::StateValue& data) const override { data = speed; }
    void OnCreate() override { events.push_back("create"); }
    void LoadGameState(const engine::StateValue& data) override {
        score = static_cast<int>(data.At("score").Number()); events.push_back("restore");
    }
    void SaveGameState(engine::StateValue& data) const override { data["score"] = score; }
    void OnEnable() override {
        events.push_back(score == 42 && GetScene().GetTransform(GetEntity()).position.x == 9 ? "enabled-restored" : "enabled-start");
    }
    std::vector<std::string>& events;
    double speed = 0;
    int score = 0;
};
struct CommandController final : engine::Controller {
    CommandController(std::vector<std::string>& events, entt::entity target, bool issuer)
        : events(events), target(target), issuer(issuer) {}
    void OnCreate() override { events.push_back(issuer ? "issuer-create" : "target-create"); }
    void OnEnable() override { events.push_back(issuer ? "issuer-enable" : "target-enable"); }
    void Update(double) override {
        events.push_back(issuer ? "issuer-update" : "target-update");
        if (!issuer || issued) return;
        issued = true;
        auto& scene = GetScene();
        scene.AttachController(target, {"test/target"});
        scene.SetControllerEnabled(target, false);
        scene.DestroyEntity(GetEntity());
    }
    void OnDisable() override { events.push_back(issuer ? "issuer-disable" : "target-disable"); }
    void OnDestroy() override {
        events.push_back(issuer ? "issuer-destroy" : "target-destroy");
        if (issuer) GetScene().SetControllerEnabled(target, true);
    }
    std::vector<std::string>& events;
    entt::entity target;
    bool issuer;
    bool issued = false;
};
}

TEST(SceneDocument, ControllerCommandsFlushInFifoOrderWithoutReentry) {
    engine::Scene scene;
    std::vector<std::string> events;
    const auto issuer = scene.CreateEntity("issuer");
    const auto target = scene.CreateEntity("target");
    ASSERT_TRUE(scene.RegisterController("test/issuer", {1, {}, [&] {
        return std::make_unique<CommandController>(events, target, true);
    }}));
    ASSERT_TRUE(scene.RegisterController("test/target", {1, {}, [&] {
        return std::make_unique<CommandController>(events, target, false);
    }}));
    scene.AttachController(issuer, {"test/issuer"});
    scene.SetControllersActive(true);
    scene.UpdateControllers(0.1);
    EXPECT_FALSE(scene.IsValid(issuer));
    ASSERT_NE(scene.GetControllerDefinition(target), nullptr);
    EXPECT_TRUE(scene.GetControllerDefinition(target)->enabled);
    EXPECT_EQ(events, (std::vector<std::string>{"issuer-create", "issuer-enable", "issuer-update",
        "target-create", "target-enable", "target-disable", "issuer-disable", "issuer-destroy", "target-enable"}));
    scene.UpdateControllers(0.1);
    EXPECT_EQ(events.back(), "target-update"); // attached during the preceding frame
}

TEST(SceneDocument, ClearAndRemoveRequestedDuringEnableWaitForCallback) {
    struct ClearController final : engine::Controller {
        explicit ClearController(std::vector<std::string>& events) : events(events) {}
        void OnEnable() override {
            events.push_back("enable");
            GetScene().RemoveController(GetEntity());
            GetScene().Clear();
            events.push_back(GetScene().IsValid(GetEntity()) ? "still-valid" : "gone-early");
        }
        void OnDisable() override { events.push_back("disable"); }
        void OnDestroy() override { events.push_back("destroy"); }
        std::vector<std::string>& events;
    };
    engine::Scene scene;
    std::vector<std::string> events;
    const auto entity = scene.CreateEntity("cleared");
    ASSERT_TRUE(scene.RegisterController("test/clear", {1, {}, [&] {
        return std::make_unique<ClearController>(events);
    }}));
    scene.AttachController(entity, {"test/clear"});
    scene.SetControllersActive(true);
    EXPECT_EQ(events, (std::vector<std::string>{"enable", "still-valid", "disable", "destroy"}));
    EXPECT_TRUE(scene.GetRootEntities().empty());
}

TEST(SceneDocument, MissingControllerPreservesAuthoredDataAndCanBeRepaired) {
    engine::Scene scene;
    const auto entity = scene.CreateEntity("Repeated");
    const auto other = scene.CreateEntity("Repeated");
    scene.AddTag(entity, "animated");
    scene.AddTag(other, "animated");
    EXPECT_EQ(scene.FindFirstByName("Repeated"), entity);
    EXPECT_EQ(scene.FindAllByName("Repeated"), (std::vector<entt::entity>{entity, other}));
    EXPECT_EQ(scene.FindWithTag("animated").size(), 2u);
    const engine::ControllerDefinition missing{"game/unknown", 1, true,
        engine::StateValue::Object{{"key", engine::StateValue::Array{1, 2}}}};
    scene.AttachController(entity, missing);
    engine::SceneDocument captured;
    std::string error, json;
    engine::Engine host;
    ASSERT_TRUE(engine::CaptureSceneDocument(scene, host.GetSceneAssets(), captured, error)) << error;
    ASSERT_TRUE(engine::SerializeSceneDocument(captured, json, error)) << error;
    engine::SceneDocument parsed;
    ASSERT_TRUE(engine::ParseSceneDocument(json, parsed, error)) << error;
    EXPECT_EQ(parsed.entities[0].controller->data.At("key").Items().size(), 2u);
    EXPECT_EQ(parsed.entities[0].tags, (std::vector<std::string>{"animated"}));
    std::vector<std::string> events;
    ASSERT_TRUE(scene.RegisterController("game/unknown", {1, {}, [&events]() {
        return std::make_unique<CountingController>(events);
    }}));
    scene.SetControllersActive(true);
    scene.UpdateControllers(0); // pause
    scene.UpdateControllers(0.1);
    EXPECT_EQ(events, (std::vector<std::string>{"create", "enable", "update", "disable", "destroy-valid"}));
    EXPECT_FALSE(scene.IsValid(entity));

    ASSERT_TRUE(scene.RegisterController("game/rejecting", {1,
        [](const engine::StateValue&, std::string& error) { error = "invalid payload"; return false; },
        [&events]() { return std::make_unique<CountingController>(events); }}));
    scene.AttachController(other, {"game/rejecting", 1, false, engine::StateValue::Object{{"repairMe", true}}});
    ASSERT_NE(scene.GetControllerDefinition(other), nullptr);
    EXPECT_TRUE(scene.GetControllerDefinition(other)->data.At("repairMe").Boolean());
    EXPECT_EQ(events.size(), 5u); // invalid payload did not construct a live controller
}

TEST(SceneDocument, BoundsQueriesAccountForParentRotationAndBoundary) {
    engine::Engine host;
    engine::Scene& scene = host.GetScene();
    const auto parent = scene.CreateEntity("parent");
    scene.GetTransform(parent).position = {3, 0, 0};
    scene.GetTransform(parent).rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(1.5707963f, 0, 0);
    const auto target = scene.CreateEntity("target", parent);
    scene.AddComponent<engine::Bounds>(target, engine::Aabb{{-1, -1, -2}, {1, 1, 2}});
    const auto hits = engine::QuerySphere(scene, host.GetSceneAssets(), {5, 0, 0}, 0);
    EXPECT_EQ(hits, (std::vector<entt::entity>{target}));
    const auto ray = engine::RaycastAll(scene, host.GetSceneAssets(), {0, 0, 0}, {10, 0, 0}, 10);
    ASSERT_EQ(ray.size(), 1u);
    EXPECT_EQ(ray[0].entity, target);
    EXPECT_NEAR(ray[0].distance, 1, 1e-4);
    EXPECT_TRUE(engine::RaycastAll(scene, host.GetSceneAssets(), {0, 0, 0}, {}, 10).empty());
    scene.DestroyEntity(parent);
    EXPECT_TRUE(engine::QuerySphere(scene, host.GetSceneAssets(), {5, 0, 0}, 0).empty());
}

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
    engine.GetScene().AttachController(camera, {"unknown/controller", 3, false,
        engine::StateValue::Object{{"initial", 4}}});
    engine.GetScene().SetActiveCamera(camera);
    const auto file = std::filesystem::temp_directory_path() / ("sri-scene-" + engine::NewUuid() + ".scene.json");
    std::string error;
    ASSERT_TRUE(engine.SaveScene(file, error)) << error;
    ASSERT_TRUE(engine.SaveScene(file, error)) << "atomic replacement: " << error;
    engine.GetScene().Clear();
    ASSERT_TRUE(engine.LoadScene(file, error)) << error;
    EXPECT_NE(error.find("unknown/controller"), std::string::npos);
    EXPECT_TRUE(engine.GetScene().FindById(id) != entt::null);
    EXPECT_EQ(engine.GetScene().GetActiveCamera(), engine.GetScene().FindById(id));
    ASSERT_NE(engine.GetScene().GetControllerDefinition(engine.GetScene().FindById(id)), nullptr);
    EXPECT_EQ(engine.GetScene().GetControllerDefinition(engine.GetScene().FindById(id))->data.At("initial").Number(), 4);
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

TEST(GameDocument, SavesOnlyOptedInStateWithSpawnsAndTombstones) {
    engine::EngineConfig config;
    config.taskThreads = 1;
    engine::Engine host(config);
    ASSERT_TRUE(host.Initialize());
    std::string error;
    ASSERT_TRUE(host.LoadScene(engine::AssetReference{"2a9a7998-f44d-44d8-8fc3-57e78da18796",
        "game/scenes/demo.scene.json"}, error)) << error;
    const auto ground = host.GetScene().FindFirstByName("Ground");
    ASSERT_TRUE(ground != entt::null);
    const auto groundHits = engine::RaycastAll(host.GetScene(), host.GetSceneAssets(),
        {12, 3, 12}, {0, -1, 0}, 10);
    ASSERT_FALSE(groundHits.empty());
    EXPECT_EQ(groundHits.front().entity, ground);
    ASSERT_TRUE(host.GetScene().TryGetComponent<engine::GamePersistence>(host.GetScene().FindFirstByName("Sign")));
    auto& scene = host.GetScene();
    const auto signId = std::string(scene.GetId(scene.FindFirstByName("Sign")));
    const auto tableId = std::string(scene.GetId(scene.FindFirstByName("Table")));
    scene.GetTransform(scene.FindById(signId)).position = {7, 8, 9};
    scene.GetTransform(scene.FindById(tableId)).position = {100, 100, 100}; // not persisted
    struct TransientCache { int value; };
    ASSERT_NE(scene.AddComponent<TransientCache>(scene.FindById(tableId), 123), nullptr);
    std::string detail;
    ASSERT_TRUE(scene.RestoreControllerGameState(scene.FindById(signId),
        engine::StateValue::Object{{"goal", "trade"}}, detail));
    const auto spawned = scene.CreateEntity("Spawned NPC", scene.FindById(tableId));
    const auto spawnedId = std::string(scene.GetId(spawned));
    ASSERT_NE(scene.AddComponent<engine::GamePersistence>(spawned, engine::GamePersistence{true, false}), nullptr);
    scene.GetTransform(spawned).position = {3, 4, 5};
    const auto child = scene.CreateEntity("Spawned child", spawned);
    const auto childId = std::string(scene.GetId(child));
    ASSERT_NE(scene.AddComponent<engine::GamePersistence>(child, engine::GamePersistence{true, false}), nullptr);
    scene.DestroyEntity(scene.FindFirstByName("Hammer"));
    const auto file = std::filesystem::temp_directory_path() / ("sri-game-" + engine::NewUuid() + ".save.json");
    ASSERT_TRUE(host.SaveGame(file, error)) << error;
    std::ifstream input(file);
    const std::string text(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    input.close();
    engine::GameDocument document;
    ASSERT_TRUE(engine::ParseGameDocument(text, document, error)) << error;
    ASSERT_EQ(document.spawned.entities.size(), 2u);
    EXPECT_EQ(document.spawned.entities[0].id, spawnedId);
    EXPECT_EQ(document.spawnParents.at(spawnedId), tableId);
    EXPECT_EQ(document.spawnParents.at(childId), spawnedId);
    EXPECT_EQ(document.deleted.size(), 1u);
    EXPECT_EQ(document.entities.size(), 3u); // sign + two spawns, not table/terrain
    ASSERT_TRUE(host.LoadGame(file, error)) << error;
    EXPECT_TRUE(scene.FindFirstByName("Hammer") == entt::null);
    EXPECT_EQ(scene.GetTransform(scene.FindById(signId)).position,
              (DirectX::SimpleMath::Vector3{7, 8, 9}));
    EXPECT_EQ(scene.GetTransform(scene.FindById(tableId)).position,
              (DirectX::SimpleMath::Vector3{-2.5f, 0.55f, 0}));
    EXPECT_TRUE(scene.FindById(spawnedId) != entt::null);
    EXPECT_EQ(scene.GetParent(scene.FindById(spawnedId)), scene.FindById(tableId));
    EXPECT_EQ(scene.GetParent(scene.FindById(childId)), scene.FindById(spawnedId));
    const auto retained = scene.CaptureControllerGameState(scene.FindById(signId), detail);
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->At("goal").String(), "trade");
    ASSERT_TRUE(host.SaveGame(file, error)) << error;
    ASSERT_TRUE(host.LoadGame(file, error)) << error;
    EXPECT_TRUE(scene.FindById(spawnedId) != entt::null);
    document.scene.id = engine::NewUuid();
    std::string invalid;
    ASSERT_TRUE(engine::SerializeGameDocument(document, invalid, error)) << error;
    std::ofstream(file, std::ios::trunc) << invalid;
    EXPECT_FALSE(host.LoadGame(file, error));
    EXPECT_TRUE(scene.FindById(spawnedId) != entt::null);
    host.Shutdown();
    std::filesystem::remove(file);
}

TEST(GameDocument, RestoresRegisteredControllerBeforeEnable) {
    engine::EngineConfig config;
    config.taskThreads = 1;
    engine::Engine host(config);
    std::vector<std::string> events;
    ASSERT_TRUE(host.GetScene().RegisterController("example/sign", {1,
        [](const engine::StateValue& value, std::string& error) {
            if (value.Number() > 0) return true;
            error = "bad speed"; return false;
        }, [&events] { return std::make_unique<RestorableController>(events); }}));
    ASSERT_TRUE(host.Initialize());
    std::string error;
    ASSERT_TRUE(host.LoadScene("game/scenes/demo.scene.json", error));
    const auto sign = host.GetScene().FindFirstByName("Sign");
    ASSERT_NE(host.GetScene().GetControllerDefinition(sign), nullptr);
    host.GetScene().GetTransform(sign).position.x = 9;
    ASSERT_TRUE(host.GetScene().RestoreControllerGameState(sign, engine::StateValue::Object{{"score", 42}}, error));
    const auto file = std::filesystem::temp_directory_path() / ("sri-game-" + engine::NewUuid() + ".save.json");
    ASSERT_TRUE(host.SaveGame(file, error)) << error;
    events.clear();
    ASSERT_TRUE(host.LoadGame(file, error)) << error;
    EXPECT_EQ(events, (std::vector<std::string>{"authored", "create", "restore", "enabled-restored"}));
    EXPECT_EQ(host.GetScene().GetControllerDefinition(host.GetScene().FindFirstByName("Sign"))->data.Number(), 0.3);
    host.Shutdown();
    std::filesystem::remove(file);
}
