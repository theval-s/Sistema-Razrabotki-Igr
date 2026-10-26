#include <engine/rendering/RenderingResourceManager.hpp>

#include <cstdlib>
#include <utility>
#include <spdlog/spdlog.h>

#include <engine/assets/AssetPathResolver.hpp>

namespace engine {
namespace {
std::string Utf8Name(const std::wstring& name) {
    const auto utf8 = std::filesystem::path(name).u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
} // namespace

void RenderingResourceManager::Initialize(ID3D11Device* device) {
    device_ = device;
}

TextureHandle RenderingResourceManager::CreateTexture(
    const TextureDesc& desc, const std::span<const D3D11_SUBRESOURCE_DATA> data) {
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
    checkResult(res, "CreateTexture2D");

    if (HasFlag(desc.usage, TextureUsage::ShaderResource)) {
        checkResult(texture.CreateDefaultSRV(device_.Get()), "texture.CreateDefaultSRV");
    }
    if (HasFlag(desc.usage, TextureUsage::RenderTarget)) {
        checkResult(texture.CreateDefaultRTV(device_.Get()), "texture.CreateDefaultRTV");
    }
    if (HasFlag(desc.usage, TextureUsage::DepthStencil)) {
        checkResult(texture.CreateDefaultDSV(device_.Get()), "texture.CreateDefaultDSV");
    }
    if (HasFlag(desc.usage, TextureUsage::UnorderedAccess)) {
        checkResult(texture.CreateDefaultUAV(device_.Get()), "texture.CreateDefaultUAV");
    }

    return textures_.Add(std::move(texture));
}

TextureHandle RenderingResourceManager::RegisterTexture(Texture&& texture) {
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
    desc.path = AssetPathResolver::DefaultPath(Utf8Name(fileName), AssetKind::Shader);
    auto shader = ShaderContext::CompileShader(device_.Get(), inputLayoutCache_,
                                                AssetPathResolver::ResolveShader(desc), shaderType, options);
    if (!shader) {
        spdlog::critical("Failed to compile shader");
        std::exit(1);
    }
    return shaders_.Add(std::move(*shader));
}

ShaderHandle RenderingResourceManager::RegisterCompiledShader(const CompiledShader& compiled) {
    auto shader = ShaderContext::FinalizeOnDevice(compiled, device_.Get(), inputLayoutCache_);
    if (!shader) {
        return ShaderHandle{};
    }
    return shaders_.Add(std::move(*shader));
}

bool RenderingResourceManager::DestroyShader(const ShaderHandle shaderHandle) {
    // Material-owned shaders are released by DestroyMaterial.
    for (const auto& [id, instanceHandle] : defaultInstances_) {
        const auto* instance = materialInstances_.TryGet(instanceHandle);
        if (!instance) continue;
        const auto* material = materials_.TryGet(instance->material);
        if (material && (material->shaders.shadowPassVS == shaderHandle ||
                         material->shaders.gBufferPassVS == shaderHandle ||
                         material->shaders.gBufferPassPS == shaderHandle)) return false;
    }
    return shaders_.Remove(shaderHandle);
}

MaterialHandle RenderingResourceManager::CreateMaterial(const MaterialDesc& desc) {
    constexpr uint8_t knownHooks = static_cast<uint8_t>(MaterialHook::GBufferVS) |
        static_cast<uint8_t>(MaterialHook::GBufferPS) | static_cast<uint8_t>(MaterialHook::ShadowVS);
    if ((static_cast<uint8_t>(desc.hooks) & ~knownHooks) ||
        (desc.hooks != MaterialHook::None && desc.shaderFile.empty()) ||
        (desc.defaults.albedoTexture && !textures_.Contains(desc.defaults.albedoTexture)) ||
        (desc.defaults.customBuffer && (!buffers_.Contains(desc.defaults.customBuffer) ||
            buffers_.Get(desc.defaults.customBuffer).type != BufferType::Constant))) return {};

    ShaderSet shaders{};
    auto compile = [&](MaterialHook hook, ShaderType type, const char* define, ShaderHandle& target) {
        if (!HasHook(desc.hooks, hook)) return true;
        const auto path = AssetPathResolver::ResolveShader({
            .path = AssetPathResolver::DefaultPath(Utf8Name(desc.shaderFile), AssetKind::Shader)});
        auto compiled = ShaderContext::CompileCpu(path, type, {.defines = {{define, "1"}, {"MATERIAL_HAS_HOOK", "1"}}});
        if (!compiled) return false;
        target = RegisterCompiledShader(*compiled);
        return static_cast<bool>(target);
    };
    if (!compile(MaterialHook::ShadowVS, ShaderType::Vertex, "MATERIAL_SHADOW_VS", shaders.shadowPassVS) ||
        !compile(MaterialHook::GBufferVS, ShaderType::Vertex, "MATERIAL_GBUFFER_VS", shaders.gBufferPassVS) ||
        !compile(MaterialHook::GBufferPS, ShaderType::Pixel, "MATERIAL_GBUFFER_PS", shaders.gBufferPassPS)) {
        shaders_.Remove(shaders.shadowPassVS);
        shaders_.Remove(shaders.gBufferPassVS);
        shaders_.Remove(shaders.gBufferPassPS);
        return {};
    }
    const auto result = RegisterMaterial(shaders, desc.defaults);
    if (!result) {
        shaders_.Remove(shaders.shadowPassVS);
        shaders_.Remove(shaders.gBufferPassVS);
        shaders_.Remove(shaders.gBufferPassPS);
    }
    return result;
}

MaterialHandle RenderingResourceManager::RegisterMaterial(const ShaderSet& shaders, const MaterialParameters& defaults) {
    const auto validShader = [&](ShaderHandle handle, ShaderType type) {
        const auto* shader = shaders_.TryGet(handle);
        return !handle || (shader && shader->type == type);
    };
    if (!validShader(shaders.shadowPassVS, ShaderType::Vertex) ||
        !validShader(shaders.gBufferPassVS, ShaderType::Vertex) ||
        !validShader(shaders.gBufferPassPS, ShaderType::Pixel) ||
        (defaults.albedoTexture && !textures_.Contains(defaults.albedoTexture)) ||
        (defaults.customBuffer && (!buffers_.Contains(defaults.customBuffer) ||
                                  buffers_.Get(defaults.customBuffer).type != BufferType::Constant))) return {};
    for (const auto& [id, instanceHandle] : defaultInstances_) {
        const auto& owned = materials_.Get(materialInstances_.Get(instanceHandle).material).shaders;
        for (auto candidate : {shaders.shadowPassVS, shaders.gBufferPassVS, shaders.gBufferPassPS}) {
            if (candidate && (candidate == owned.shadowPassVS || candidate == owned.gBufferPassVS ||
                              candidate == owned.gBufferPassPS)) return {};
        }
    }
    Material material{.shaders = shaders, .defaultParameters = defaults, .instanceCount = 1};
    const auto handle = materials_.Add(std::move(material));
    const auto instance = materialInstances_.Add(MaterialInstance{handle, defaults});
    defaultInstances_.emplace(handle.id, instance);
    return handle;
}

bool RenderingResourceManager::DestroyMaterial(const MaterialHandle handle) {
    const auto* material = materials_.TryGet(handle);
    if (!material || material->instanceCount != 1) return false;
    const auto shaders = material->shaders;
    materialInstances_.Remove(defaultInstances_.at(handle.id));
    defaultInstances_.erase(handle.id);
    materials_.Remove(handle);
    shaders_.Remove(shaders.shadowPassVS);
    shaders_.Remove(shaders.gBufferPassVS);
    shaders_.Remove(shaders.gBufferPassPS);
    return true;
}

MaterialInstanceHandle RenderingResourceManager::GetDefaultMaterialInstance(const MaterialHandle handle) const {
    if (!materials_.Contains(handle)) return {};
    return defaultInstances_.at(handle.id);
}

MaterialInstanceHandle RenderingResourceManager::CreateMaterialInstance(const MaterialHandle handle) {
    auto* material = materials_.TryGet(handle);
    if (!material) return {};
    ++material->instanceCount;
    return materialInstances_.Add(MaterialInstance{handle, material->defaultParameters});
}

bool RenderingResourceManager::SetMaterialInstanceParameters(const MaterialInstanceHandle handle,
                                                               const MaterialParameters& parameters) {
    auto* instance = materialInstances_.TryGet(handle);
    if (!instance || (parameters.albedoTexture && !textures_.Contains(parameters.albedoTexture)) ||
        (parameters.customBuffer && (!buffers_.Contains(parameters.customBuffer) ||
            buffers_.Get(parameters.customBuffer).type != BufferType::Constant))) return false;
    instance->parameters = parameters;
    return true;
}

bool RenderingResourceManager::DestroyMaterialInstance(const MaterialInstanceHandle handle) {
    auto* instance = materialInstances_.TryGet(handle);
    if (!instance || defaultInstances_.at(instance->material.id) == handle) return false;
    --materials_.Get(instance->material).instanceCount;
    return materialInstances_.Remove(handle);
}

const Material* RenderingResourceManager::TryGetMaterial(const MaterialHandle handle) const {
    return materials_.TryGet(handle);
}

const MaterialInstance* RenderingResourceManager::TryGetMaterialInstance(const MaterialInstanceHandle handle) const {
    return materialInstances_.TryGet(handle);
}

BufferHandle RenderingResourceManager::CreateBuffer(const BufferDesc& desc, const void* initialData) {
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

    checkResult(res, "CreateBuffer");
    return buffers_.Add(std::move(result));
}

bool RenderingResourceManager::DestroyBuffer(const BufferHandle bufferHandle) {
    return buffers_.Remove(bufferHandle);
}

void RenderingResourceManager::checkResult(const HRESULT result, const char* message) {
    if (FAILED(result)) {
        // ass error handling, needs a solution
        spdlog::critical("{} {}", message, result);
        std::exit(1);
    }
}

}
