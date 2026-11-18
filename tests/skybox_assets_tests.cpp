#include <engine/assets/AssetEvents.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/scene/Scene.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <thread>
#include "content_fixture.hpp"

namespace {
using namespace engine;
using Json = nlohmann::json;

TEST(SkyboxDefinition, OptionalDiagnosticHintAndCanonicalRoundTrip) {
    SkyboxDefinition value;
    const auto id = NewUuid();
    std::string error, text;
    ASSERT_TRUE(ParseSkyboxDefinition(Json{{"version", 1}, {"cubemap", {{"id", id}}}}.dump(), value, error)) << error;
    EXPECT_TRUE(value.cubemap.path.empty());
    value.cubemap.path = "stale diagnostic hint";
    ASSERT_TRUE(SerializeSkyboxDefinition(value, text, error)) << error;
    SkyboxDefinition decoded;
    ASSERT_TRUE(ParseSkyboxDefinition(text, decoded, error)) << error;
    EXPECT_EQ(value, decoded);
    std::string canonical;
    ASSERT_TRUE(SerializeSkyboxDefinition(decoded, canonical, error)) << error;
    EXPECT_EQ(text, canonical);
}

TEST(SkyboxDefinition, RejectsMalformedDefinitionsTransactionally) {
    SkyboxDefinition value;
    value.cubemap = {NewUuid(), "unchanged"};
    const auto before = value;
    const Json valid = {{"version", 1}, {"cubemap", {{"id", value.cubemap.id.ToString()}}}};
    std::string error;
    const auto reject = [&](const Json& root) {
        EXPECT_FALSE(ParseSkyboxDefinition(root.dump(), value, error)) << root;
        EXPECT_EQ(value, before);
        EXPECT_FALSE(error.empty());
    };
    for (const auto& version : {Json(0), Json(2), Json(1.0), Json(true), Json("1"), Json(nullptr)}) {
        auto root = valid; root["version"] = version; reject(root);
    }
    auto root = valid; root.erase("version"); reject(root);
    root = valid; root.erase("cubemap"); reject(root);
    root = valid; root["extra"] = 0; reject(root);
    for (const auto& reference : {Json(nullptr), Json("game/cube.dds"), Json::object(),
        Json{{"id", "bad"}}, Json{{"path", "game/cube.dds"}},
        Json{{"id", "00000000-0000-0000-0000-000000000000"}},
        Json{{"id", before.cubemap.id.ToString()}, {"path", 1}},
        Json{{"id", before.cubemap.id.ToString()}, {"extra", true}}}) {
        root = valid; root["cubemap"] = reference; reject(root);
    }
    EXPECT_FALSE(ParseSkyboxDefinition(R"({"version":1,"version":1,"cubemap":{}})", value, error));
    const auto duplicate = "{\"version\":1,\"cubemap\":{\"id\":\"" + before.cubemap.id.ToString() +
        "\",\"id\":\"" + before.cubemap.id.ToString() + "\"}}";
    EXPECT_FALSE(ParseSkyboxDefinition(duplicate, value, error));
    EXPECT_EQ(value, before);
    std::string text = "unchanged";
    value.cubemap = {};
    EXPECT_FALSE(SerializeSkyboxDefinition(value, text, error));
    EXPECT_EQ(text, "unchanged");
}

TEST(SkyboxAssets, CatalogMetadataAndManifestRoundTrip) {
    const auto root = std::filesystem::temp_directory_path() / ("sri-skybox-" + NewUuid());
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); } } cleanup{root};
    std::filesystem::create_directories(root);
    std::string error;
    ASSERT_TRUE(WriteFileAtomic(root / "day.skybox.json", "{}", error)) << error;
    std::size_t generated = 0;
    ASSERT_TRUE(GenerateMissingAssetMetadata(root, generated, error)) << error;
    EXPECT_EQ(generated, 1u);
    EXPECT_EQ(CatalogKindForFile("DAY.SKYBOX.JSON"), CatalogAssetKind::Skybox);
    AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(root, {}, error)) << error;
    const auto* entry = catalog.FindPath("game/day.skybox.json");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->metadata.kind, CatalogAssetKind::Skybox);
    std::string manifest;
    ASSERT_TRUE(catalog.SerializeManifest(manifest, error)) << error;
    EXPECT_EQ(Json::parse(manifest)["assets"][0]["metadata"]["kind"], "skybox");
    AssetCatalog cooked;
    ASSERT_TRUE(cooked.LoadManifest(manifest, root, {}, error)) << error;
    EXPECT_NE(cooked.Find(entry->metadata.id, CatalogAssetKind::Skybox), nullptr);
}

