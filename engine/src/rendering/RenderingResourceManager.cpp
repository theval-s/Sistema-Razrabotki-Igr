#include <engine/rendering/RenderingResourceManager.hpp>

#include "RenderingFailureUtils.hpp"
#include <climits>
#include <cmath>
#include <cstring>
#include <utility>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {
bool IsMaterialTexture(ID3D11ShaderResourceView* view) {
    if (!view) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    view->GetDesc(&desc);
    return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
}

bool ValidMaterialTexture(const RenderingResourceManager& resources, TextureHandle handle) {
    if (!handle) return true;
    const auto* texture = resources.TryGetTexture(handle);
    return texture && IsMaterialTexture(texture->GetSRV());
}

bool ValidEmission(const Vector3& emission) {
    return std::isfinite(emission.x) && emission.x >= 0 &&
           std::isfinite(emission.y) && emission.y >= 0 &&
           std::isfinite(emission.z) && emission.z >= 0;
}

bool ValidLightingWeight(float weight) {
    return std::isfinite(weight) && weight >= 0 && weight <= 1;
}

bool ValidSurface(const MaterialPBRParameters& parameters) {
    return ValidEmission(parameters.emission) && ValidLightingWeight(parameters.lightingWeight);
}

bool ValidSurface(const MaterialInstance::Overrides& overrides) {
    return (!overrides.emission || ValidEmission(*overrides.emission)) &&
           (!overrides.lightingWeight || ValidLightingWeight(*overrides.lightingWeight));
}

// Validate at material registration, including raw native ShaderSet users.
// Missing MRT3 writes cannot be repaired by a clear or a write mask: they can
// retain the contribution of previously drawn, now-occluded geometry.
bool ValidateMaterialShader(const ShaderContext& shader, bool gBufferPixel) {
    const auto reject = [](const char* reason) {
        log::Render().error("Material shader ABI 2: {} Recompile with MaterialTemplate.hlsl or use "
                           "GBufferOutput/EncodeMaterialSurface from ShaderTypes.hlsl.", reason);
        return false;
    };
    ComPtr<ID3D11ShaderReflection> reflection;
    if (!shader.blob || FAILED(D3DReflect(shader.blob->GetBufferPointer(), shader.blob->GetBufferSize(),
                                         IID_PPV_ARGS(&reflection))))
        return reject("Cannot reflect material shader.");
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc))) return reject("Cannot inspect material shader.");
    if (gBufferPixel) {
        unsigned targets = 0;
        for (UINT i = 0; i < desc.OutputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC output{};
            if (FAILED(reflection->GetOutputParameterDesc(i, &output)))
                return reject("Cannot inspect G-buffer outputs.");
            if (output.SystemValueType != D3D_NAME_TARGET) continue;
            if (output.SemanticIndex > 3 || output.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32 ||
                output.Mask != 15 || (output.ReadWriteMask & output.Mask) != 0)
                return reject("G-buffer pixel shaders must write float4 SV_Target0 through SV_Target3.");
            targets |= 1u << output.SemanticIndex;
        }
        if (targets != 15)
            return reject("G-buffer pixel shaders must write float4 SV_Target0 through SV_Target3.");
    }
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding))) return reject("Cannot inspect bindings.");
        if (binding.Type != D3D_SIT_CBUFFER || binding.BindPoint != 2) continue;
        auto* buffer = reflection->GetConstantBufferByName(binding.Name);
        D3D11_SHADER_BUFFER_DESC bufferDesc{};
        auto* variable = buffer->GetVariableByName("Material");
        D3D11_SHADER_VARIABLE_DESC variableDesc{};
        D3D11_SHADER_TYPE_DESC typeDesc{};
        auto* type = variable->GetType();
        if (std::strcmp(binding.Name, "EngineMaterial") != 0 ||
            FAILED(buffer->GetDesc(&bufferDesc)) || bufferDesc.Size != 32 || bufferDesc.Variables != 1 ||
            FAILED(variable->GetDesc(&variableDesc)) || variableDesc.StartOffset != 0 ||
            FAILED(type->GetDesc(&typeDesc)) || typeDesc.Class != D3D_SVC_STRUCT || typeDesc.Members != 4)
            return reject("EngineMaterial at b2 must match the 32-byte Common.hlsl layout.");
        constexpr const char* names[] = {"specularColor", "shininess", "emission", "lightingWeight"};
        constexpr UINT offsets[] = {0, 12, 16, 28};
        constexpr UINT columns[] = {3, 1, 3, 1};
        for (UINT member = 0; member < 4; ++member) {
            D3D11_SHADER_TYPE_DESC field{};
            if (FAILED(type->GetMemberTypeByIndex(member)->GetDesc(&field)) ||
                std::strcmp(type->GetMemberTypeName(member), names[member]) != 0 ||
                field.Offset != offsets[member] || field.Type != D3D_SVT_FLOAT ||
                field.Rows != 1 || field.Columns != columns[member] || field.Elements != 0)
                return reject("EngineMaterial members must match Common.hlsl offsets and types.");
        }
    }
    return true;
}
} // namespace

