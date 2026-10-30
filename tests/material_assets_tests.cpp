#include <engine/assets/AssetEvents.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
#include <vector>

namespace {
using namespace engine;

TEST(MaterialDefinitions, MaterialRoundTrip) {
    MaterialDefinition material;
    std::string error, json;
    ASSERT_TRUE(ParseMaterialDefinition(R"({"version":1,"shaderFile":"game/test.hlsl",
        "hooks":["GBufferPS"],"defaults":{"albedoTexture":"game/white.png","shininess":8}})", material, error)) << error;
    EXPECT_EQ(material.hooks, MaterialHook::GBufferPS);
    EXPECT_EQ(material.defaults.shininess, 8);
    ASSERT_TRUE(SerializeMaterialDefinition(material, json, error)) << error;
    MaterialDefinition roundTrip;
    ASSERT_TRUE(ParseMaterialDefinition(json, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip.shaderFile, material.shaderFile);
    EXPECT_EQ(roundTrip.hooks, material.hooks);
    EXPECT_EQ(roundTrip.defaults.albedoTexture, material.defaults.albedoTexture);
    EXPECT_EQ(roundTrip.defaults.shininess, material.defaults.shininess);
}

TEST(MaterialDefinitions, UuidReferencesRoundTripAndVersionTypes) {
    const std::string uuid = NewUuid();
    MaterialDefinition definition;
    definition.version = 2;
    definition.defaults.albedoReference = AssetReference{uuid, "engine/primitives/white/texture.png"};
    definition.defaults.albedoTexture = definition.defaults.albedoReference->path;
    std::string json, error;
    ASSERT_TRUE(SerializeMaterialDefinition(definition, json, error)) << error;
    MaterialDefinition loaded;
    ASSERT_TRUE(ParseMaterialDefinition(json, loaded, error)) << error;
    ASSERT_TRUE(loaded.defaults.albedoReference);
    EXPECT_EQ(loaded.defaults.albedoReference->id, uuid);
    EXPECT_FALSE(ParseMaterialDefinition(R"({"version":2,"hooks":[],"defaults":{"albedoTexture":"game/a.png"}})",
                                         loaded, error));
    MaterialInstanceDefinition instance;
    instance.version = 2;
    instance.materialReference = AssetReference{uuid, "game/materials/stone.material.json"};
    instance.material = instance.materialReference->path;
    ASSERT_TRUE(SerializeMaterialInstanceDefinition(instance, json, error)) << error;
    MaterialInstanceDefinition restored;
    ASSERT_TRUE(ParseMaterialInstanceDefinition(json, restored, error)) << error;
    ASSERT_TRUE(restored.materialReference);
    EXPECT_EQ(restored.materialReference->id, uuid);
}

TEST(MaterialDefinitions, AcceptsUtf8BomAndBuiltinDefaults) {
    MaterialDefinition material;
    std::string error;
    ASSERT_TRUE(ParseMaterialDefinition("\xEF\xBB\xBF" R"({"version":1,"hooks":[]})", material, error)) << "UTF-8 BOM";
    EXPECT_EQ(material.hooks, MaterialHook::None);
    EXPECT_TRUE(material.shaderFile.empty());
}

TEST(MaterialDefinitions, RejectsInvalidMaterialWithoutChangingOutput) {
    MaterialDefinition material;
    std::string error;
    const std::vector<std::string> invalidMaterials{
        R"({"version":1})", R"({"version":3,"hooks":[]})",
        R"({"version":1,"hooks":["GBufferPS"]})",
        R"({"version":1,"hooks":["unknown"]})",
        R"({"version":1,"hooks":["ShadowVS","ShadowVS"],"shaderFile":"game/x"})",
        R"({"version":1,"hooks":[],"hooks":[]})",
        R"({"version":1,"hooks":[],"defaults":{"shininess":-1}})",
        R"({"version":1,"hooks":[],"defaults":{"shininess":1e300}})",
        R"({"version":1,"hooks":[],"defaults":{"specularColor":[0,1]}})",
        R"({"version":1,"hooks":[],"defaults":{"specularColor":[0,-1,0]}})",
        R"({"version":1,"hooks":[],"defaults":{"customBuffer":42}})",
        R"({"version":1,"hooks":[],"defaults":{"albedoTexture":"../escape.png"}})",
        R"({"version":1,"hooks":[],"shaderFile":"game/../escape.hlsl"})",
        R"({"version":1,"hooks":[],"shaderFile":"game/./escape.hlsl"})",
        R"({"version":1,"hooks":[],"shaderFile":"game//escape.hlsl"})",
        R"({"version":1,"hooks":[],"shaderFile":"/game/escape.hlsl"})",
        R"({"version":1,"hooks":[],"shaderFile":"game/"})",
        R"({"version":1,"hooks":[],"shaderFile":"game\\..\\escape.hlsl"})",
        R"({"version":1,"hooks":[],"shaderFile":"C:/absolute.hlsl"})",
        R"({"version":1,"hooks":[],"defaults":{"albedoTexture":"game/a\u0000.png"}})",
        R"({"version":1,"hooks":[],"defaults":{"shininess":1,"shininess":2}})",
        R"({"version":1,"hooks":[],"material":"game/wrong-kind"})", "{", "[]",
        std::string(4096, '[') + "0" + std::string(4096, ']'),
        std::string("{\"version\":1,\"hooks\":[],\"shaderFile\":\"game/") + "\xFF" + "\"}"
    };
    for (const auto& invalid : invalidMaterials) {
        SCOPED_TRACE(invalid);
        material.shaderFile = "unchanged";
        EXPECT_FALSE(ParseMaterialDefinition(invalid, material, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(material.shaderFile, "unchanged");
    }
}

TEST(MaterialDefinitions, PreservesSparseNullAndZeroOverrides) {
    std::string error, json;
    MaterialInstanceDefinition instance;
    ASSERT_TRUE(ParseMaterialInstanceDefinition(R"({"version":1,"material":"game/base","parameters":{}})", instance, error)) << error;
    EXPECT_FALSE(instance.parameters.albedoTexture);
    EXPECT_FALSE(instance.parameters.shininess);
    ASSERT_TRUE(ParseMaterialInstanceDefinition(R"({"version":1,"material":"game/base","parameters":{
        "albedoTexture":null,"shininess":0,"specularColor":[0,0,0]}})", instance, error)) << error;
    ASSERT_TRUE(instance.parameters.albedoTexture);
    EXPECT_TRUE(instance.parameters.albedoTexture->empty());
    EXPECT_EQ(instance.parameters.shininess, 0);
    ASSERT_TRUE(SerializeMaterialInstanceDefinition(instance, json, error)) << error;
    MaterialInstanceDefinition decoded;
    ASSERT_TRUE(ParseMaterialInstanceDefinition(json, decoded, error)) << error;
    ASSERT_TRUE(decoded.parameters.albedoTexture);
    EXPECT_TRUE(decoded.parameters.albedoTexture->empty());
    EXPECT_EQ(decoded.parameters.shininess, 0);
}

TEST(MaterialDefinitions, RejectsInvalidInstance) {
    MaterialInstanceDefinition decoded;
    std::string error;
    for (const auto& invalid : {
        R"({"version":1,"material":""})", R"({"version":1,"material":"../base"})",
        R"({"version":1,"material":"game/base","parameters":{"albedoTexture":""}})",
        R"({"version":1,"material":"game/base","hooks":[]})",
        R"({"version":1,"material":"game/base","parameters":{"shininess":null}})"}) {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(ParseMaterialInstanceDefinition(invalid, decoded, error));
    }
}

TEST(MaterialDefinitions, RejectsUnserializableValuesWithoutChangingOutput) {
    MaterialInstanceDefinition instance;
    MaterialDefinition material;
    std::string error, json;
    instance.parameters.shininess = std::numeric_limits<float>::infinity();
    json = "unchanged";
    EXPECT_FALSE(SerializeMaterialInstanceDefinition(instance, json, error));
    EXPECT_EQ(json, "unchanged");
    material.hooks = static_cast<MaterialHook>(0x80);
    EXPECT_FALSE(SerializeMaterialDefinition(material, json, error));
    EXPECT_EQ(json, "unchanged");
}

TEST(MaterialDefinitions, Utf8ReferenceRoundTrip) {
    MaterialDefinition material, roundTrip;
    std::string error, json;
    material.shaderFile = "game/материалы/цвет.hlsl";
    ASSERT_TRUE(SerializeMaterialDefinition(material, json, error)) << "UTF-8 reference serialization";
    ASSERT_TRUE(ParseMaterialDefinition(json, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip.shaderFile, material.shaderFile);
}

TEST(AssetTable, FailedLoadsNeverReuseStaleGenerations) {
    AssetTable<MaterialAsset> table;
    std::vector<AssetHandle<MaterialAsset>> stale;
    for (int i = 0; i < 5; ++i) {
        const auto handle = table.Acquire(AssetDesc{.name = "retry"});
        for (auto old : stale) EXPECT_EQ(table.Get(old), nullptr) << "stale generation was reused";
        stale.push_back(handle);
        table.Fail(handle, "test");
    }
}

TEST(AssetPaths, MaterialsAndShadersRespectProjectGameRoot) {
    struct RestoreGameRoot {
        std::filesystem::path previous = AssetPathResolver::GetGameRoot();
        ~RestoreGameRoot() { AssetPathResolver::SetGameRoot(previous); }
    } restore;
    const auto content = AssetPathResolver::GetRoot() / "project-content";
    AssetPathResolver::SetGameRoot(content);
    AssetDesc desc;
    desc.path = AssetPathResolver::DefaultPath("game/materials/цвет", AssetKind::Material);
    EXPECT_EQ(AssetPathResolver::ResolveMaterial(desc), content / FromUtf8("materials/цвет.material.json"));
    EXPECT_EQ(AssetPathResolver::ResolveMaterial(desc, true), content / FromUtf8("materials/цвет.material-instance.json"));
    desc.path = AssetPathResolver::DefaultPath("game/цвет", AssetKind::Shader);
    EXPECT_EQ(AssetPathResolver::ResolveShader(desc), content / FromUtf8("shaders/цвет.hlsl"));
    desc.path = AssetPathResolver::DefaultPath("engine/default", AssetKind::Shader);
    EXPECT_EQ(AssetPathResolver::ResolveShader(desc), AssetPathResolver::GetRoot() / "engine/shaders/default.hlsl");
}

struct Fixture {
    std::string prefix = "game/material-tests-" + std::to_string(GetCurrentProcessId());
    std::filesystem::path root = AssetPathResolver::GetRoot() / prefix;
    std::filesystem::path shaders = AssetPathResolver::GetRoot() / "game/shaders" /
                                   std::filesystem::path(prefix).filename();
    HWND window = nullptr;
    Fixture() {
        std::filesystem::create_directories(root);
        std::filesystem::create_directories(shaders);
        window = CreateWindowExW(0, L"STATIC", L"Material asset tests", WS_OVERLAPPEDWINDOW,
                                 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    ~Fixture() {
        if (window) DestroyWindow(window);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::remove_all(shaders, ignored);
    }
    void Write(const std::string& name, const std::string& text) {
        std::ofstream output(root / name, std::ios::binary);
        output << text;
        ASSERT_TRUE(static_cast<bool>(output)) << "could not write test fixture";
    }
    void MaterialFile(const std::string& name, MaterialDefinition definition) {
        std::string json, error;
        ASSERT_TRUE(SerializeMaterialDefinition(definition, json, error)) << error;
        Write(name + ".material.json", json);
    }
    void InstanceFile(const std::string& name, MaterialInstanceDefinition definition) {
        std::string json, error;
        ASSERT_TRUE(SerializeMaterialInstanceDefinition(definition, json, error)) << error;
        Write(name + ".material-instance.json", json);
    }
};

struct Events {
    std::vector<AssetLoadedEvent> loaded;
    std::vector<AssetFailedEvent> failed;
    void Loaded(const AssetLoadedEvent& e) { loaded.push_back(e); }
    void Failed(const AssetFailedEvent& e) { failed.push_back(e); }
};

template <typename T>
void WaitFor(AssetSystem& assets, AssetHandle<T> handle, Events& events) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    FrameContext frame;
    while (!assets.IsReady(handle) && events.failed.empty() && std::chrono::steady_clock::now() < until) {
        assets.Update(frame);
        ++frame.frameIndex;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(events.failed.empty()) << (events.failed.empty() ? "" : events.failed.back().error);
    ASSERT_TRUE(assets.IsReady(handle)) << "asynchronous dependency graph never reached Ready";
}

void Integration() {
    Fixture fixture;
    ASSERT_NE(fixture.window, nullptr) << "could not create hidden D3D test surface";
    Engine engine({.applicationName = "Material tests", .surface = {fixture.window, 64, 64}, .taskThreads = 2});
    auto* rendering = engine.AddSubsystem<RenderingSubsystem>();
    if (!engine.Initialize()) {
        GTEST_SKIP() << "D3D11 hardware/debug layer unavailable";
    }
    auto& assets = engine.GetAssets();
    auto& resources = rendering->GetResourceManager();
    ASSERT_NO_FATAL_FAILURE(fixture.Write("bounds.obj",
        "v 2 3 4\nv 6 3 4\nv 2 7 12\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"));
    const auto meshAsset = assets.LoadMesh(fixture.prefix + "/bounds.obj");
    ASSERT_TRUE(meshAsset);
    EXPECT_EQ(meshAsset->bounds.min, (Vector3{2, 3, 4}));
    EXPECT_EQ(meshAsset->bounds.max, (Vector3{6, 7, 12}));
    ASSERT_NO_FATAL_FAILURE(fixture.Write("normalized.obj",
        "v 2 3 4\nv 6 3 4\nv 2 7 12\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"));
    const auto normalizedMesh = assets.LoadMesh(fixture.prefix + "/normalized.obj", true);
    ASSERT_TRUE(normalizedMesh);
    EXPECT_EQ(normalizedMesh->bounds.min, (Vector3{-0.25f, -0.25f, -0.5f}));
    EXPECT_EQ(normalizedMesh->bounds.max, (Vector3{0.25f, 0.25f, 0.5f}));
    Events events;
    engine.GetEvents().sink<AssetLoadedEvent>().connect<&Events::Loaded>(events);
    engine.GetEvents().sink<AssetFailedEvent>().connect<&Events::Failed>(events);
    const std::string shaderFile = fixture.prefix + "/hooks.hlsl";
    std::ofstream(fixture.shaders / "hooks.hlsl") << R"(
#include "engine/MaterialTemplate.hlsl"
#if defined(MATERIAL_GBUFFER_VS)
void PatchGBufferVertex(inout MaterialVertex vertex) {}
#elif defined(MATERIAL_SHADOW_VS)
void PatchShadowVertex(inout MaterialVertex vertex) {}
#elif defined(MATERIAL_GBUFFER_PS)
void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) { surface.shininess = 16; }
#endif
)";
    MaterialDefinition base;
    base.shaderFile = shaderFile;
    base.hooks = MaterialHook::GBufferVS | MaterialHook::GBufferPS | MaterialHook::ShadowVS;
    base.defaults.albedoTexture = "engine/primitives/white";
    base.defaults.specularColor = Vector3{0.2f, 0.3f, 0.4f};
    base.defaults.shininess = 48.0f;
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("base", base));
    const auto baseAsset = assets.LoadMaterial(fixture.prefix + "/base");
    ASSERT_TRUE(static_cast<bool>(baseAsset)) << "synchronous material failed";
    const auto materialHandle = baseAsset->material;
    const auto* material = resources.TryGetMaterial(materialHandle);
    ASSERT_TRUE(material && material->shaders.shadowPassVS && material->shaders.gBufferPassVS &&
          material->shaders.gBufferPassPS && material->defaultParameters.albedoTexture) << "declared shader set or texture missing";
    ASSERT_TRUE(baseAsset->definition.shaderFile == shaderFile) << "authored material definition not preserved";
    const auto canonical = assets.RequestMaterial(fixture.prefix + "/base.material.json");
    ASSERT_TRUE(canonical == assets.RequestMaterial(fixture.prefix + "/base")) << "extensionless material not deduplicated";
    ASSERT_TRUE(assets.RequestMaterial(fixture.prefix + "/BASE.MATERIAL.JSON") == canonical) << "case normalization";
    ASSERT_TRUE(assets.GetState<MaterialAsset>(fixture.prefix + "/base") == AssetState::Ready &&
          assets.Peek<MaterialAsset>(fixture.prefix + "/base")->material == materialHandle) << "material lookup canonicalization";

    MaterialInstanceDefinition gloss;
    gloss.material = fixture.prefix + "/base";
    gloss.parameters.shininess = 96.0f;
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("gloss", gloss));
    const auto glossAsset = assets.LoadMaterialInstance(fixture.prefix + "/gloss");
    ASSERT_TRUE(static_cast<bool>(glossAsset)) << "synchronous instance failed";
    const auto* instance = resources.TryGetMaterialInstance(glossAsset->instance);
    ASSERT_TRUE(instance && instance->material == materialHandle && instance->parameters.parameters.shininess == 96 &&
          instance->parameters.parameters.specularColor == *base.defaults.specularColor &&
          instance->parameters.albedoTexture == resources.TryGetMaterial(materialHandle)->defaultParameters.albedoTexture) << "sparse overrides did not inherit defaults";
    ASSERT_TRUE(resources.TryGetMaterial(materialHandle)->defaultParameters.parameters.shininess == 48) << "instance mutated base";
    ASSERT_TRUE(!glossAsset->definition.parameters.albedoTexture) << "authored overrides were flattened";
    ASSERT_TRUE(assets.GetState<MaterialInstanceAsset>(fixture.prefix + "/gloss") == AssetState::Ready &&
          assets.Peek<MaterialInstanceAsset>(fixture.prefix + "/gloss")->instance == glossAsset->instance) << "instance lookup canonicalization";
    gloss.parameters.albedoTexture = "";
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("clear", gloss));
    const auto clear = assets.LoadMaterialInstance(fixture.prefix + "/clear");
    ASSERT_TRUE(clear && !resources.TryGetMaterialInstance(clear->instance)->parameters.albedoTexture) << "null did not clear texture";
    ASSERT_TRUE(resources.TryGetMaterial(materialHandle)->instanceCount == 3) << "instance count is not maintained";
    ASSERT_TRUE(!resources.DestroyMaterial(materialHandle)) << "material with live instances was destroyed";

    std::filesystem::copy_file(AssetPathResolver::GetRoot() / "engine/primitives/white/texture.png",
                               fixture.root / "override.png", std::filesystem::copy_options::overwrite_existing);
    gloss.parameters.albedoTexture = fixture.prefix + "/override.png";
    gloss.parameters.specularColor = Vector3{0.8f, 0.1f, 0.2f};
    gloss.parameters.shininess.reset();
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("textured", gloss));
    const auto textured = assets.LoadMaterialInstance(fixture.prefix + "/textured");
    ASSERT_TRUE(static_cast<bool>(textured)) << "texture override failed";
    const auto* texturedInstance = resources.TryGetMaterialInstance(textured->instance);
    ASSERT_TRUE(texturedInstance->parameters.albedoTexture &&
          texturedInstance->parameters.albedoTexture != resources.TryGetMaterial(materialHandle)->defaultParameters.albedoTexture &&
          texturedInstance->parameters.parameters.specularColor == *gloss.parameters.specularColor &&
          texturedInstance->parameters.parameters.shininess == 48) << "texture/color override or removed scalar override failed";

