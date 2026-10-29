#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/PathUtils.hpp>

#include <array>
#include <utility>

namespace engine {
namespace {

bool ReadDefinition(const AssetDesc& desc, bool instance, std::string& text, std::string& error) {
    const auto path = AssetPathResolver::ResolveMaterial(desc, instance);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { error = "cannot open material JSON: " + ToUtf8(path); return false; }
    const auto size = input.tellg();
    if (size <= 0 || size > 1024 * 1024) {
        error = "material JSON must contain between 1 byte and 1 MiB: " + ToUtf8(path);
        return false;
    }
    text.resize(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(text.data(), static_cast<std::streamsize>(size))) {
        error = "failed to read material JSON: " + ToUtf8(path);
        return false;
    }
    return true;
}

struct HookStage { MaterialHook hook; ShaderType stage; const char* define; };
constexpr std::array Stages{
    HookStage{MaterialHook::ShadowVS, ShaderType::Vertex, "MATERIAL_SHADOW_VS"},
    HookStage{MaterialHook::GBufferVS, ShaderType::Vertex, "MATERIAL_GBUFFER_VS"},
    HookStage{MaterialHook::GBufferPS, ShaderType::Pixel, "MATERIAL_GBUFFER_PS"}
};

} // namespace

bool MaterialImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload, std::string& error) const {
    std::string text;
    MaterialCpu parsed;
    if (!ReadDefinition(desc, false, text, error) || !ParseMaterialDefinition(text, parsed.definition, error))
        return false;
    if (parsed.definition.hooks != MaterialHook::None) {
        auto path = AssetPathResolver::ResolveShader({
            .path = AssetPathResolver::DefaultPath(parsed.definition.shaderFile, AssetKind::Shader)});
        if (parsed.definition.version == 2) {
            const CatalogEntry* entry = desc.catalog
                ? desc.catalog->Find(parsed.definition.shaderReference->id, CatalogAssetKind::Shader) : nullptr;
            if (!entry) { error = "missing shader UUID: " + parsed.definition.shaderReference->id.ToString(); return false; }
            path = entry->source;
        }
        for (size_t i = 0; i < Stages.size(); ++i) {
            const auto& stage = Stages[i];
            if (!HasHook(parsed.definition.hooks, stage.hook)) continue;
            auto compiled = ShaderContext::CompileCpu(path, stage.stage,
                {.defines = {{stage.define, "1"}, {"MATERIAL_HAS_HOOK", "1"}}});
            if (!compiled) {
                error = "HLSL compilation failed for material '" + desc.name + "' (" + stage.define + ")";
                return false;
            }
            parsed.shaders[i] = std::move(*compiled);
        }
    }
    payload.kind = CpuPayload::Kind::Material;
    payload.material = std::move(parsed);
    return true;
}

bool MaterialImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                  GpuPayload& result, std::string& error) const {
    if (payload.kind != CpuPayload::Kind::Material) {
        error = "MaterialImporter: wrong payload kind";
        return false;
    }
    ShaderSet shaders;
    const std::array targets{&shaders.shadowPassVS, &shaders.gBufferPassVS, &shaders.gBufferPassPS};
    const auto rollback = [&] {
        for (const auto target : targets) if (*target) transfer.resources->DestroyShader(*target);
    };
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!HasHook(payload.material.definition.hooks, Stages[i].hook)) continue;
        *targets[i] = transfer.resources->RegisterCompiledShader(payload.material.shaders[i]);
        if (!*targets[i]) {
            rollback();
            error = "material shader registration on device failed";
            return false;
        }
    }
    result.material = transfer.resources->RegisterMaterial(shaders, transfer.materialParameters);
    if (!result.material) {
        rollback();
        error = "material registration failed";
        return false;
    }
    return true;
}

bool MaterialInstanceImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload,
                                       std::string& error) const {
    std::string text;
    MaterialInstanceDefinition parsed;
    if (!ReadDefinition(desc, true, text, error) || !ParseMaterialInstanceDefinition(text, parsed, error))
        return false;
    payload.kind = CpuPayload::Kind::MaterialInstance;
    payload.materialInstance = std::move(parsed);
    return true;
}

bool MaterialInstanceImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                         GpuPayload& result, std::string& error) const {
    if (payload.kind != CpuPayload::Kind::MaterialInstance) {
        error = "MaterialInstanceImporter: wrong payload kind";
        return false;
    }
    const auto instance = transfer.resources->CreateMaterialInstance(transfer.material);
    if (!instance || !transfer.resources->SetMaterialInstanceParameters(instance, transfer.materialParameters)) {
        if (instance) transfer.resources->DestroyMaterialInstance(instance);
        error = "material instance registration failed";
        return false;
    }
    result.materialInstance = instance;
    return true;
}

} // namespace engine