void RenderingResourceManager::Initialize(ID3D11Device* device) {
    rendering_failure::RequireDevice(device, "InitializeResourceManager");
    device_ = device;
}

void RenderingResourceManager::Shutdown() noexcept {
    materialInstances_.Clear();
    materials_.Clear();
    meshes_.Clear();
    shaders_.Clear();
    buffers_.Clear();
    textures_.Clear();
    inputLayoutCache_.Clear();
    device_.Reset();
}

TextureHandle RenderingResourceManager::CreateTexture(
    const TextureDesc& desc, const std::span<const D3D11_SUBRESOURCE_DATA> data) {
    rendering_failure::RequireDevice(device_.Get(), "CreateTexture2D");
    if (!data.empty() && (desc.mipLevels == 0 ||
        data.size() != static_cast<std::size_t>(desc.mipLevels) * desc.arraySize))
        rendering_failure::Check(E_INVALIDARG, "CreateTexture2D", device_.Get(), "Initial subresource count does not match mip levels and array size.");
    const D3D11_TEXTURE2D_DESC dxDesc{
        .Width = desc.width,
        .Height = desc.height,
        .MipLevels = desc.mipLevels,
        .ArraySize = desc.arraySize,
        .Format = desc.format,
        .SampleDesc = {
            .Count = desc.sampleCount,
            .Quality = desc.sampleQuality
        },
        .Usage = desc.memoryUsage,
        .BindFlags = desc.GetBindFlags(),
        .CPUAccessFlags = desc.cpuAccessFlags,
        .MiscFlags = desc.miscFlags
    };

    Texture texture;
    texture.desc_ = desc;

    const auto res = device_->CreateTexture2D(
        &dxDesc,
        data.empty() ? nullptr : data.data(),
        &texture.resource_);
    const auto checkTexture = [&](HRESULT hr, const char* operation) {
        rendering_failure::Check(hr, operation, device_.Get(), std::format(
            "Texture {}x{}, mips={}, array={}, format={}, samples={}, bindFlags=0x{:X}; view formats SRV={}, RTV={}, DSV={}, UAV={}",
            desc.width, desc.height, desc.mipLevels, desc.arraySize, static_cast<unsigned>(desc.format),
            desc.sampleCount, desc.GetBindFlags(), static_cast<unsigned>(desc.srvFormat),
            static_cast<unsigned>(desc.rtvFormat), static_cast<unsigned>(desc.dsvFormat), static_cast<unsigned>(desc.uavFormat)));
    };
    checkTexture(res, "CreateTexture2D");

    if (HasFlag(desc.usage, TextureUsage::ShaderResource)) {
        checkTexture(texture.CreateDefaultSRV(device_.Get()), "CreateShaderResourceView");
    }
    if (HasFlag(desc.usage, TextureUsage::RenderTarget)) {
        checkTexture(texture.CreateDefaultRTV(device_.Get()), "CreateRenderTargetView");
    }
    if (HasFlag(desc.usage, TextureUsage::DepthStencil)) {
        checkTexture(texture.CreateDefaultDSV(device_.Get()), "CreateDepthStencilViews");
    }
    if (HasFlag(desc.usage, TextureUsage::UnorderedAccess)) {
        checkTexture(texture.CreateDefaultUAV(device_.Get()), "CreateUnorderedAccessView");
    }

    return textures_.Add(std::move(texture));
}

