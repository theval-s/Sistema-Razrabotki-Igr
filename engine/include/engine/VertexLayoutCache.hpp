#pragma once
#include <cstddef>
#include <d3d11.h>
#include <engine/engine_export.hpp>
#include <wrl/client.h>
#include <unordered_map>

#include "HandleMap.hpp"
#include "VertexLayout.hpp"

namespace engine {

/**
 * Input layout is unique for each mesh-shader combination (for each distinct mesh and shader vertex layouts)
 * This class handles vertex layout signatures for meshes and shaders and also creates and caches DX11 InputLayouts
 */
struct ENGINE_API InputLayoutCache {
    MeshLayoutHandle RegisterMeshLayout(VertexLayout&& vertexLayout);
    ShaderLayoutHandle RegisterShaderLayout(ShaderVertexLayout&& vertexLayout);

    /**
     * Find or creates Input layout for given layout handles
     * @param device DX11 device
     * @param meshLayout mesh layout, UB if invalid
     * @param shaderLayout shader layout, UB if invalid
     * @return pointer to created input layout, can be null if layout creation failed
     */
    ID3D11InputLayout* GetInputLayout(ID3D11Device* device, MeshLayoutHandle meshLayout,
                                      ShaderLayoutHandle shaderLayout);

private:
    struct LayoutKey {
        MeshLayoutHandle mesh;
        ShaderLayoutHandle shader;

        bool operator==(const LayoutKey& other) const;
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& key) const noexcept;
    };

    [[nodiscard]] static ComPtr<ID3D11InputLayout> CreateInputLayout(ID3D11Device* device,
                                                                      const VertexLayout& meshLayout,
                                                                      const ShaderVertexLayout& shaderLayout);

    HandleMap<VertexLayout> meshVertexLayouts_{};
    HandleMap<ShaderVertexLayout> shaderVertexLayouts_{};
    std::unordered_map<LayoutKey, ComPtr<ID3D11InputLayout>, LayoutKeyHash> inputLayoutCache_{};
};

}
