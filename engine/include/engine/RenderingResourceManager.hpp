#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <engine/engine_export.hpp>

#include "Buffer.hpp"
#include "HandleMap.hpp"
#include "RenderedMesh.h"
#include "ShaderContext.hpp"
#include "Texture.hpp"
#include "VertexLayoutCache.hpp"

namespace engine {
struct RenderingContext;

struct ENGINE_API RenderingResourceManager {
    void Initialize(ID3D11Device* device);
    TextureHandle CreateTexture(const TextureDesc& desc,
                                std::span<const D3D11_SUBRESOURCE_DATA> data = {});
    TextureHandle RegisterTexture(Texture&& texture);
    MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer, VertexLayout&& layout,
                          uint32_t vertexCount);
    ShaderHandle CompileShader(const std::wstring& fileName, ShaderType shaderType);
    [[nodiscard]] BufferHandle CreateBuffer(const BufferDesc& desc, const void* initialData);

    template <typename T>
    [[nodiscard]] BufferHandle CreateVertexBuffer(std::span<const T> vertices,
                                                   const BufferUsage usage = BufferUsage::Immutable) {
        return CreateBuffer(
            {
                .type = BufferType::Vertex,
                .usage = usage,
                .size = static_cast<uint32_t>(vertices.size_bytes()),
                .stride = sizeof(T)
            }, vertices.data());
    }

    template <typename T>
    [[nodiscard]] BufferHandle CreateIndexBuffer(std::span<const T> indices) {
        static_assert(std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t>);

        return CreateBuffer(
            {
                .type = BufferType::Index,
                .usage = BufferUsage::Immutable,
                .size = static_cast<uint32_t>(indices.size_bytes()),
                .stride = sizeof(T)
            }, indices.data());
    }

    template <typename T>
    [[nodiscard]] BufferHandle CreateConstantBuffer() {
        static_assert(std::is_trivially_copyable_v<T>);

        constexpr uint32_t size = (sizeof(T) + 15) & ~15u; // CBuffers size should be n * 16
        return CreateBuffer(
            {
                .type = BufferType::Constant,
                .usage = BufferUsage::Dynamic,
                .size = size
            }, nullptr);
    }

    friend RenderingContext;
    friend ShaderContext;

private:
    static void checkResult(HRESULT result, const char* message);

    HandleMap<Texture> textures_{};
    HandleMap<GpuBuffer> buffers_{};
    HandleMap<ShaderContext> shaders_{};
    HandleMap<RenderedMesh> meshes_{};
    ComPtr<ID3D11Device> device_{};
    InputLayoutCache inputLayoutCache_;
};
}