TextureHandle RenderingResourceManager::RegisterTexture(Texture&& texture) {
    if (!texture.resource_ && !texture.srv_ && !texture.rtv_ && !texture.dsv_ && !texture.uav_)
        rendering_failure::Check(E_INVALIDARG, "RegisterTexture", device_.Get(), "Texture has no resource or usable view.");
    return textures_.Add(std::move(texture));
}

bool RenderingResourceManager::DestroyTexture(const TextureHandle texture) {
    return textures_.Remove(texture);
}

MeshHandle RenderingResourceManager::CreateMesh(const BufferHandle vertexBuffer, const BufferHandle indexBuffer,
                                                VertexLayout&& layout, const uint32_t vertexCount) {
    auto layoutHandle = inputLayoutCache_.RegisterMeshLayout(std::move(layout));
    RenderedMesh mesh{vertexBuffer, indexBuffer, std::move(layoutHandle), vertexCount};
    return meshes_.Add(std::move(mesh));
}

bool RenderingResourceManager::DestroyMesh(const MeshHandle meshHandle, const bool destroyBuffers) {
    const auto* mesh = meshes_.TryGet(meshHandle);
    if (!mesh) {
        return false;
    }
    const BufferHandle vertexBuffer = mesh->vertexBuffer;
    const BufferHandle indexBuffer = mesh->indexBuffer;
    meshes_.Remove(meshHandle);
    if (destroyBuffers) {
        DestroyBuffer(vertexBuffer);
        if (indexBuffer != vertexBuffer) {
            DestroyBuffer(indexBuffer);
        }
    }
    return true;
}

ShaderHandle RenderingResourceManager::CompileShader(const std::wstring& fileName, const ShaderType shaderType,
                                                      const ShaderCompileOptions& options) {
    AssetDesc desc;
    desc.path = AssetPathResolver::DefaultPath(ToUtf8(fileName), AssetKind::Shader);
    auto shader = ShaderContext::CompileShader(device_.Get(), inputLayoutCache_,
                                                AssetPathResolver::ResolveShader(desc), shaderType, options);
    return shaders_.Add(std::move(shader));
}

ShaderHandle RenderingResourceManager::CompileShader(const EngineShader shader) {
    const auto& variant = GetEngineShader(shader);
    return CompileShader(variant.file.wstring(), variant.stage, variant.options);
}

ShaderHandle RenderingResourceManager::RegisterCompiledShader(const CompiledShader& compiled) {
    auto shader = ShaderContext::FinalizeOnDevice(compiled, device_.Get(), inputLayoutCache_);
    return shaders_.Add(std::move(shader));
}

bool RenderingResourceManager::DestroyShader(const ShaderHandle shaderHandle) {
    // Material-owned shaders are released by DestroyMaterial.
    bool owned = false;
    materials_.ForEach([&](const Material& material) {
        owned |= material.shaders.shadowPassVS == shaderHandle ||
                 material.shaders.gBufferPassVS == shaderHandle || material.shaders.gBufferPassPS == shaderHandle ||
                 material.shaders.skinnedGBufferPassVS == shaderHandle || material.shaders.skinnedShadowPassVS == shaderHandle;
    });
    if (owned) return false;
    return shaders_.Remove(shaderHandle);
}

