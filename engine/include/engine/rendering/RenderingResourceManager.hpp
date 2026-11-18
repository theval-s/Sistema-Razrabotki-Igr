#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <engine/rendering/EngineShaders.hpp>
#include <engine/engine_export.hpp>

#include <engine/rendering/resources/Buffer.hpp>
#include <engine/rendering/resources/Material.hpp>
#include <engine/core/HandleMap.hpp>
#include <engine/rendering/resources/RenderedMesh.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include <engine/rendering/resources/Texture.hpp>
#include <engine/rendering/resources/VertexLayoutCache.hpp>

namespace engine {
struct RenderingContext;

struct ENGINE_API RenderingResourceManager {
    /// GPU creation/compilation returns usable handles or throws RenderingException.
    /// Invalid material/reference validation retains its existing empty/false contract.
    /// Destroy operations are safe for empty/stale handles and do not throw.
    void Initialize(ID3D11Device* device);
    /// Releases all owned GPU objects/layouts and the device; invalidates all handles.
    /// Call only after consumers and bindings have been detached.
    void Shutdown() noexcept;
    TextureHandle CreateTexture(const TextureDesc& desc,
                                std::span<const D3D11_SUBRESOURCE_DATA> data = {});
    TextureHandle RegisterTexture(Texture&& texture);
    bool DestroyTexture(TextureHandle texture);
    /// Main-thread, generation-checked lookup; null for empty or stale handles.
    /// Borrowed pointer: do not retain across texture registration/destruction or shutdown.
    /// Inspect GetSRV() for imported resource shape; Texture::desc may be default-initialized.
    [[nodiscard]] const Texture* TryGetTexture(TextureHandle texture) const;
    MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer, VertexLayout&& layout,
                          uint32_t vertexCount);
    /// Meshes own their vertex and index buffers by default. Pass false if the buffers are shared.
    bool DestroyMesh(MeshHandle meshHandle, bool destroyBuffers = true);
    /// File names use executable-relative engine/... or game/... shader roots.
    ShaderHandle CompileShader(const std::wstring& fileName, ShaderType shaderType,
                               const ShaderCompileOptions& options = {});
    ShaderHandle CompileShader(EngineShader shader);
    /// Uploads a CPU-compiled shader to the device and registers it; main thread only.
    /// Throws RenderingException with the compiled shader's source/variant on failure.
    ShaderHandle RegisterCompiledShader(const CompiledShader& compiled);
    bool DestroyShader(ShaderHandle shaderHandle);
    /// Compiles declared hooks. Empty for invalid descriptors; compilation/device failures
    /// throw RenderingException after releasing every partially acquired shader.
    MaterialHandle CreateMaterial(const MaterialDesc& desc);
    /// Main-thread upload path for the asset importer: no file I/O or compilation.
    /// Takes ownership of the shader handles on success; they must not be shared.
    MaterialHandle RegisterMaterial(const ShaderSet& shaders, const MaterialParameters& defaults);
    bool DestroyMaterial(MaterialHandle material);
    /// Consumes a prepared material, retaining the live handle and all its instances.
    bool ReplaceMaterial(MaterialHandle live, MaterialHandle staged);
    [[nodiscard]] MaterialInstanceHandle CreateMaterialInstance(MaterialHandle material);
    /// Advanced escape hatch: fixes every supplied PBR/texture value as an explicit override.
    /// Those fields will not follow subsequent material-default changes.
    bool SetMaterialInstanceParameters(MaterialInstanceHandle instance, const MaterialParameters& parameters);
    /// Changes an authored instance without replacing its handle (scene renderers retain that handle).
    bool UpdateMaterialInstance(MaterialInstanceHandle instance, MaterialHandle material,
                                const MaterialInstance::Overrides& overrides);
    bool SetMaterialInstanceOverrides(MaterialInstanceHandle instance, const MaterialInstance::Overrides& overrides);
    bool SetMaterialInstanceCustomBuffer(MaterialInstanceHandle instance, BufferHandle buffer);
    bool DestroyMaterialInstance(MaterialInstanceHandle instance);
    [[nodiscard]] const Material* TryGetMaterial(MaterialHandle material) const;
    [[nodiscard]] const MaterialInstance* TryGetMaterialInstance(MaterialInstanceHandle instance) const;
    
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
    
    bool DestroyBuffer(BufferHandle bufferHandle);

    friend RenderingContext;
    friend class ParticlePass;
    friend ShaderContext;

private:
    HandleMap<Texture> textures_{};
    HandleMap<GpuBuffer> buffers_{};
    HandleMap<ShaderContext> shaders_{};
    HandleMap<RenderedMesh> meshes_{};
    HandleMap<Material> materials_{};
    HandleMap<MaterialInstance> materialInstances_{};
    ComPtr<ID3D11Device> device_{};
    InputLayoutCache inputLayoutCache_;
};
}
