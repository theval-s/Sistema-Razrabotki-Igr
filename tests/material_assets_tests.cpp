#include <engine/assets/AssetEvents.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace engine;
void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void Definitions() {
    MaterialDefinition material;
    std::string error, json;
    Check(ParseMaterialDefinition(R"({"version":1,"shaderFile":"game/test.hlsl",
        "hooks":["GBufferPS"],"defaults":{"albedoTexture":"game/white.png","shininess":8}})", material, error),
        error);
    Check(material.hooks == MaterialHook::GBufferPS && material.defaults.shininess == 8, "parsed material");
    Check(SerializeMaterialDefinition(material, json, error), error);
    MaterialDefinition roundTrip;
    Check(ParseMaterialDefinition(json, roundTrip, error), error);
    Check(roundTrip.shaderFile == material.shaderFile && roundTrip.hooks == material.hooks &&
          roundTrip.defaults.albedoTexture == material.defaults.albedoTexture, "material round trip");
    Check(ParseMaterialDefinition("\xEF\xBB\xBF" R"({"version":1,"hooks":[]})", material, error), "UTF-8 BOM");
    Check(material.hooks == MaterialHook::None && material.shaderFile.empty(), "built-in defaults");

    const std::vector<std::string> invalidMaterials{
        R"({"version":1})", R"({"version":2,"hooks":[]})",
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
        material.shaderFile = "unchanged";
        Check(!ParseMaterialDefinition(invalid, material, error) && !error.empty(), "invalid material accepted");
        Check(material.shaderFile == "unchanged", "failed parse overwrote the output");
    }
    MaterialInstanceDefinition instance;
    Check(ParseMaterialInstanceDefinition(R"({"version":1,"material":"game/base","parameters":{}})", instance, error),
          error);
    Check(!instance.parameters.albedoTexture && !instance.parameters.shininess, "absent overrides inherit");
    Check(ParseMaterialInstanceDefinition(R"({"version":1,"material":"game/base","parameters":{
        "albedoTexture":null,"shininess":0,"specularColor":[0,0,0]}})", instance, error), error);
    Check(instance.parameters.albedoTexture && instance.parameters.albedoTexture->empty() &&
          instance.parameters.shininess == 0, "explicit null and zero are overrides");
    Check(SerializeMaterialInstanceDefinition(instance, json, error), error);
    MaterialInstanceDefinition decoded;
    Check(ParseMaterialInstanceDefinition(json, decoded, error), error);
    Check(decoded.parameters.albedoTexture && decoded.parameters.albedoTexture->empty(), "null round trip");
    for (const auto& invalid : {
        R"({"version":1,"material":""})", R"({"version":1,"material":"../base"})",
        R"({"version":1,"material":"game/base","parameters":{"albedoTexture":""}})",
        R"({"version":1,"material":"game/base","hooks":[]})",
        R"({"version":1,"material":"game/base","parameters":{"shininess":null}})"}) {
        Check(!ParseMaterialInstanceDefinition(invalid, decoded, error), "invalid instance accepted");
    }
    instance.parameters.shininess = std::numeric_limits<float>::infinity();
    json = "unchanged";
    Check(!SerializeMaterialInstanceDefinition(instance, json, error) && json == "unchanged", "infinite value saved");
    material = {};
    material.hooks = static_cast<MaterialHook>(0x80);
    Check(!SerializeMaterialDefinition(material, json, error), "unknown hook bits saved");
    material = {};
    material.shaderFile = "game/материалы/цвет.hlsl";
    Check(SerializeMaterialDefinition(material, json, error), "UTF-8 reference serialization");
    Check(ParseMaterialDefinition(json, roundTrip, error) && roundTrip.shaderFile == material.shaderFile, "UTF-8 round trip");

    AssetTable<MaterialAsset> table;
    std::vector<AssetHandle<MaterialAsset>> stale;
    for (int i = 0; i < 5; ++i) {
        const auto handle = table.Acquire(AssetDesc{.name = "retry"});
        for (auto old : stale) Check(table.Get(old) == nullptr, "stale generation was reused");
        stale.push_back(handle);
        table.Fail(handle, "test");
    }
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
        Check(window != nullptr, "could not create hidden D3D test surface");
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
        Check(static_cast<bool>(output), "could not write test fixture");
    }
    void MaterialFile(const std::string& name, MaterialDefinition definition) {
        std::string json, error;
        Check(SerializeMaterialDefinition(definition, json, error), error);
        Write(name + ".material.json", json);
    }
    void InstanceFile(const std::string& name, MaterialInstanceDefinition definition) {
        std::string json, error;
        Check(SerializeMaterialInstanceDefinition(definition, json, error), error);
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
    Check(events.failed.empty(), events.failed.empty() ? "" : events.failed.back().error);
    Check(assets.IsReady(handle), "asynchronous dependency graph never reached Ready");
}