MaterialHandle RenderingResourceManager::CreateMaterial(const MaterialDesc& desc) {
    const auto* texture = textures_.TryGet(desc.defaults.albedoTexture);
    constexpr uint8_t knownHooks = static_cast<uint8_t>(MaterialHook::GBufferVS) |
        static_cast<uint8_t>(MaterialHook::GBufferPS) | static_cast<uint8_t>(MaterialHook::ShadowVS);
    if ((static_cast<uint8_t>(desc.hooks) & ~knownHooks) ||
        (desc.hooks != MaterialHook::None && desc.shaderFile.empty()) ||
        !ValidSurface(desc.defaults.parameters) ||
        !ValidMaterialTexture(*this, desc.defaults.emissionTexture) ||
        (desc.defaults.albedoTexture && (!texture || !IsMaterialTexture(texture->srv_.Get()))) ||
        (desc.defaults.customBuffer && (!buffers_.Contains(desc.defaults.customBuffer) ||
            buffers_.Get(desc.defaults.customBuffer).type != BufferType::Constant))) return {};

    ShaderSet shaders{};
    auto compile = [&](MaterialHook hook, ShaderType type, const char* define, ShaderHandle& target, bool skinned = false) {
        if (!HasHook(desc.hooks, hook)) return;
        const auto path = AssetPathResolver::ResolveShader({
            .path = AssetPathResolver::DefaultPath(ToUtf8(desc.shaderFile), AssetKind::Shader)});
        ShaderCompileOptions options{.defines = {{define, "1"}, {"MATERIAL_HAS_HOOK", "1"}}};
        if (skinned) options.defines.push_back({"ENGINE_SKINNED", "1"});
        auto compiled = ShaderContext::CompileCpu(path, type, options);
        target = RegisterCompiledShader(compiled);
    };
    const auto rollback = [&] {
        shaders_.Remove(shaders.shadowPassVS);
        shaders_.Remove(shaders.gBufferPassVS);
        shaders_.Remove(shaders.gBufferPassPS);
        shaders_.Remove(shaders.skinnedGBufferPassVS);
        shaders_.Remove(shaders.skinnedShadowPassVS);
    };
    try {
        compile(MaterialHook::ShadowVS, ShaderType::Vertex, "MATERIAL_SHADOW_VS", shaders.shadowPassVS);
        compile(MaterialHook::GBufferVS, ShaderType::Vertex, "MATERIAL_GBUFFER_VS", shaders.gBufferPassVS);
        compile(MaterialHook::GBufferPS, ShaderType::Pixel, "MATERIAL_GBUFFER_PS", shaders.gBufferPassPS);
        compile(MaterialHook::GBufferVS, ShaderType::Vertex, "MATERIAL_GBUFFER_VS", shaders.skinnedGBufferPassVS, true);
        compile(MaterialHook::ShadowVS, ShaderType::Vertex, "MATERIAL_SHADOW_VS", shaders.skinnedShadowPassVS, true);
        const auto result = RegisterMaterial(shaders, desc.defaults);
        if (!result) rollback();
        return result;
    } catch (...) {
        rollback();
        throw;
    }
}

MaterialHandle RenderingResourceManager::RegisterMaterial(const ShaderSet& shaders, const MaterialParameters& defaults) {
    const auto* texture = textures_.TryGet(defaults.albedoTexture);
    const auto validShader = [&](ShaderHandle handle, ShaderType type) {
        const auto* shader = shaders_.TryGet(handle);
        return !handle || (shader && shader->type == type);
    };
    if (!validShader(shaders.shadowPassVS, ShaderType::Vertex) ||
        !validShader(shaders.gBufferPassVS, ShaderType::Vertex) ||
        !validShader(shaders.gBufferPassPS, ShaderType::Pixel) ||
        !validShader(shaders.skinnedGBufferPassVS, ShaderType::Vertex) ||
        !validShader(shaders.skinnedShadowPassVS, ShaderType::Vertex) ||
        !ValidSurface(defaults.parameters) ||
        !ValidMaterialTexture(*this, defaults.emissionTexture) ||
        (defaults.albedoTexture && (!texture || !IsMaterialTexture(texture->srv_.Get()))) ||
        (defaults.customBuffer && (!buffers_.Contains(defaults.customBuffer) ||
                                  buffers_.Get(defaults.customBuffer).type != BufferType::Constant))) return {};
    bool shared = false;
    materials_.ForEach([&](const Material& material) {
        const auto& owned = material.shaders;
        for (auto candidate : {shaders.shadowPassVS, shaders.gBufferPassVS, shaders.gBufferPassPS,
                               shaders.skinnedGBufferPassVS, shaders.skinnedShadowPassVS}) {
            if (candidate && (candidate == owned.shadowPassVS || candidate == owned.gBufferPassVS ||
                               candidate == owned.gBufferPassPS || candidate == owned.skinnedGBufferPassVS ||
                               candidate == owned.skinnedShadowPassVS)) shared = true;
        }
    });
    if (shared) return {};
    for (auto handle : {shaders.shadowPassVS, shaders.gBufferPassVS, shaders.gBufferPassPS,
                        shaders.skinnedGBufferPassVS, shaders.skinnedShadowPassVS}) {
        if (handle && !ValidateMaterialShader(shaders_.Get(handle), handle == shaders.gBufferPassPS)) return {};
    }
    return materials_.Add(Material{.shaders = shaders, .defaultParameters = defaults});
}