    std::filesystem::copy_file(AssetPathResolver::GetRoot() / "engine/primitives/white/texture.png",
                               fixture.root / "async.png", std::filesystem::copy_options::overwrite_existing);
    base.hooks = MaterialHook::GBufferPS;
    base.defaults.albedoTexture = fixture.prefix + "/async.png";
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("async", base));
    gloss.material = fixture.prefix + "/async";
    gloss.parameters.albedoTexture.reset();
    gloss.parameters.shininess = 0.0f;
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("async", gloss));
    events.loaded.clear();
    events.failed.clear();
    const auto async = assets.RequestMaterialInstance(fixture.prefix + "/async");
    ASSERT_TRUE(async && !assets.Peek(async)) << "Request published before CPU work and dependencies completed";
    ASSERT_TRUE(async == assets.RequestMaterialInstance(fixture.prefix + "/async.material-instance.json")) << "instance deduplication";
    ASSERT_NO_FATAL_FAILURE(WaitFor(assets, async, events));
    const auto asyncAsset = assets.Peek(async);
    const auto* asyncInstance = resources.TryGetMaterialInstance(asyncAsset->instance);
    const auto* asyncMaterial = resources.TryGetMaterial(asyncInstance->material);
    ASSERT_TRUE(asyncMaterial->shaders.gBufferPassPS && !asyncMaterial->shaders.gBufferPassVS &&
          !asyncMaterial->shaders.shadowPassVS) << "undeclared hooks were compiled";
    ASSERT_TRUE(asyncInstance->parameters.parameters.shininess == 0) << "zero override was lost";
    size_t textureEvent = events.loaded.size(), materialEvent = events.loaded.size(), instanceEvent = events.loaded.size();
    for (size_t i = 0; i < events.loaded.size(); ++i) {
        if (events.loaded[i].kind == AssetKind::Texture) textureEvent = i;
        if (events.loaded[i].kind == AssetKind::Material) materialEvent = i;
        if (events.loaded[i].kind == AssetKind::MaterialInstance) instanceEvent = i;
    }
    ASSERT_TRUE(textureEvent < materialEvent && materialEvent < instanceEvent) << "dependency publication order is wrong";

    std::filesystem::copy_file(fixture.shaders / "hooks.hlsl", fixture.shaders / FromUtf8("цвет.hlsl"),
                               std::filesystem::copy_options::overwrite_existing);
    auto unicode = base;
    unicode.shaderFile = fixture.prefix + "/цвет.hlsl";
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("unicode", unicode));
    ASSERT_TRUE(static_cast<bool>(assets.LoadMaterial(fixture.prefix + "/unicode"))) << "UTF-8 shader reference failed";
    const auto legacyUnicode = resources.CreateMaterial({
        .shaderFile = FromUtf8(unicode.shaderFile).wstring(),
        .hooks = MaterialHook::GBufferPS});
    ASSERT_TRUE(static_cast<bool>(legacyUnicode)) << "wstring material API lost Unicode compatibility";
    ASSERT_TRUE(resources.DestroyMaterial(legacyUnicode)) << "legacy material cleanup failed";

    ASSERT_NO_FATAL_FAILURE(fixture.Write("bad.material.json", R"({"version":1,"hooks":["missing"]})"));
    ASSERT_TRUE(!assets.LoadMaterial(fixture.prefix + "/bad")) << "invalid JSON loaded";
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("bad", MaterialDefinition{}));
    ASSERT_TRUE(static_cast<bool>(assets.LoadMaterial(fixture.prefix + "/bad"))) << "failed JSON load could not be retried";
    base.defaults.albedoTexture = fixture.prefix + "/missing.png";
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("missing-texture", base));
    ASSERT_TRUE(!assets.LoadMaterial(fixture.prefix + "/missing-texture")) << "missing texture did not fail the parent";
    base.defaults.albedoTexture.reset();
    base.shaderFile = fixture.prefix + "/missing.hlsl";
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("missing-shader", base));
    ASSERT_TRUE(!assets.LoadMaterial(fixture.prefix + "/missing-shader")) << "missing shader did not fail the material";
    gloss.material = fixture.prefix + "/missing-material";
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("missing-parent", gloss));
    events.failed.clear();
    const auto missing = assets.RequestMaterialInstance(fixture.prefix + "/missing-parent");
    FrameContext frame;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool parentFailed = false;
    while (!parentFailed && std::chrono::steady_clock::now() < until) {
        assets.Update(frame);
        ++frame.frameIndex;
        for (const auto& e : events.failed) parentFailed |= e.kind == AssetKind::MaterialInstance;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(parentFailed && !assets.IsReady(missing) && !assets.Peek(missing)) << "failed dependency did not fail async instance";
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("missing-parent", MaterialInstanceDefinition{.material = fixture.prefix + "/base"}));
    const auto retried = assets.RequestMaterialInstance(fixture.prefix + "/missing-parent");
    ASSERT_TRUE(retried != missing) << "retry reused a stale handle";
    events.failed.clear();
    ASSERT_NO_FATAL_FAILURE(WaitFor(assets, retried, events));
    ASSERT_TRUE(!assets.Peek(missing)) << "stale failed handle became readable";

    MaterialDefinition guidShader;
    guidShader.version = 2;
    guidShader.hooks = MaterialHook::GBufferPS;
    guidShader.shaderReference = AssetReference{"4f860052-e658-479b-bf3a-4e0143f4881f",
                                                 "game/shaders/old-name.hlsl"};
    guidShader.shaderFile = guidShader.shaderReference->path;
    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("uuid-shader", guidShader));
    const auto guidMaterial = assets.LoadMaterial(fixture.prefix + "/uuid-shader");
    ASSERT_TRUE(guidMaterial) << "UUID shader did not survive stale path hint";
    EXPECT_TRUE(resources.TryGetMaterial(guidMaterial->material)->shaders.gBufferPassPS);

    for (std::string_view name : {"ground", "enchanted", "marker-orange", "marker-blue",
                                  "marker-green", "marker-magenta"}) {
        const auto authored = assets.LoadMaterialInstance("game/materials/" + std::string(name) + ".material-instance.json");
        ASSERT_TRUE(authored) << name << " ExampleGame disk material did not load";
        EXPECT_NE(resources.TryGetMaterialInstance(authored->instance), nullptr);
    }

    const auto builtIn = assets.LoadMaterialInstance("engine/primitives/default.material-instance.json");
    ASSERT_TRUE(builtIn) << "version 2 UUID dependency graph failed";
    const auto oldBuiltin = assets.RequestMaterialInstance("engine/primitives/default.material-instance.json");
    ASSERT_TRUE(resources.TryGetMaterialInstance(builtIn->instance));
    assets.UnloadAll();
    EXPECT_FALSE(assets.Peek(oldBuiltin));
    EXPECT_EQ(resources.TryGetMaterialInstance(builtIn->instance), nullptr);
    const auto reloaded = assets.LoadMaterialInstance("engine/primitives/default.material-instance.json");
    ASSERT_TRUE(reloaded) << "asset did not reload after GPU eviction";
    EXPECT_NE(assets.RequestMaterialInstance("engine/primitives/default.material-instance.json"), oldBuiltin);
    auto& sceneAssets = engine.GetSceneAssets();
    auto* extraction = engine.AddSubsystem<RenderExtractionSubsystem>();
    ASSERT_NE(extraction, nullptr);
    std::string sceneError;
    ASSERT_TRUE(engine.LoadScene(AssetReference{"2a9a7998-f44d-44d8-8fc3-57e78da18796",
                                               "game/scenes/old-name.scene.json"}, sceneError)) << sceneError;
    const auto cube = engine.GetScene().FindById("c8727a24-84b3-4fe7-b1d8-291ba9d2e8df");
    ASSERT_TRUE(cube != entt::null);
    const auto sceneHandle = engine.GetScene().TryGetComponent<MeshRenderer>(cube)->materialInstance;
    const auto untilScene = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    FrameContext sceneFrame;
    while (!sceneAssets.IsReady(sceneHandle) && std::chrono::steady_clock::now() < untilScene) {
        sceneAssets.Update(sceneFrame);
        ++sceneFrame.frameIndex;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sceneAssets.IsReady(sceneHandle)) << "example scene asset did not load";
    const auto marker = engine.GetScene().FindById("3b93eafd-a888-4720-be22-77c5ea028bca");
    ASSERT_TRUE(marker != entt::null);
    const auto* markerRenderer = engine.GetScene().TryGetComponent<MeshRenderer>(marker);
    ASSERT_NE(markerRenderer, nullptr);
    while ((!sceneAssets.IsReady(markerRenderer->mesh) || !sceneAssets.IsReady(markerRenderer->materialInstance)) &&
           std::chrono::steady_clock::now() < untilScene) {
        sceneAssets.Update(sceneFrame);
        ++sceneFrame.frameIndex;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(sceneAssets.IsReady(markerRenderer->mesh)) << "marker mesh did not load";
    ASSERT_TRUE(sceneAssets.IsReady(markerRenderer->materialInstance)) << "marker material did not load";
    const auto otherCube = engine.GetScene().CreateEntity("Same material");
    ASSERT_NE(engine.GetScene().AddComponent<MeshRenderer>(otherCube,
        *engine.GetScene().TryGetComponent<MeshRenderer>(cube)), nullptr);
    const auto baseInstance = sceneAssets.Peek(sceneHandle)->instance;
    const auto baseShininess = resources.TryGetMaterialInstance(baseInstance)->parameters.parameters.shininess;
    ASSERT_TRUE(engine.EditEntityMaterial(cube, [](MaterialParameters& parameters) {
        parameters.parameters.shininess = 17;
    }, sceneError)) << sceneError;
    const auto clone = engine.GetEntityMaterialOverride(cube);
    ASSERT_TRUE(clone);
    EXPECT_NE(clone, baseInstance);
    EXPECT_FALSE(engine.GetEntityMaterialOverride(otherCube));
    EXPECT_FLOAT_EQ(resources.TryGetMaterialInstance(baseInstance)->parameters.parameters.shininess, baseShininess);
    EXPECT_FLOAT_EQ(resources.TryGetMaterialInstance(clone)->parameters.parameters.shininess, 17);
    engine.GetScene().DestroyEntity(cube);
    EXPECT_EQ(resources.TryGetMaterialInstance(clone), nullptr);
    ASSERT_TRUE(engine.EditEntityMaterial(otherCube, [](MaterialParameters& parameters) {
        parameters.parameters.shininess = 19;
    }, sceneError)) << sceneError;
    const auto otherClone = engine.GetEntityMaterialOverride(otherCube);
    ASSERT_TRUE(otherClone);
    EXPECT_TRUE(engine.GetScene().RemoveComponent<MeshRenderer>(otherCube));
    EXPECT_FALSE(engine.GetEntityMaterialOverride(otherCube));
    EXPECT_EQ(resources.TryGetMaterialInstance(otherClone), nullptr);
    rendering->GetWorld().renderItems.push_back({}); // host-owned list is not touched by extraction
    extraction->LateUpdate(sceneFrame);
    EXPECT_EQ(rendering->GetWorld().renderItems.size(), 1u);
    EXPECT_GE(rendering->GetWorld().sceneItems.size(), 1u);
    rendering->GetWorld().renderItems.clear();
    const auto sceneInstance = sceneAssets.Peek(sceneHandle);
    ASSERT_TRUE(sceneInstance);
    ASSERT_NO_FATAL_FAILURE(fixture.Write("empty.scene.json", R"({"version":1,"entities":[]})"));
    ASSERT_TRUE(engine.LoadScene(fixture.root / "empty.scene.json", sceneError)) << sceneError;
    EXPECT_FALSE(sceneAssets.Peek(sceneHandle));
    EXPECT_EQ(resources.TryGetMaterialInstance(sceneInstance->instance), nullptr);
    EXPECT_NE(resources.TryGetMaterialInstance(reloaded->instance), nullptr)
        << "switching scenes destroyed a host-owned material instance";
    ASSERT_TRUE(engine.LoadScene(AssetReference{"2a9a7998-f44d-44d8-8fc3-57e78da18796",
                                               "game/scenes/demo.scene.json"}, sceneError)) << sceneError;
    const auto newCube = engine.GetScene().FindById("c8727a24-84b3-4fe7-b1d8-291ba9d2e8df");
    ASSERT_TRUE(newCube != entt::null);
    const auto nextHandle = engine.GetScene().TryGetComponent<MeshRenderer>(newCube)->materialInstance;
    EXPECT_NE(nextHandle, sceneHandle);
    EXPECT_FALSE(sceneAssets.Peek(sceneHandle));
    ASSERT_TRUE(engine.LoadScene(fixture.root / "empty.scene.json", sceneError)) << sceneError;

    ASSERT_NO_FATAL_FAILURE(fixture.MaterialFile("shutdown", MaterialDefinition{}));
    ASSERT_NO_FATAL_FAILURE(fixture.InstanceFile("shutdown", MaterialInstanceDefinition{.material = fixture.prefix + "/shutdown"}));
    assets.RequestMaterialInstance(fixture.prefix + "/shutdown");
    engine.Shutdown();
}

TEST(MaterialAssetsIntegration, LoadsDependenciesAndSurvivesEngineRestart) {
    // Keep both lifetimes in one process to exercise DirectXTK's cached WIC factory.
    for (int restart = 0; restart < 2; ++restart) {
        SCOPED_TRACE(restart);
        ASSERT_NO_FATAL_FAILURE(Integration());
        if (::testing::Test::IsSkipped()) return;
    }
}
} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