// Minimal DX10 RGBA DDS, including every subresource. Array size counts cubes.
std::string Dds(unsigned width = 1, unsigned height = 1, unsigned cubes = 1, bool cube = true) {
    const unsigned faces = cubes * (cube ? 6 : 1);
    std::string bytes(148 + width * height * 4 * faces, '\xff');
    std::fill(bytes.begin(), bytes.begin() + 148, '\0');
    const auto word = [&](unsigned offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
    };
    word(0, 0x20534444); word(4, 124); word(8, 0x100f);
    word(12, height); word(16, width); word(20, width * 4); word(28, 1);
    word(76, 32); word(80, 4); word(84, 0x30315844); word(108, 0x1000);
    word(112, cube ? 0xfe00 : 0); word(128, 28); word(132, 3);
    word(136, cube ? 4 : 0); word(140, cubes);
    return bytes;
}

struct PackedSkyboxes {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-skybox-packed-" + NewUuid());
    std::shared_ptr<MemorySource> source = std::make_shared<MemorySource>();
    MountScope mount{root, source};
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path oldManifest = AssetPathResolver::GetManifest();
    Json manifest;
    HWND window = nullptr;
    PackedSkyboxes() {
        AssetCatalog empty;
        std::string text, error;
        EXPECT_TRUE(empty.SerializeManifest(text, error));
        manifest = Json::parse(text);
        AssetPathResolver::SetGameRoot(root);
        AssetPathResolver::SetManifest(root / "manifest.json");
        window = CreateWindowExW(0, L"STATIC", L"Skybox asset tests", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    ~PackedSkyboxes() {
        AssetPathResolver::SetManifest(oldManifest);
        AssetPathResolver::SetGameRoot(oldRoot);
        if (window) DestroyWindow(window);
    }
    AssetReference Add(std::string name, CatalogAssetKind kind, const std::string& bytes) {
        AssetReference reference{NewUuid(), "game/" + name};
        source->Add(name, bytes);
        std::string text, error;
        EXPECT_TRUE(SerializeAssetMetadata({reference.id, kind}, text, error)) << error;
        manifest["assets"].push_back({{"path", reference.path}, {"metadata", Json::parse(text)}});
        source->Add("manifest.json", manifest.dump());
        return reference;
    }
    std::string Definition(const AssetReference& texture) {
        std::string text, error;
        EXPECT_TRUE(SerializeSkyboxDefinition({1, texture}, text, error)) << error;
        return text;
    }
};

struct SkyboxEvents {
    std::vector<AssetKind> loaded;
    std::vector<AssetFailedEvent> failed;
    void Loaded(const AssetLoadedEvent& event) { loaded.push_back(event.kind); }
    void Failed(const AssetFailedEvent& event) { failed.push_back(event); }
};

TEST(SkyboxAssets, PackedAsyncDependencyReloadRetentionAndUnload) {
    PackedSkyboxes fixture;
    const auto a = fixture.Add("a.dds", CatalogAssetKind::Texture, Dds());
    const auto b = fixture.Add("b.dds", CatalogAssetKind::Texture, Dds(2, 2));
    const auto flat = fixture.Add("flat.dds", CatalogAssetKind::Texture, Dds(1, 1, 1, false));
    const auto sky = fixture.Add("day.skybox.json", CatalogAssetKind::Skybox,
        fixture.Definition({a.id, "game/stale-hint.png"})); // catalog DDS path is authoritative
    Engine host({.applicationName = "Skybox tests", .surface = {fixture.window, 64, 64}, .taskThreads = 2});
    host.AddSubsystem<RenderingSubsystem>();
    if (!host.Initialize()) GTEST_SKIP() << "D3D11 hardware/debug layer unavailable";
    auto& assets = host.GetSceneAssets();
    SkyboxEvents events;
    host.GetEvents().sink<AssetLoadedEvent>().connect<&SkyboxEvents::Loaded>(events);
    host.GetEvents().sink<AssetFailedEvent>().connect<&SkyboxEvents::Failed>(events);
    const auto handle = assets.RequestSkybox(sky);
    EXPECT_EQ(handle, assets.RequestSkybox("GAME/DAY"));
    EXPECT_EQ(handle, assets.RequestSkybox(AssetReference{sky.id, "stale hint"}));
    EXPECT_EQ(assets.GetState(handle), AssetState::Loading);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (assets.GetState(handle) == AssetState::Loading && std::chrono::steady_clock::now() < until) {
        assets.Update(FrameContext{});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(events.failed.empty());
    ASSERT_TRUE(assets.IsReady(handle));
    const auto live = assets.Peek(handle);
    ASSERT_TRUE(live);
    EXPECT_TRUE(assets.IsReady(live->cubemap));
    const auto textureEvent = std::find(events.loaded.begin(), events.loaded.end(), AssetKind::Texture);
    const auto skyEvent = std::find(events.loaded.begin(), events.loaded.end(), AssetKind::Skybox);
    EXPECT_LT(textureEvent, skyEvent);
    const auto oldTexture = live->cubemap;
    std::string error;
    fixture.source->Add("day.skybox.json", "broken");
    EXPECT_FALSE(assets.Reload("game/day", AssetKind::Skybox, error));
    EXPECT_EQ(live->cubemap, oldTexture);
    EXPECT_TRUE(assets.IsReady(handle));
    fixture.source->Add("day.skybox.json", fixture.Definition({NewUuid(), "game/missing.dds"}));
    EXPECT_FALSE(assets.Reload("game/day", AssetKind::Skybox, error));
    EXPECT_EQ(live->cubemap, oldTexture);
    fixture.source->Add("day.skybox.json", fixture.Definition(flat));
    EXPECT_FALSE(assets.Reload("game/day", AssetKind::Skybox, error));
    EXPECT_EQ(live->cubemap, oldTexture);
    EXPECT_EQ(live->definition.cubemap.id, a.id);
    fixture.source->Add("day.skybox.json", fixture.Definition(b));
    fixture.source->Add("b.dds", "broken DDS");
    EXPECT_FALSE(assets.Reload("game/day", AssetKind::Skybox, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(live->cubemap, oldTexture);
    EXPECT_TRUE(assets.IsReady(handle));
    fixture.source->Add("b.dds", Dds(2, 2));
    ASSERT_TRUE(assets.Reload("game/day", AssetKind::Skybox, error)) << error;
    EXPECT_NE(live->cubemap, oldTexture);
    EXPECT_EQ(live->definition.cubemap.id, b.id);
    EXPECT_EQ(assets.Peek(handle)->cubemap, live->cubemap);
    assets.UnloadAll();
    EXPECT_FALSE(assets.Peek(handle));
    EXPECT_FALSE(assets.Peek(oldTexture));
    const auto replacement = assets.RequestSkybox(sky);
    EXPECT_NE(replacement, handle);
    EXPECT_TRUE(assets.LoadSkybox("game/day"));
    host.GetEvents().sink<AssetLoadedEvent>().disconnect<&SkyboxEvents::Loaded>(events);
    host.GetEvents().sink<AssetFailedEvent>().disconnect<&SkyboxEvents::Failed>(events);
}

TEST(SkyboxAssets, Rejects2DArrayNonSquareMissingAndWrongKindDependencies) {
    PackedSkyboxes fixture;
    const auto flat = fixture.Add("flat.dds", CatalogAssetKind::Texture, Dds(1, 1, 1, false));
    const auto array = fixture.Add("array.dds", CatalogAssetKind::Texture, Dds(1, 1, 2));
    const auto flatArray = fixture.Add("flat-array.dds", CatalogAssetKind::Texture, Dds(1, 1, 2, false));
    const auto rectangle = fixture.Add("rectangle.dds", CatalogAssetKind::Texture, Dds(2, 1));
    const auto notDds = fixture.Add("fake.png", CatalogAssetKind::Texture, Dds());
    const auto wrongKind = fixture.Add("other.skybox.json", CatalogAssetKind::Skybox, fixture.Definition(flat));
    const auto cube = fixture.Add("cube.dds", CatalogAssetKind::Texture, Dds());
    Engine host({.surface = {fixture.window, 64, 64}, .taskThreads = 2});
    host.AddSubsystem<RenderingSubsystem>();
    if (!host.Initialize()) GTEST_SKIP() << "D3D11 hardware/debug layer unavailable";
    auto& assets = host.GetSceneAssets();
    unsigned index = 0;
    for (const auto& reference : {flat, array, flatArray, rectangle, notDds, wrongKind, AssetReference{NewUuid(), flat.path}}) {
        const auto name = "bad" + std::to_string(index++);
        fixture.source->Add(name + ".skybox.json", fixture.Definition(reference));
        const auto handle = assets.RequestSkybox("game/" + name);
        EXPECT_FALSE(assets.LoadSkybox("game/" + name)) << reference.path;
        EXPECT_EQ(assets.GetState(handle), AssetState::Failed);
    }
    // A cube remains a legal generic TextureAsset, but cannot bind as Texture2D.
    ASSERT_TRUE(assets.LoadTexture(cube.path));
    fixture.source->Add("cube.material.json", Json{{"version", 1}, {"hooks", Json::array()},
        {"defaults", {{"albedoTexture", cube.path}}}}.dump());
    EXPECT_FALSE(assets.LoadMaterial("game/cube"));
    ParticleEffectDefinition particles;
    particles.texture = cube;
    std::string text, error;
    ASSERT_TRUE(SerializeParticleEffectDefinition(particles, text, error)) << error;
    fixture.source->Add("cube.particle-effect.json", text);
    EXPECT_FALSE(assets.LoadParticleEffect("game/cube"));
    fixture.source->Add("recover.skybox.json", "broken");
    const auto failed = assets.RequestSkybox("game/recover");
    EXPECT_FALSE(assets.LoadSkybox("game/recover"));
    fixture.source->Add("recover.skybox.json", fixture.Definition(cube));
    ASSERT_TRUE(assets.Reload("game/recover", AssetKind::Skybox, error)) << error;
    EXPECT_TRUE(assets.LoadSkybox("game/recover"));
    EXPECT_FALSE(assets.Peek(failed));
}

TEST(SkyboxAssets, DirectMaterialApisRejectCubeAndAcceptNative2DViewsTransactionally) {
    PackedSkyboxes fixture;
    const auto cubeReference = fixture.Add("cube.dds", CatalogAssetKind::Texture, Dds());
    const auto flatReference = fixture.Add("flat.dds", CatalogAssetKind::Texture, Dds(1, 1, 1, false));
    Engine host({.surface = {fixture.window, 64, 64}, .taskThreads = 2});
    auto* rendering = host.AddSubsystem<RenderingSubsystem>();
    if (!host.Initialize()) GTEST_SKIP() << "D3D11 hardware/debug layer unavailable";
    auto& assets = host.GetSceneAssets();
    auto& resources = rendering->GetResourceManager();
    const auto cube = assets.LoadTexture(cubeReference.path);
    const auto flat = assets.LoadTexture(flatReference.path);
    ASSERT_TRUE(cube);
    ASSERT_TRUE(flat);
    // Both DDS assets wrap existing SRVs; Texture::desc is not authoritative.
    MaterialDesc desc;
    desc.defaults.albedoTexture = cube->texture;
    EXPECT_FALSE(resources.CreateMaterial(desc));
    EXPECT_FALSE(resources.RegisterMaterial({}, desc.defaults));
    desc.defaults.albedoTexture = flat->texture;
    const auto material = resources.CreateMaterial(desc);
    const auto other = resources.RegisterMaterial({}, desc.defaults);
    ASSERT_TRUE(material);
    ASSERT_TRUE(other);
    const auto instance = resources.CreateMaterialInstance(material);
    ASSERT_TRUE(instance);
    MaterialParameters parameters;
    parameters.albedoTexture = flat->texture;
    parameters.parameters.shininess = 13;
    ASSERT_TRUE(resources.SetMaterialInstanceParameters(instance, parameters));
    parameters.albedoTexture = cube->texture;
    parameters.parameters.shininess = 99;
    EXPECT_FALSE(resources.SetMaterialInstanceParameters(instance, parameters));
    EXPECT_EQ(resources.TryGetMaterialInstance(instance)->overrides.albedoTexture, flat->texture);
    EXPECT_EQ(resources.TryGetMaterialInstance(instance)->overrides.shininess, 13);

    MaterialInstance::Overrides overrides;
    overrides.albedoTexture = cube->texture;
    overrides.shininess = 27;
    EXPECT_FALSE(resources.SetMaterialInstanceOverrides(instance, overrides));
    EXPECT_FALSE(resources.UpdateMaterialInstance(instance, other, overrides));
    EXPECT_EQ(resources.TryGetMaterialInstance(instance)->material, material);
    EXPECT_EQ(resources.TryGetMaterialInstance(instance)->overrides.albedoTexture, flat->texture);
    EXPECT_EQ(resources.TryGetMaterial(material)->instanceCount, 1u);
    EXPECT_EQ(resources.TryGetMaterial(other)->instanceCount, 0u);
    overrides.albedoTexture = flat->texture;
    ASSERT_TRUE(resources.SetMaterialInstanceOverrides(instance, overrides));
    ASSERT_TRUE(resources.UpdateMaterialInstance(instance, other, overrides));
    EXPECT_EQ(resources.TryGetMaterialInstance(instance)->material, other);
    EXPECT_EQ(resources.TryGetMaterialInstance(instance)->overrides.shininess, 27);

    // Explicit clear and absent/inherit retain their distinct existing semantics.
    overrides.albedoTexture = TextureHandle{};
    ASSERT_TRUE(resources.SetMaterialInstanceOverrides(instance, overrides));
    EXPECT_TRUE(resources.TryGetMaterialInstance(instance)->overrides.albedoTexture.has_value());
    overrides.albedoTexture.reset();
    ASSERT_TRUE(resources.UpdateMaterialInstance(instance, material, overrides));
    EXPECT_FALSE(resources.TryGetMaterialInstance(instance)->overrides.albedoTexture);

    const auto staged = resources.RegisterMaterial({}, desc.defaults);
    ASSERT_TRUE(staged);
    EXPECT_TRUE(resources.ReplaceMaterial(material, staged));
    EXPECT_EQ(resources.TryGetMaterial(material)->defaultParameters.albedoTexture, flat->texture);

    // A registered render-target-only texture has no SRV: reject without invoking
    // Texture::GetSRV's assertion or changing the existing material instance.
    TextureDesc targetDesc;
    targetDesc.width = targetDesc.height = 1;
    targetDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    targetDesc.usage = TextureUsage::RenderTarget;
    const auto target = resources.CreateTexture(targetDesc);
    parameters.albedoTexture = target;
    EXPECT_FALSE(resources.SetMaterialInstanceParameters(instance, parameters));
    desc.defaults.albedoTexture = target;
    EXPECT_FALSE(resources.CreateMaterial(desc));
    EXPECT_FALSE(resources.RegisterMaterial({}, desc.defaults));
    EXPECT_TRUE(resources.DestroyMaterialInstance(instance));
    EXPECT_TRUE(resources.DestroyMaterial(material));
    EXPECT_TRUE(resources.DestroyMaterial(other));
    EXPECT_TRUE(resources.DestroyTexture(target));
}

TEST(SkyboxAssets, SceneEnvironmentCatalogRenameRequestsNewPathWithoutSettingsChange) {
    PackedSkyboxes fixture;
    const auto texture = fixture.Add("old.dds", CatalogAssetKind::Texture, Dds());
    const auto skybox = fixture.Add("old.skybox.json", CatalogAssetKind::Skybox, fixture.Definition(texture));
    Engine host({.surface = {fixture.window, 64, 64}, .taskThreads = 2});
    auto* rendering = host.AddSubsystem<RenderingSubsystem>();
    auto* extraction = host.AddSubsystem<RenderExtractionSubsystem>();
    std::string error;
    EnvironmentSettings environment;
    environment.skybox = skybox;
    // Initial assignment must also work before Initialize installs its listener.
    ASSERT_TRUE(host.GetScene().SetEnvironment(environment, error)) << error;
    if (!host.Initialize()) GTEST_SKIP() << "D3D11 hardware/debug layer unavailable";
    auto& assets = host.GetSceneAssets();
    const auto pumpUntilReady = [&](std::string_view path) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!assets.Peek<SkyboxAsset>(path) && std::chrono::steady_clock::now() < until) {
            assets.Update(FrameContext{});
            extraction->LateUpdate(FrameContext{});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        extraction->LateUpdate(FrameContext{});
        ASSERT_TRUE(assets.Peek<SkyboxAsset>(path)) << path;
        ASSERT_TRUE(rendering->GetWorld().environment.cubemap);
    };
    ASSERT_NO_FATAL_FAILURE(pumpUntilReady(skybox.path));
    const auto oldSkybox = assets.Peek<SkyboxAsset>(skybox.path);
    ASSERT_TRUE(oldSkybox);
    const auto oldTexture = assets.Peek(oldSkybox->cubemap);
    ASSERT_TRUE(oldTexture);
    EXPECT_EQ(rendering->GetWorld().environment.cubemap, oldTexture->texture);
    EXPECT_EQ(host.GetAssets().GetState<SkyboxAsset>(skybox.path), AssetState::Unloaded);

    const AssetReference renamedTexture{texture.id, "game/renamed.dds"};
    const AssetReference renamedSkybox{skybox.id, "game/renamed.skybox.json"};
    fixture.source->Add("renamed.dds", Dds(2, 2));
    // Keep the descriptor's old path hint too: its texture UUID must resolve anew.
    fixture.source->Add("renamed.skybox.json", fixture.Definition(texture));
    fixture.source->Add("old.dds", "removed by rename");
    fixture.source->Add("old.skybox.json", "removed by rename");
    for (auto& entry : fixture.manifest["assets"]) {
        if (entry["path"] == texture.path) entry["path"] = renamedTexture.path;
        if (entry["path"] == skybox.path) entry["path"] = renamedSkybox.path;
    }
    fixture.source->Add("manifest.json", fixture.manifest.dump());

    // A host-owned catalog rescan must not take ownership of scene environment assets.
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    host.GetAssets().Update(FrameContext{});
    EXPECT_EQ(host.GetAssets().GetState<SkyboxAsset>(renamedSkybox.path), AssetState::Unloaded);
    ASSERT_TRUE(assets.RescanCatalog(error)) << error;
    // Do not Request/Load here: ApplyCatalog alone must queue the new path.
    ASSERT_NO_FATAL_FAILURE(pumpUntilReady(renamedSkybox.path));
    EXPECT_EQ(host.GetScene().GetEnvironment(), environment);
    EXPECT_EQ(host.GetScene().GetEnvironment().skybox, skybox);
    const auto* entry = assets.GetCatalog().Find(skybox.id, CatalogAssetKind::Skybox);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->path, renamedSkybox.path);
    EXPECT_EQ(assets.GetCatalog().FindPath(skybox.path), nullptr);
    const auto renamed = assets.Peek<SkyboxAsset>(entry->path);
    ASSERT_TRUE(renamed);
    ASSERT_TRUE(assets.IsReady(renamed->cubemap));
    EXPECT_NE(renamed->cubemap, oldSkybox->cubemap);
    const auto readyTexture = assets.Peek<TextureAsset>(renamedTexture.path);
    ASSERT_TRUE(readyTexture);
    EXPECT_EQ(rendering->GetWorld().environment.cubemap, readyTexture->texture);
    EXPECT_NE(rendering->GetWorld().environment.cubemap, oldTexture->texture);
    EXPECT_EQ(host.GetAssets().GetState<SkyboxAsset>(renamedSkybox.path), AssetState::Unloaded);
}
} // namespace