bool RenderingResourceManager::DestroyMaterial(const MaterialHandle handle) {
    const auto* material = materials_.TryGet(handle);
    if (!material || material->instanceCount != 0) return false;
    const auto shaders = material->shaders;
    materials_.Remove(handle);
    shaders_.Remove(shaders.shadowPassVS);
    shaders_.Remove(shaders.gBufferPassVS);
    shaders_.Remove(shaders.gBufferPassPS);
    shaders_.Remove(shaders.skinnedGBufferPassVS);
    shaders_.Remove(shaders.skinnedShadowPassVS);
    return true;
}

MaterialInstanceHandle RenderingResourceManager::CreateMaterialInstance(const MaterialHandle handle) {
    auto* material = materials_.TryGet(handle);
    if (!material) return {};
    const auto instance = materialInstances_.Add(MaterialInstance{.material = handle});
    ++material->instanceCount;
    return instance;
}

bool RenderingResourceManager::SetMaterialInstanceOverrides(const MaterialInstanceHandle handle,
                                                              const MaterialInstance::Overrides& overrides) {
    auto* instance = materialInstances_.TryGet(handle);
    const auto* texture = textures_.TryGet(overrides.albedoTexture.value_or(TextureHandle{}));
    if (!instance || !ValidSurface(overrides) ||
        !ValidMaterialTexture(*this, overrides.emissionTexture.value_or(TextureHandle{})) ||
        (overrides.albedoTexture && *overrides.albedoTexture &&
        (!texture || !IsMaterialTexture(texture->srv_.Get())))) return false;
    instance->overrides = overrides;
    return true;
}

bool RenderingResourceManager::SetMaterialInstanceCustomBuffer(const MaterialInstanceHandle handle,
                                                                 const BufferHandle buffer) {
    auto* instance = materialInstances_.TryGet(handle);
    if (!instance || (buffer && (!buffers_.Contains(buffer) || buffers_.Get(buffer).type != BufferType::Constant)))
        return false;
    instance->customBuffer = buffer;
    return true;
}

bool RenderingResourceManager::SetMaterialInstanceParameters(const MaterialInstanceHandle handle,
                                                               const MaterialParameters& parameters) {
    auto* instance = materialInstances_.TryGet(handle);
    const auto* texture = textures_.TryGet(parameters.albedoTexture);
    if (!instance || !ValidSurface(parameters.parameters) ||
        !ValidMaterialTexture(*this, parameters.emissionTexture) ||
        (parameters.albedoTexture && (!texture || !IsMaterialTexture(texture->srv_.Get()))) ||
        (parameters.customBuffer && (!buffers_.Contains(parameters.customBuffer) ||
            buffers_.Get(parameters.customBuffer).type != BufferType::Constant))) return false;
    instance->overrides = {.specularColor = parameters.parameters.specularColor,
        .shininess = parameters.parameters.shininess, .albedoTexture = parameters.albedoTexture,
        .emission = parameters.parameters.emission, .lightingWeight = parameters.parameters.lightingWeight,
        .emissionTexture = parameters.emissionTexture};
    instance->customBuffer = parameters.customBuffer;
    return true;
}

bool RenderingResourceManager::ReplaceMaterial(const MaterialHandle live, const MaterialHandle staged) {
    auto* current = materials_.TryGet(live);
    auto* replacement = materials_.TryGet(staged);
    if (!current || !replacement || live == staged || replacement->instanceCount != 0) return false;
    if (!ValidSurface(replacement->defaultParameters.parameters) ||
        !ValidMaterialTexture(*this, replacement->defaultParameters.emissionTexture)) return false;
    const auto* texture = textures_.TryGet(replacement->defaultParameters.albedoTexture);
    if (replacement->defaultParameters.albedoTexture &&
        (!texture || !IsMaterialTexture(texture->srv_.Get()))) return false;
    const auto oldShaders = current->shaders;
    current->shaders = replacement->shaders;
    current->defaultParameters = replacement->defaultParameters;
    materials_.Remove(staged);
    shaders_.Remove(oldShaders.shadowPassVS);
    shaders_.Remove(oldShaders.gBufferPassVS);
    shaders_.Remove(oldShaders.gBufferPassPS);
    shaders_.Remove(oldShaders.skinnedGBufferPassVS);
    shaders_.Remove(oldShaders.skinnedShadowPassVS);
    return true;
}

