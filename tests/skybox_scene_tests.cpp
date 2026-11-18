#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <limits>
#include "content_fixture.hpp"

namespace {
using namespace engine;
using Json = nlohmann::json;
constexpr std::string_view Minimal = R"({"version":1,"entities":[]})";

TEST(SkyboxScene, OldScenesAndOptionalFieldsUseDefaults) {
    SceneDocument document;
    document.environment.brightness = 42;
    std::string text, error;
    ASSERT_TRUE(ParseSceneDocument(Minimal, document, error)) << error;
    EXPECT_EQ(document.environment, EnvironmentSettings{});
    for (const auto& environment : {Json::object(), Json{{"skybox", nullptr}}, Json{{"brightness", 1}}}) {
        auto root = Json::parse(Minimal);
        root["environment"] = environment;
        ASSERT_TRUE(ParseSceneDocument(root.dump(), document, error)) << error;
        EXPECT_EQ(document.environment, EnvironmentSettings{});
    }
    ASSERT_TRUE(SerializeSceneDocument(document, text, error)) << error;
    EXPECT_EQ(Json::parse(text).at("version"), 1);
    EXPECT_FALSE(Json::parse(text).contains("environment"));
}

TEST(SkyboxScene, HdrAndUuidOnlyReferencesRoundTripWithoutResolution) {
    auto root = Json::parse(Minimal);
    const auto id = NewUuid();
    root["environment"] = {{"skybox", {{"id", id}}}, {"yawRadians", -100}, {"brightness", 12},
        {"tint", {2, 0, 4}}, {"fallbackColor", {5, 6, 7}}};
    SceneDocument document, decoded;
    std::string text, error;
    ASSERT_TRUE(ParseSceneDocument(root.dump(), document, error)) << error;
    EXPECT_EQ(document.environment.skybox.id, AssetUuid(id));
    EXPECT_TRUE(document.environment.skybox.path.empty());
    ASSERT_TRUE(SerializeSceneDocument(document, text, error)) << error;
    EXPECT_FALSE(Json::parse(text).at("environment").at("skybox").contains("path"));
    ASSERT_TRUE(ParseSceneDocument(text, decoded, error)) << error;
    EXPECT_EQ(decoded.environment, document.environment);
    document.environment.skybox.path = "game/diagnostic-only.skybox.json";
    ASSERT_TRUE(SerializeSceneDocument(document, text, error)) << error;
    ASSERT_TRUE(ParseSceneDocument(text, decoded, error)) << error;
    EXPECT_EQ(decoded.environment, document.environment);
    std::string canonical;
    ASSERT_TRUE(SerializeSceneDocument(decoded, canonical, error)) << error;
    EXPECT_EQ(text, canonical);
}

TEST(SkyboxScene, OptionalDiagnosticHintsAndProgrammaticUuidOnlySettingsRoundTrip) {
    const AssetUuid id(NewUuid());
    std::string text, error;
    for (const auto& reference : {Json{{"id", id.ToString()}},
        Json{{"id", id.ToString()}, {"path", ""}},
        Json{{"id", id.ToString()}, {"path", "game/old-name.skybox.json"}},
        Json{{"id", id.ToString()}, {"path", "old diagnostic hint"}}}) {
        SCOPED_TRACE(reference.dump());
        auto root = Json::parse(Minimal);
        root["environment"] = {{"skybox", reference}};
        SceneDocument document, decoded;
        ASSERT_TRUE(ParseSceneDocument(root.dump(), document, error)) << error;
        EXPECT_EQ(document.environment.skybox.id, id);
        EXPECT_EQ(document.environment.skybox.path, reference.value("path", std::string{}));
        ASSERT_TRUE(SerializeSceneDocument(document, text, error)) << error;
        const auto encodedReference = Json::parse(text).at("environment").at("skybox");
        EXPECT_EQ(encodedReference.at("id"), id.ToString());
        if (document.environment.skybox.path.empty()) EXPECT_FALSE(encodedReference.contains("path"));
        else EXPECT_EQ(encodedReference.at("path"), document.environment.skybox.path);
        ASSERT_TRUE(ParseSceneDocument(text, decoded, error)) << error;
        EXPECT_EQ(decoded.environment, document.environment);
    }
    Engine host;
    EnvironmentSettings settings;
    settings.skybox.id = id;
    ASSERT_TRUE(host.GetScene().SetEnvironment(settings, error)) << error;
    SceneDocument captured, decoded;
    ASSERT_TRUE(CaptureSceneDocument(host.GetScene(), host.GetSceneAssets(), captured, error)) << error;
    ASSERT_TRUE(SerializeSceneDocument(captured, text, error)) << error;
    ASSERT_TRUE(ParseSceneDocument(text, decoded, error)) << error;
    EXPECT_EQ(decoded.environment, settings);
    Scene populated;
    PopulateScene(decoded, host.GetSceneAssets(), populated);
    EXPECT_EQ(populated.GetEnvironment(), settings);
}

TEST(SkyboxScene, InvalidCodecInputIsRejectedAtomically) {
    SceneDocument document;
    document.environment.brightness = 7;
    const auto before = document.environment;
    std::string error;
    const auto reject = [&](const Json& environment) {
        auto root = Json::parse(Minimal);
        root["environment"] = environment;
        EXPECT_FALSE(ParseSceneDocument(root.dump(), document, error)) << root;
        EXPECT_EQ(document.environment, before);
        EXPECT_FALSE(error.empty());
    };
    reject(nullptr);
    reject(Json::array());
    for (const auto& [name, value] : std::vector<std::pair<std::string, Json>>{
        {"unknown", 0}, {"brightness", -1}, {"brightness", true}, {"yawRadians", "0"},
        {"tint", {-1, 1, 1}}, {"tint", {1, 2}}, {"fallbackColor", {0, -1, 0}},
        {"skybox", "game/sky.skybox.json"}, {"skybox", {{"path", "game/sky.skybox.json"}}},
        {"skybox", {{"id", "bad"}}}, {"skybox", {{"id", ""}}}, {"skybox", {{"id", 1}}},
        {"skybox", {{"id", "00000000-0000-0000-0000-000000000000"}}},
        {"skybox", {{"id", NewUuid()}, {"path", nullptr}}},
        {"skybox", {{"id", NewUuid()}, {"path", 42}}},
        {"skybox", {{"id", NewUuid()}, {"extra", 0}}}})
        reject(Json{{name, value}});
    EXPECT_FALSE(ParseSceneDocument(R"({"version":1,"entities":[],"environment":{"brightness":1,"brightness":2}})", document, error));
    EXPECT_FALSE(ParseSceneDocument(R"({"version":1,"entities":[],"environment":{"yawRadians":1e999}})", document, error));
    const auto duplicateReference = std::string(R"({"version":1,"entities":[],"environment":{"skybox":{"id":")") +
        NewUuid() + R"(","path":"first","path":"second"}}})";
    EXPECT_FALSE(ParseSceneDocument(duplicateReference, document, error));
    EXPECT_EQ(document.environment, before);
    for (const auto invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -1.0f}) {
        document.environment.brightness = invalid;
        std::string text = "unchanged";
        EXPECT_FALSE(SerializeSceneDocument(document, text, error));
        EXPECT_EQ(text, "unchanged");
    }
}

