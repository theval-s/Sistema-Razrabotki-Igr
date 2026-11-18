#include <engine/assets/ParticleEffectDefinition.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/Scene.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <limits>
#include <chrono>
#include <thread>
#include "content_fixture.hpp"

namespace {
using namespace engine;
using Json = nlohmann::json;
constexpr std::string_view Minimal = R"({"version":1,"texture":null,"settings":{}})";

TEST(ParticleEffectDefinition, DefaultsCanonicalRoundTripAndAdvancedHooks) {
    ParticleEffectDefinition value;
    std::string text, error;
    ASSERT_TRUE(ParseParticleEffectDefinition(Minimal, value, error)) << error;
    EXPECT_EQ(value.settings, ParticleEffectSettings{});
    value.settings.appearance = 2;
    value.settings.acceleration = {-1, 2, -3};
    value.settings.blend = ParticleBlendMode::Additive;
    value.texture = {NewUuid(), "game/textures/smoke.png"};
    ASSERT_TRUE(SerializeParticleEffectDefinition(value, text, error)) << error;
    const auto json = Json::parse(text);
    EXPECT_EQ(json.at("settings").size(), 23u);
    EXPECT_EQ(json.at("settings").at("blend"), "additive");
    ParticleEffectDefinition decoded;
    ASSERT_TRUE(ParseParticleEffectDefinition(text, decoded, error)) << error;
    EXPECT_EQ(value, decoded);
    std::string canonical;
    ASSERT_TRUE(SerializeParticleEffectDefinition(decoded, canonical, error)) << error;
    EXPECT_EQ(canonical, text);
}

TEST(ParticleEffectDefinition, RejectsInvalidNumbersTypesAndUnknownFieldsTransactionally) {
    ParticleEffectDefinition value;
    value.settings.rate = 123;
    const auto before = value;
    std::string error;
    const auto reject = [&](Json root) {
        EXPECT_FALSE(ParseParticleEffectDefinition(root.dump(), value, error)) << root;
        EXPECT_EQ(value, before);
        EXPECT_FALSE(error.empty());
    };
    for (const auto& [name, invalid] : std::vector<std::pair<std::string, Json>>{
        {"appearance", 1}, {"appearance", -1}, {"appearance", 4294967296ull}, {"maxAlive", 0},
        {"maxAlive", 1.5}, {"lifetime", 0}, {"lifetimeVariation", 1}, {"lifetimeVariation", -0.1},
        {"speedVariation", 1.01}, {"rate", -1}, {"speed", -1}, {"radius", -1}, {"spread", -1},
        {"drag", -1}, {"sizeStart", -1}, {"sizeEnd", -1}, {"softDistance", -1}, {"shadowDensity", -1},
        {"opacityStart", 1.1}, {"opacityEnd", -0.1}, {"fadeIn", 1.1}, {"colorStart", Json::array({1, -1, 1})},
        {"colorEnd", Json::array({1, 2})}, {"acceleration", "bad"}, {"blend", "fire"},
        {"randomRotation", 1}, {"castShadows", "true"}, {"rate", true}, {"unknown", 0}}) {
        SCOPED_TRACE(name);
        auto root = Json::parse(Minimal);
        root["settings"][name] = invalid;
        reject(root);
    }
    auto root = Json::parse(Minimal);
    root["extra"] = 1;
    reject(root);
    root = Json::parse(Minimal); root["version"] = 2; reject(root);
    root = Json::parse(Minimal); root["texture"] = "game/smoke.png"; reject(root);
    EXPECT_FALSE(ParseParticleEffectDefinition(R"({"version":1,"texture":null,"settings":{"rate":1,"rate":2}})", value, error));
    EXPECT_EQ(value, before);
    EXPECT_FALSE(ParseParticleEffectDefinition(R"({"version":1,"texture":null,"settings":{"rate":1e999}})", value, error));
    std::string text = "unchanged";
    value.settings.acceleration.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(SerializeParticleEffectDefinition(value, text, error));
    EXPECT_EQ(text, "unchanged");
    value = before;
    value.settings.lifetime = std::numeric_limits<float>::denorm_min();
    value.settings.lifetimeVariation = 0.999f;
    EXPECT_FALSE(ValidateParticleEffectSettings(value.settings, error));
    value = before;
    value.settings.speedVariation = 1;
    value.settings.opacityStart = 1;
    value.settings.fadeIn = 0;
    EXPECT_TRUE(ValidateParticleEffectSettings(value.settings, error)) << error;
}

TEST(ParticleEffectAssets, CatalogMetadataAndBundledManifestRoundTrip) {
    const auto root = std::filesystem::temp_directory_path() / ("sri-particle-" + NewUuid());
    const auto game = root / "game";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    std::filesystem::create_directories(game);
    std::string error;
    ASSERT_TRUE(WriteFileAtomic(game / "smoke.particle-effect.json", Minimal, error)) << error;
    std::size_t generated = 0;
    ASSERT_TRUE(GenerateMissingAssetMetadata(game, generated, error)) << error;
    EXPECT_EQ(generated, 1u);
    EXPECT_EQ(CatalogKindForFile("SMOKE.PARTICLE-EFFECT.JSON"), CatalogAssetKind::ParticleEffect);
    AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(game, {}, error)) << error;
    const auto* entry = catalog.FindPath("game/smoke.particle-effect.json");
    ASSERT_NE(entry, nullptr);
    const auto id = entry->metadata.id;
    EXPECT_EQ(entry->metadata.kind, CatalogAssetKind::ParticleEffect);
    std::string manifest;
    ASSERT_TRUE(catalog.SerializeManifest(manifest, error)) << error;
    AssetCatalog bundled;
    ASSERT_TRUE(bundled.LoadManifest(manifest, root / "absent-game", root / "absent-engine", error)) << error;
    ASSERT_NE(bundled.Find(id, CatalogAssetKind::ParticleEffect), nullptr);
    EXPECT_EQ(bundled.Find(id)->path, entry->path);
}