bool RenderingResourceManager::UpdateMaterialInstance(const MaterialInstanceHandle handle, const MaterialHandle material,
                                                       const MaterialInstance::Overrides& overrides) {
    auto* instance = materialInstances_.TryGet(handle);
    const auto* texture = textures_.TryGet(overrides.albedoTexture.value_or(TextureHandle{}));
    if (!instance || !materials_.Contains(material) || !ValidSurface(overrides) ||
        !ValidMaterialTexture(*this, overrides.emissionTexture.value_or(TextureHandle{})) ||
        (overrides.albedoTexture && *overrides.albedoTexture &&
            (!texture || !IsMaterialTexture(texture->srv_.Get())))) return false;
    if (instance->material != material) {
        --materials_.Get(instance->material).instanceCount;
        ++materials_.Get(material).instanceCount;
        instance->material = material;
    }
    instance->overrides = overrides;
    return true;
}

bool RenderingResourceManager::DestroyMaterialInstance(const MaterialInstanceHandle handle) {
    auto* instance = materialInstances_.TryGet(handle);
    if (!instance) return false;
    --materials_.Get(instance->material).instanceCount;
    return materialInstances_.Remove(handle);
}

const Texture* RenderingResourceManager::TryGetTexture(const TextureHandle handle) const {
    return textures_.TryGet(handle);
}

const Material* RenderingResourceManager::TryGetMaterial(const MaterialHandle handle) const {
    return materials_.TryGet(handle);
}

const MaterialInstance* RenderingResourceManager::TryGetMaterialInstance(const MaterialInstanceHandle handle) const {
    return materialInstances_.TryGet(handle);
}

BufferHandle RenderingResourceManager::CreateBuffer(const BufferDesc& desc, const void* initialData) {
    rendering_failure::RequireDevice(device_.Get(), "CreateBuffer");
    if (desc.size == 0 || (desc.type == BufferType::Constant && desc.size > UINT_MAX - 15u))
        rendering_failure::Check(E_INVALIDARG, "CreateBuffer", device_.Get(), "Invalid buffer byte size: " + std::to_string(desc.size));
    D3D11_BUFFER_DESC dxDesc{};
    dxDesc.ByteWidth = desc.size;

    switch (desc.type) {
    case BufferType::Vertex:
        dxDesc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        break;
    case BufferType::Index:
        dxDesc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
        break;
    case BufferType::Constant:
        dxDesc.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
        dxDesc.ByteWidth = (desc.size + 15) & ~15u;
        break;
    case BufferType::Structured:
        dxDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        dxDesc.StructureByteStride = desc.stride;
        break;
    }

    if (desc.shaderResource) dxDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    switch (desc.usage) {
    case BufferUsage::Immutable:
        dxDesc.Usage = D3D11_USAGE_IMMUTABLE;
        break;
    case BufferUsage::Default:
        dxDesc.Usage = D3D11_USAGE_DEFAULT;
        break;
    case BufferUsage::Dynamic:
        dxDesc.Usage = D3D11_USAGE_DYNAMIC;
        dxDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        break;
    }

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initialData;

    GpuBuffer result;
    result.type = desc.type;
    result.size = dxDesc.ByteWidth;
    result.stride = desc.stride;

    const auto res = device_->CreateBuffer(
        &dxDesc,
        initialData ? &data : nullptr,
        &result.buffer);

    rendering_failure::Check(res, "CreateBuffer", device_.Get(), std::format(
        "Buffer size={}, allocated size={}, stride={}, type={}, usage={}, bindFlags=0x{:X}",
        desc.size, dxDesc.ByteWidth, desc.stride, static_cast<unsigned>(desc.type),
        static_cast<unsigned>(desc.usage), dxDesc.BindFlags));
    return buffers_.Add(std::move(result));
}

bool RenderingResourceManager::DestroyBuffer(const BufferHandle bufferHandle) {
    return buffers_.Remove(bufferHandle);
}

}