int Integration() {
    Fixture fixture;
    Engine engine({.applicationName = "Material tests", .surface = {fixture.window, 64, 64}, .taskThreads = 2});
    auto* rendering = engine.AddSubsystem<RenderingSubsystem>();
    if (!engine.Initialize()) {
        std::cerr << "D3D11 hardware/debug layer unavailable; integration test skipped\n";
        return 77;
    }
    auto& assets = engine.GetAssets();
    auto& resources = rendering->GetResourceManager();
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
    fixture.MaterialFile("base", base);
    const auto baseAsset = assets.LoadMaterial(fixture.prefix + "/base");
    Check(static_cast<bool>(baseAsset), "synchronous material failed");
    const auto materialHandle = baseAsset->material;
    const auto* material = resources.TryGetMaterial(materialHandle);
    Check(material && material->shaders.shadowPassVS && material->shaders.gBufferPassVS &&
          material->shaders.gBufferPassPS && material->defaultParameters.albedoTexture, "declared shader set or texture missing");
    Check(baseAsset->definition.shaderFile == shaderFile, "authored material definition not preserved");
    const auto canonical = assets.RequestMaterial(fixture.prefix + "/base.material.json");
    Check(canonical == assets.RequestMaterial(fixture.prefix + "/base"), "extensionless material not deduplicated");
    Check(assets.RequestMaterial(fixture.prefix + "/BASE.MATERIAL.JSON") == canonical, "case normalization");
    Check(assets.GetState<MaterialAsset>(fixture.prefix + "/base") == AssetState::Ready &&
          assets.Peek<MaterialAsset>(fixture.prefix + "/base")->material == materialHandle, "material lookup canonicalization");

    MaterialInstanceDefinition gloss;
    gloss.material = fixture.prefix + "/base";
    gloss.parameters.shininess = 96.0f;
    fixture.InstanceFile("gloss", gloss);
    const auto glossAsset = assets.LoadMaterialInstance(fixture.prefix + "/gloss");
    Check(static_cast<bool>(glossAsset), "synchronous instance failed");
    const auto* instance = resources.TryGetMaterialInstance(glossAsset->instance);
    Check(instance && instance->material == materialHandle && instance->parameters.parameters.shininess == 96 &&
          instance->parameters.parameters.specularColor == *base.defaults.specularColor &&
          instance->parameters.albedoTexture == resources.TryGetMaterial(materialHandle)->defaultParameters.albedoTexture,
          "sparse overrides did not inherit defaults");
    Check(resources.TryGetMaterial(materialHandle)->defaultParameters.parameters.shininess == 48, "instance mutated base");
    Check(!glossAsset->definition.parameters.albedoTexture, "authored overrides were flattened");
    Check(assets.GetState<MaterialInstanceAsset>(fixture.prefix + "/gloss") == AssetState::Ready &&
          assets.Peek<MaterialInstanceAsset>(fixture.prefix + "/gloss")->instance == glossAsset->instance,
          "instance lookup canonicalization");
    gloss.parameters.albedoTexture = "";
    fixture.InstanceFile("clear", gloss);
    const auto clear = assets.LoadMaterialInstance(fixture.prefix + "/clear");
    Check(clear && !resources.TryGetMaterialInstance(clear->instance)->parameters.albedoTexture, "null did not clear texture");
    Check(resources.TryGetMaterial(materialHandle)->instanceCount == 3, "instance count is not maintained");
    Check(!resources.DestroyMaterial(materialHandle), "material with live instances was destroyed");

    std::filesystem::copy_file(AssetPathResolver::GetRoot() / "engine/primitives/white/texture.png",
                               fixture.root / "override.png", std::filesystem::copy_options::overwrite_existing);
    gloss.parameters.albedoTexture = fixture.prefix + "/override.png";
    gloss.parameters.specularColor = Vector3{0.8f, 0.1f, 0.2f};
    gloss.parameters.shininess.reset();
    fixture.InstanceFile("textured", gloss);
    const auto textured = assets.LoadMaterialInstance(fixture.prefix + "/textured");
    Check(static_cast<bool>(textured), "texture override failed");
    const auto* texturedInstance = resources.TryGetMaterialInstance(textured->instance);
    Check(texturedInstance->parameters.albedoTexture &&
          texturedInstance->parameters.albedoTexture != resources.TryGetMaterial(materialHandle)->defaultParameters.albedoTexture &&
          texturedInstance->parameters.parameters.specularColor == *gloss.parameters.specularColor &&
          texturedInstance->parameters.parameters.shininess == 48, "texture/color override or removed scalar override failed");

    std::filesystem::copy_file(AssetPathResolver::GetRoot() / "engine/primitives/white/texture.png",
                               fixture.root / "async.png", std::filesystem::copy_options::overwrite_existing);
    base.hooks = MaterialHook::GBufferPS;
    base.defaults.albedoTexture = fixture.prefix + "/async.png";
    fixture.MaterialFile("async", base);
    gloss.material = fixture.prefix + "/async";
    gloss.parameters.albedoTexture.reset();
    gloss.parameters.shininess = 0.0f;
    fixture.InstanceFile("async", gloss);
    events.loaded.clear();
    events.failed.clear();
    const auto async = assets.RequestMaterialInstance(fixture.prefix + "/async");
    Check(async && !assets.Peek(async), "Request published before CPU work and dependencies completed");
    Check(async == assets.RequestMaterialInstance(fixture.prefix + "/async.material-instance.json"), "instance deduplication");
    WaitFor(assets, async, events);
    const auto asyncAsset = assets.Peek(async);
    const auto* asyncInstance = resources.TryGetMaterialInstance(asyncAsset->instance);
    const auto* asyncMaterial = resources.TryGetMaterial(asyncInstance->material);
    Check(asyncMaterial->shaders.gBufferPassPS && !asyncMaterial->shaders.gBufferPassVS &&
          !asyncMaterial->shaders.shadowPassVS, "undeclared hooks were compiled");
    Check(asyncInstance->parameters.parameters.shininess == 0, "zero override was lost");
    size_t textureEvent = events.loaded.size(), materialEvent = events.loaded.size(), instanceEvent = events.loaded.size();
    for (size_t i = 0; i < events.loaded.size(); ++i) {
        if (events.loaded[i].kind == AssetKind::Texture) textureEvent = i;
        if (events.loaded[i].kind == AssetKind::Material) materialEvent = i;
        if (events.loaded[i].kind == AssetKind::MaterialInstance) instanceEvent = i;
    }
    Check(textureEvent < materialEvent && materialEvent < instanceEvent, "dependency publication order is wrong");

    std::filesystem::copy_file(fixture.shaders / "hooks.hlsl", fixture.shaders / AssetPathResolver::Utf8Path("цвет.hlsl"),
                               std::filesystem::copy_options::overwrite_existing);
    auto unicode = base;
    unicode.shaderFile = fixture.prefix + "/цвет.hlsl";
    fixture.MaterialFile("unicode", unicode);
    Check(static_cast<bool>(assets.LoadMaterial(fixture.prefix + "/unicode")), "UTF-8 shader reference failed");
    const auto legacyUnicode = resources.CreateMaterial({
        .shaderFile = AssetPathResolver::Utf8Path(unicode.shaderFile).wstring(),
        .hooks = MaterialHook::GBufferPS});
    Check(static_cast<bool>(legacyUnicode), "wstring material API lost Unicode compatibility");
    Check(resources.DestroyMaterial(legacyUnicode), "legacy material cleanup failed");

    fixture.Write("bad.material.json", R"({"version":1,"hooks":["missing"]})");
    Check(!assets.LoadMaterial(fixture.prefix + "/bad"), "invalid JSON loaded");
    fixture.MaterialFile("bad", MaterialDefinition{});
    Check(static_cast<bool>(assets.LoadMaterial(fixture.prefix + "/bad")), "failed JSON load could not be retried");
    base.defaults.albedoTexture = fixture.prefix + "/missing.png";
    fixture.MaterialFile("missing-texture", base);
    Check(!assets.LoadMaterial(fixture.prefix + "/missing-texture"), "missing texture did not fail the parent");
    base.defaults.albedoTexture.reset();
    base.shaderFile = fixture.prefix + "/missing.hlsl";
    fixture.MaterialFile("missing-shader", base);
    Check(!assets.LoadMaterial(fixture.prefix + "/missing-shader"), "missing shader did not fail the material");
    gloss.material = fixture.prefix + "/missing-material";
    fixture.InstanceFile("missing-parent", gloss);
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
    Check(parentFailed && !assets.IsReady(missing) && !assets.Peek(missing), "failed dependency did not fail async instance");
    fixture.InstanceFile("missing-parent", MaterialInstanceDefinition{.material = fixture.prefix + "/base"});
    const auto retried = assets.RequestMaterialInstance(fixture.prefix + "/missing-parent");
    Check(retried != missing, "retry reused a stale handle");
    events.failed.clear();
    WaitFor(assets, retried, events);
    Check(!assets.Peek(missing), "stale failed handle became readable");

    fixture.MaterialFile("shutdown", MaterialDefinition{});
    fixture.InstanceFile("shutdown", MaterialInstanceDefinition{.material = fixture.prefix + "/shutdown"});
    assets.RequestMaterialInstance(fixture.prefix + "/shutdown");
    engine.Shutdown();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--integration") {
            const auto result = Integration();
            return result == 0 ? Integration() : result; // WIC must also survive engine/scheduler restart
        }
        Definitions();
        std::cout << "Material definition and generational-handle tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TEST FAILURE: " << e.what() << '\n';
        return 1;
    }
}