TEST(ParticleEffectAssets, PackedAsyncLoadingReloadAndTextureFailure) {
    const auto gameRoot = std::filesystem::temp_directory_path() / ("sri-particle-memory-" + NewUuid());
    auto source = std::make_shared<MemorySource>();
    source->Add("smoke.particle-effect.json", Minimal);
    MountScope mount(gameRoot, source);
    struct Reset { ~Reset() { AssetPathResolver::SetGameRoot({}); } } reset;
    AssetPathResolver::SetGameRoot(gameRoot);
    Engine engine{EngineConfig{.taskThreads = 1}};
    ASSERT_TRUE(engine.Initialize());
    auto& assets = engine.GetSceneAssets();
    const auto handle = assets.RequestParticleEffect("game/smoke");
    EXPECT_EQ(handle, assets.RequestParticleEffect("GAME/SMOKE.PARTICLE-EFFECT.JSON"));
    EXPECT_EQ(assets.GetState(handle), AssetState::Loading);
    for (unsigned i = 0; i < 1000 && assets.GetState(handle) == AssetState::Loading; ++i) {
        assets.Update(FrameContext{});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(assets.IsReady(handle));
    auto live = assets.Peek(handle);
    ASSERT_TRUE(live);
    EXPECT_FALSE(live->texture);
    std::string error;
    ParticleEffectDefinition definition;
    definition.settings.rate = 91;
    definition.settings.appearance = 3;
    std::string text;
    ASSERT_TRUE(SerializeParticleEffectDefinition(definition, text, error)) << error;
    source->Add("smoke.particle-effect.json", text);
    ASSERT_TRUE(assets.Reload("game/smoke", AssetKind::ParticleEffect, error)) << error;
    EXPECT_EQ(live->definition.settings.rate, 91);
    EXPECT_EQ(assets.Peek(handle)->definition.settings.appearance, 3u);
    source->Add("smoke.particle-effect.json", "broken");
    EXPECT_FALSE(assets.Reload("game/smoke", AssetKind::ParticleEffect, error));
    EXPECT_EQ(live->definition.settings.rate, 91);
    definition.texture = {NewUuid(), "game/missing.png"};
    ASSERT_TRUE(SerializeParticleEffectDefinition(definition, text, error)) << error;
    source->Add("smoke.particle-effect.json", text);
    EXPECT_FALSE(assets.Reload("game/smoke", AssetKind::ParticleEffect, error));
    EXPECT_EQ(live->definition.settings.rate, 91);
    source->Add("broken.particle-effect.json", text);
    const auto broken = assets.RequestParticleEffect("game/broken");
    EXPECT_FALSE(assets.LoadParticleEffect("game/broken"));
    EXPECT_EQ(assets.GetState(broken), AssetState::Failed);
    source->Add("broken.particle-effect.json", Minimal);
    ASSERT_TRUE(assets.Reload("game/broken", AssetKind::ParticleEffect, error)) << error;
    ASSERT_TRUE(assets.LoadParticleEffect("game/broken"));
    assets.UnloadAll();
    EXPECT_FALSE(assets.Peek(handle));
    source->Add("smoke.particle-effect.json", Minimal);
    const auto replacement = assets.RequestParticleEffect("game/smoke");
    EXPECT_NE(replacement, handle);
    EXPECT_TRUE(assets.LoadParticleEffect("game/smoke"));
}

TEST(ParticleEffectScene, ReferencesOverridesAndLegacyFireMigration) {
    SceneDocument document;
    SceneEntityDefinition entity;
    entity.id = EntityUuid(NewUuid());
    entity.name = "Smoke";
    entity.particleEmitter = ParticleEmitter{};
    entity.particleEmitter->effect = {NewUuid(), "game/smoke.particle-effect.json"};
    entity.particleEmitter->overrideEmission = true;
    entity.particleEmitter->seed = 42;
    entity.particleEmitter->tint = {2, 3, 4};
    document.entities.push_back(entity);
    std::string text, error;
    ASSERT_TRUE(SerializeSceneDocument(document, text, error)) << error;
    SceneDocument decoded;
    ASSERT_TRUE(ParseSceneDocument(text, decoded, error)) << error;
    EXPECT_EQ(decoded.entities[0].particleEmitter, entity.particleEmitter);
    auto json = Json::parse(text);
    auto& emitter = json["entities"][0]["components"]["ParticleEmitter"];
    emitter.erase("effect"); emitter.erase("overrideEmission"); emitter["appearance"] = 1;
    ASSERT_TRUE(ParseSceneDocument(json.dump(), decoded, error)) << error;
    ASSERT_TRUE(decoded.entities[0].particleEmitter);
    EXPECT_EQ(decoded.entities[0].particleEmitter->appearance, 0u);
    EXPECT_FALSE(decoded.entities[0].particleEmitter->effect.id.IsValid());
    EXPECT_FALSE(decoded.entities[0].particleEmitter->overrideEmission);
    emitter["appearance"] = 2;
    ASSERT_TRUE(ParseSceneDocument(json.dump(), decoded, error)) << error;
    EXPECT_EQ(decoded.entities[0].particleEmitter->appearance, 2u);
    emitter["effect"] = {{"id", "bad"}, {"path", "game/smoke.particle-effect.json"}};
    const auto before = decoded;
    EXPECT_FALSE(ParseSceneDocument(json.dump(), decoded, error));
    EXPECT_EQ(decoded.entities, before.entities);
}

TEST(ParticleEffectScene, ComponentLoadAndChangesRequestPackedUuidAssets) {
    const auto gameRoot = std::filesystem::temp_directory_path() / ("sri-particle-scene-" + NewUuid());
    auto source = std::make_shared<MemorySource>();
    source->Add("a.particle-effect.json", Minimal);
    source->Add("b.particle-effect.json", Minimal);
    AssetCatalog empty;
    std::string manifest, metadata, error;
    ASSERT_TRUE(empty.SerializeManifest(manifest, error)) << error;
    auto json = Json::parse(manifest);
    const AssetReference a{NewUuid(), "game/a.particle-effect.json"};
    const AssetReference b{NewUuid(), "game/b.particle-effect.json"};
    for (const auto& reference : {a, b}) {
        ASSERT_TRUE(SerializeAssetMetadata({reference.id, CatalogAssetKind::ParticleEffect}, metadata, error)) << error;
        json["assets"].push_back({{"path", reference.path}, {"metadata", Json::parse(metadata)}});
    }
    source->Add("manifest.json", json.dump());
    MountScope mount(gameRoot, source);
    struct Reset {
        ~Reset() { AssetPathResolver::SetManifest({}); AssetPathResolver::SetGameRoot({}); }
    } reset;
    AssetPathResolver::SetGameRoot(gameRoot);
    AssetPathResolver::SetManifest(gameRoot / "manifest.json");
    Engine host{EngineConfig{.taskThreads = 1}};
    auto& scene = host.GetScene();
    auto& assets = host.GetSceneAssets();
    const auto entity = scene.CreateEntity("Preexisting");
    ParticleEmitter emitter;
    emitter.effect = a;
    ASSERT_NE(scene.AddComponent<ParticleEmitter>(entity, emitter), nullptr);
    ASSERT_TRUE(host.Initialize());
    EXPECT_EQ(assets.GetState<ParticleEffectAsset>(a.path), AssetState::Loading);
    ASSERT_TRUE(assets.LoadParticleEffect(a.path));
    scene.Modify<ParticleEmitter>(entity, [&](ParticleEmitter& changed) { changed.effect = b; });
    EXPECT_EQ(assets.GetState<ParticleEffectAsset>(b.path), AssetState::Loading);
    ASSERT_TRUE(assets.LoadParticleEffect(b.path));
    const auto requested = assets.RequestParticleEffect(b);
    EXPECT_EQ(requested, assets.RequestParticleEffect(AssetReference{b.id, a.path})); // UUID wins over hint
    EXPECT_FALSE(assets.RequestParticleEffect(AssetReference{AssetUuid(NewUuid()), a.path}));

    SceneEntityDefinition definition;
    definition.id = EntityUuid(NewUuid());
    definition.name = "Populated";
    definition.particleEmitter = emitter;
    Scene other;
    const std::vector definitions{definition};
    const auto populated = ApplyEntityDefinitions(definitions, assets, other);
    ASSERT_EQ(populated.size(), 1u);
    ASSERT_NE(other.TryGetComponent<ParticleEmitter>(populated[0]), nullptr);
    EXPECT_EQ(other.TryGetComponent<ParticleEmitter>(populated[0])->effect, a);
}
} // namespace