TEST(SkyboxScene, ValidationNotificationsAndSettingsHistory) {
    Engine host;
    auto& scene = host.GetScene();
    std::string error = "old error";
    const auto before = CaptureSceneSettings(scene);
    const auto revision = scene.GetRevision();
    unsigned changes = 0;
    const auto observer = scene.AddChangeObserver([&](Scene::ChangeKind kind, entt::entity entity) {
        EXPECT_EQ(kind, Scene::ChangeKind::Settings);
        EXPECT_TRUE(entity == entt::null);
        ++changes;
    });
    EnvironmentSettings value;
    value.skybox = {NewUuid(), "game/missing.skybox.json"};
    value.yawRadians = -20;
    value.tint = {8, 2, 0};
    ASSERT_TRUE(scene.SetEnvironment(value, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(changes, 1u);
    EXPECT_EQ(scene.GetRevision(), revision);
    ASSERT_TRUE(scene.SetEnvironment(value, error));
    EXPECT_EQ(changes, 1u);
    const auto after = CaptureSceneSettings(scene);
    EXPECT_NE(before, after);
    auto invalid = value;
    invalid.fallbackColor.z = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(scene.SetEnvironment(invalid, error));
    EXPECT_EQ(scene.GetEnvironment(), value);
    invalid = value;
    invalid.skybox.id = {};
    EXPECT_FALSE(scene.SetEnvironment(invalid, error));
    invalid = value;
    invalid.yawRadians = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(scene.SetEnvironment(invalid, error));
    EXPECT_EQ(changes, 1u);
    ApplySceneSettings(before, scene);
    EXPECT_EQ(scene.GetEnvironment(), EnvironmentSettings{});
    ApplySceneSettings(after, scene);
    ApplySceneSettings(after, scene);
    EXPECT_EQ(scene.GetEnvironment(), value);
    EXPECT_EQ(changes, 3u);
    EXPECT_EQ(scene.GetRevision(), revision);
    SceneDocument captured;
    ASSERT_TRUE(CaptureSceneDocument(scene, host.GetSceneAssets(), captured, error)) << error;
    Scene other;
    PopulateScene(captured, host.GetSceneAssets(), other);
    EXPECT_EQ(other.GetEnvironment(), value);
    scene.Clear();
    EXPECT_EQ(scene.GetEnvironment(), EnvironmentSettings{});
    EXPECT_EQ(changes, 4u);
    scene.Clear();
    EXPECT_EQ(changes, 4u); // clearing default environment emits no extra settings event
    scene.RemoveChangeObserver(observer);
}

TEST(SkyboxScene, InitPausedChangesUndoAndSceneReplacementRequestAssets) {
    const auto gameRoot = std::filesystem::temp_directory_path() / ("sri-skybox-scene-" + NewUuid());
    auto source = std::make_shared<MemorySource>();
    AssetCatalog empty;
    std::string manifest, metadata, error;
    ASSERT_TRUE(empty.SerializeManifest(manifest, error)) << error;
    auto json = Json::parse(manifest);
    const AssetReference a{NewUuid(), "game/a.skybox.json"};
    const AssetReference b{NewUuid(), "game/b.skybox.json"};
    for (const auto& reference : {a, b}) {
        ASSERT_TRUE(SerializeAssetMetadata({reference.id, CatalogAssetKind::Skybox}, metadata, error)) << error;
        json["assets"].push_back({{"path", reference.path}, {"metadata", Json::parse(metadata)}});
    }
    // CPU decode fails deliberately: no device is needed to verify request timing/retry behavior.
    source->Add("a.skybox.json", "{}");
    source->Add("b.skybox.json", "{}");
    source->Add("scene.json", R"({"version":1,"entities":[]})");
    auto authoredScene = Json::parse(Minimal);
    authoredScene["environment"] = {{"skybox", {{"id", a.id.ToString()}}}};
    source->Add("sky-scene.json", authoredScene.dump());
    source->Add("manifest.json", json.dump());
    MountScope mount(gameRoot, source);
    struct Reset { ~Reset() { AssetPathResolver::SetManifest({}); AssetPathResolver::SetGameRoot({}); } } reset;
    AssetPathResolver::SetGameRoot(gameRoot);
    AssetPathResolver::SetManifest(gameRoot / "manifest.json");
    Engine host{EngineConfig{.taskThreads = 1}};
    auto& scene = host.GetScene();
    auto& assets = host.GetSceneAssets();
    EnvironmentSettings value;
    value.skybox = {a.id, b.path}; // UUID is authoritative even with a wrong hint.
    ASSERT_TRUE(scene.SetEnvironment(value, error));
    ASSERT_TRUE(host.Initialize());
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Loading);
    EXPECT_EQ(assets.GetState<SkyboxAsset>(b.path), AssetState::Unloaded);
    EXPECT_FALSE(assets.LoadSkybox(a.path));
    host.SetPaused(true);
    const auto beforeClear = CaptureSceneSettings(scene);
    scene.Clear();
    EXPECT_EQ(scene.GetEnvironment(), EnvironmentSettings{});
    // Failed attempts retire their name; no scene setting change should re-request it.
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Unloaded);
    ApplySceneSettings(beforeClear, scene);
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Loading);
    EXPECT_FALSE(assets.LoadSkybox(a.path));
    value.brightness = 4;
    ASSERT_TRUE(scene.SetEnvironment(value, error));
    host.Tick();
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Unloaded); // no per-frame or scalar-edit retry
    const auto saved = CaptureSceneSettings(scene);
    value.skybox = b;
    ASSERT_TRUE(scene.SetEnvironment(value, error));
    EXPECT_EQ(assets.GetState<SkyboxAsset>(b.path), AssetState::Loading);
    ApplySceneSettings(saved, scene);
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Loading);
    ASSERT_TRUE(host.LoadScene(gameRoot / "scene.json", error)) << error;
    EXPECT_EQ(scene.GetEnvironment(), EnvironmentSettings{});
    ApplySceneSettings(saved, scene);
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Loading);
    ASSERT_TRUE(host.LoadScene(gameRoot / "sky-scene.json", error)) << error;
    EXPECT_EQ(scene.GetEnvironment().skybox.id, a.id);
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Loading);
    ASSERT_TRUE(host.LoadScene(gameRoot / "sky-scene.json", error)) << error;
    EXPECT_EQ(assets.GetState<SkyboxAsset>(a.path), AssetState::Loading);
    host.Shutdown();
    EXPECT_EQ(scene.GetEnvironment(), EnvironmentSettings{});
}

