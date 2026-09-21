#include "engine/RenderingResourceManager.hpp"

#include <cstdlib>
#include <utility>
#include <spdlog/spdlog.h>

namespace engine {

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

MeshHandle RenderingResourceManager::CreateMesh(const BufferHandle vertexBuffer, const BufferHandle indexBuffer,
                                                VertexLayout&& layout, const uint32_t vertexCount) {
    auto layoutHandle = inputLayoutCache_.RegisterMeshLayout(std::move(layout));
    RenderedMesh mesh{vertexBuffer, indexBuffer, std::move(layoutHandle), vertexCount};
    return meshes_.Add(std::move(mesh));
}

ShaderHandle RenderingResourceManager::CompileShader(const std::wstring& fileName, const ShaderType shaderType) {
    std::wstring path = L"./engine/shaders/";
    path += fileName;
    auto shader = ShaderContext::CompileShader(device_.Get(), inputLayoutCache_, path, shaderType);
    if (!shader) {
        spdlog::critical("Failed to compile shader");
        std::exit(1);
    }
    return shaders_.Add(std::move(*shader));
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
    result.size = dxDesc.ByteWidth;
    result.stride = desc.stride;

    const auto res = device_->CreateBuffer(
        &dxDesc,
        initialData ? &data : nullptr,
        &result.buffer);

    checkResult(res, "CreateBuffer");
    return buffers_.Add(std::move(result));
}

void RenderingResourceManager::checkResult(const HRESULT result, const char* message) {
    if (FAILED(result)) {
        // ass error handling, needs a solution
        spdlog::critical("{} {}", message, result);
        std::exit(1);
    }
}

}
