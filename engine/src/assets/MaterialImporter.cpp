#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include "../rendering/RenderingFailureUtils.hpp"

#include <array>
#include <utility>

namespace engine {
namespace {

bool ReadDefinition(const AssetDesc& desc, bool instance, std::string& text, std::string& error) {
    return ReadTextFile(AssetPathResolver::ResolveMaterial(desc, instance), text, error, 1024 * 1024);
}

struct HookStage { MaterialHook hook; ShaderType stage; const char* define; bool skinned = false; };
constexpr std::array Stages{
    HookStage{MaterialHook::ShadowVS, ShaderType::Vertex, "MATERIAL_SHADOW_VS"},
    HookStage{MaterialHook::GBufferVS, ShaderType::Vertex, "MATERIAL_GBUFFER_VS"},
    HookStage{MaterialHook::GBufferPS, ShaderType::Pixel, "MATERIAL_GBUFFER_PS"},
    HookStage{MaterialHook::GBufferVS, ShaderType::Vertex, "MATERIAL_GBUFFER_VS", true},
    HookStage{MaterialHook::ShadowVS, ShaderType::Vertex, "MATERIAL_SHADOW_VS", true}
};

} // namespace

bool MaterialShaderFile(const MaterialDefinition& definition, const AssetCatalog* catalog,
                        std::filesystem::path& file, std::string& error) {
    if (definition.version == 2) {
        const CatalogEntry* entry = catalog && definition.shaderReference
            ? catalog->Find(definition.shaderReference->id, CatalogAssetKind::Shader) : nullptr;
        if (!entry) {
            error = "missing shader UUID: " +
                (definition.shaderReference ? definition.shaderReference->id.ToString() : std::string("(none)"));
            return false;
        }
        file = entry->source;
        return true;
    }
    file = AssetPathResolver::ResolveShader({
        .path = AssetPathResolver::DefaultPath(definition.shaderFile, AssetKind::Shader)});
    return true;
}

std::array<std::optional<ShaderVariant>, 5> MaterialShaderVariants(const std::filesystem::path& file,
                                                                   MaterialHook hooks) {
    std::array<std::optional<ShaderVariant>, 5> variants;
    for (size_t i = 0; i < Stages.size(); ++i) {
        const auto& stage = Stages[i];
        if (!HasHook(hooks, stage.hook)) continue;
        ShaderCompileOptions options{.defines = {{stage.define, "1"}, {"MATERIAL_HAS_HOOK", "1"}}};
        if (stage.skinned) options.defines.push_back({"ENGINE_SKINNED", "1"});
        variants[i] = ShaderVariant{file, stage.stage, std::move(options)};
    }
    return variants;
}

bool MaterialImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload, std::string& error) const try {
    std::string text;
    MaterialCpu parsed;
    if (!ReadDefinition(desc, false, text, error) || !ParseMaterialDefinition(text, parsed.definition, error))
        return false;
    if (parsed.definition.hooks != MaterialHook::None) {
        std::filesystem::path path;
        if (!MaterialShaderFile(parsed.definition, desc.catalog, path, error)) return false;
        const auto variants = MaterialShaderVariants(path, parsed.definition.hooks);
        for (size_t i = 0; i < variants.size(); ++i) {
            if (!variants[i]) continue;
            parsed.shaders[i] = ShaderContext::CompileCpu(variants[i]->file, variants[i]->stage, variants[i]->options);
        }
    }
    payload.kind = CpuPayload::Kind::Material;
    payload.material = std::move(parsed);
    return true;
} catch (const RenderingException& e) {
    error = rendering_failure::Format(e.GetFailure());
    return false;
} catch (const std::exception& e) {
    error = e.what();
    return false;
} catch (...) {
    error = "MaterialImporter: unknown read failure";
    return false;
}

bool MaterialImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                  GpuPayload& result, std::string& error) const {
    if (!transfer.resources) { error = "MaterialImporter: missing resource manager"; return false; }
    if (payload.kind != CpuPayload::Kind::Material) {
        error = "MaterialImporter: wrong payload kind";
        return false;
    }
    ShaderSet shaders{};
    const std::array targets{&shaders.shadowPassVS, &shaders.gBufferPassVS, &shaders.gBufferPassPS,
        &shaders.skinnedGBufferPassVS, &shaders.skinnedShadowPassVS};
    const auto rollback = [&] {
        for (const auto target : targets) if (*target) transfer.resources->DestroyShader(*target);
    };
    try {
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!HasHook(payload.material.definition.hooks, Stages[i].hook)) continue;
            *targets[i] = transfer.resources->RegisterCompiledShader(payload.material.shaders[i]);
        }
        result.material = transfer.resources->RegisterMaterial(shaders, transfer.materialParameters);
        if (!result.material) {
            rollback();
            error = "material registration failed";
            return false;
        }
        return true;
    } catch (const RenderingException& e) {
        rollback();
        error = rendering_failure::Format(e.GetFailure());
    } catch (const std::exception& e) {
        rollback();
        error = e.what();
    } catch (...) {
        rollback();
        error = "MaterialImporter: unknown upload failure";
    }
    return false;
}

bool MaterialInstanceImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload,
                                       std::string& error) const try {
    std::string text;
    MaterialInstanceDefinition parsed;
    if (!ReadDefinition(desc, true, text, error) || !ParseMaterialInstanceDefinition(text, parsed, error))
        return false;
    payload.kind = CpuPayload::Kind::MaterialInstance;
    payload.materialInstance = std::move(parsed);
    return true;
} catch (const RenderingException& e) {
    error = rendering_failure::Format(e.GetFailure());
    return false;
} catch (const std::exception& e) {
    error = e.what();
    return false;
} catch (...) {
    error = "MaterialInstanceImporter: unknown read failure";
    return false;
}

MaterialInstance::Overrides MaterialInstanceImporter::MakeOverrides(const MaterialParameterOverrides& authored,
                                                                     TextureHandle explicitTexture) {
    MaterialInstance::Overrides result;
    result.specularColor = authored.specularColor;
    result.shininess = authored.shininess;
    if (authored.albedoTexture) result.albedoTexture = explicitTexture;
    return result;
}

bool MaterialInstanceImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                         GpuPayload& result, std::string& error) const {
    if (!transfer.resources) { error = "MaterialInstanceImporter: missing resource manager"; return false; }
    if (payload.kind != CpuPayload::Kind::MaterialInstance) {
        error = "MaterialInstanceImporter: wrong payload kind";
        return false;
    }
    MaterialInstanceHandle instance;
    try {
        instance = transfer.resources->CreateMaterialInstance(transfer.material);
        if (!instance || !transfer.resources->SetMaterialInstanceOverrides(instance,
            MakeOverrides(payload.materialInstance.parameters, transfer.materialParameters.albedoTexture))) {
            if (instance) transfer.resources->DestroyMaterialInstance(instance);
            error = "material instance registration failed";
            return false;
        }
        result.materialInstance = instance;
        return true;
    } catch (const RenderingException& e) {
        error = rendering_failure::Format(e.GetFailure());
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "MaterialInstanceImporter: unknown upload failure";
    }
    transfer.resources->DestroyMaterialInstance(instance);
    return false;
}

} // namespace engine