TEST(SkyboxScene, SpawnedEntitiesPreserveBaselineSettingsWithoutTransientEnvironmentChanges) {
    Engine host;
    auto& scene = host.GetScene();
    const auto camera = scene.CreateEntity("Camera");
    scene.AddComponent<Camera>(camera);
    scene.SetActiveCamera(camera);
    scene.SetEditorFolders({"Baseline"});
    EnvironmentSettings environment;
    environment.skybox.id = AssetUuid(NewUuid());
    std::string error;
    ASSERT_TRUE(scene.SetEnvironment(environment, error));
    const auto baseline = CaptureSceneSettings(scene);
    unsigned settingsChanges = 0;
    const auto observer = scene.AddChangeObserver([&](Scene::ChangeKind kind, entt::entity) {
        if (kind == Scene::ChangeKind::Settings) ++settingsChanges;
    });
    SceneDocument spawned;
    SceneEntityDefinition entity;
    entity.id = EntityUuid(NewUuid());
    entity.name = "Saved spawn";
    spawned.entities.push_back(entity);
    const auto created = InstantiateEntities(spawned.entities, host.GetSceneAssets(), scene, false);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_TRUE(scene.IsValid(created.front()));
    EXPECT_EQ(CaptureSceneSettings(scene), baseline);
    EXPECT_EQ(settingsChanges, 0u);
    scene.RemoveChangeObserver(observer);
}

TEST(SkyboxScene, ExtractionCopiesFallbackSettingsWhilePausedAndClearsOnShutdown) {
    Engine host;
    // Extraction needs the world, but an empty scene never touches the device.
    auto* rendering = host.AddSubsystem<RenderingSubsystem>();
    auto* extraction = host.AddSubsystem<RenderExtractionSubsystem>();
    ASSERT_NE(rendering, nullptr);
    ASSERT_NE(extraction, nullptr);
    ASSERT_TRUE(extraction->Initialize());
    auto& scene = host.GetScene();
    auto& world = rendering->GetWorld();
    EnvironmentSettings settings;
    settings.skybox = {NewUuid(), "game/missing.skybox.json"};
    settings.brightness = 8;
    settings.tint = {2, 3, 4};
    settings.fallbackColor = {0.2f, 0.3f, 0.4f};
    std::string error;
    ASSERT_TRUE(scene.SetEnvironment(settings, error));
    FrameContext frame{};
    frame.paused = true;
    extraction->LateUpdate(frame);
    EXPECT_EQ(world.environment.settings, settings);
    EXPECT_FALSE(world.environment.cubemap);
    settings.skybox = {};
    settings.fallbackColor = {4, 5, 6};
    ASSERT_TRUE(scene.SetEnvironment(settings, error));
    extraction->LateUpdate(frame);
    EXPECT_EQ(world.environment.settings, settings);
    EXPECT_FALSE(world.environment.cubemap);
    scene.Clear();
    extraction->LateUpdate(frame);
    EXPECT_EQ(world.environment.settings, EnvironmentSettings{});
    ASSERT_TRUE(scene.SetEnvironment(settings, error));
    extraction->LateUpdate(frame);
    extraction->Shutdown();
    EXPECT_EQ(world.environment.settings, EnvironmentSettings{});
    EXPECT_FALSE(world.environment.cubemap);
}
} // namespace
