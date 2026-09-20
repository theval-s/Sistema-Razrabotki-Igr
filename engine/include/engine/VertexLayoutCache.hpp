#pragma once
#include <d3d11.h>
#include <spdlog/spdlog.h>
#include <wrl/client.h>

#include "HandleMap.hpp"
#include "VertexLayout.hpp"

namespace engine {

/**
 * Input layout is unique for each mesh-shader combination (for each distinct mesh and shader vertex layouts)
 * This class handles vertex layout signatures for meshes and shaders and also creates and caches DX11 InputLayouts
 */
struct InputLayoutCache {
    HandleMap<VertexLayout> meshVertexLayouts{};
    HandleMap<ShaderVertexLayout> shaderVertexLayouts{};

    std::unordered_map<std::pair<MeshLayoutHandle, ShaderLayoutHandle>, ComPtr<ID3D11InputLayout>> inputLayoutCache{};

    MeshLayoutHandle RegisterMeshLayout(VertexLayout&& vertexLayout) {
        if (const auto result = meshVertexLayouts.Find(vertexLayout)) {
            return result.value();
        }
        return meshVertexLayouts.Add(std::move(vertexLayout));
    }

    ShaderLayoutHandle RegisterShaderLayout(ShaderVertexLayout&& vertexLayout) {
        if (const auto result = shaderVertexLayouts.Find(vertexLayout)) {
            return result.value();
        }
        return shaderVertexLayouts.Add(std::move(vertexLayout));
    }

    /**
     * Find or creates Input layout for given layout handles
     * @param device DX11 device
     * @param meshLayout mesh layout, UB if invalid
     * @param shaderLayout shader layout, UB if invalid
     * @return ComPtr to created input layout, can be if layout creation failed
     */
    ID3D11InputLayout* GetInputLayout(ID3D11Device* device, MeshLayoutHandle meshLayout, ShaderLayoutHandle shaderLayout) {
        assert(meshLayout);
        assert(shaderLayout);
        if (const auto result = inputLayoutCache.find(std::make_pair(meshLayout, shaderLayout)); result != inputLayoutCache.
            end()) {
            return result->second.Get();
        }
        auto layout = CreateInputLayout(device, meshVertexLayouts.Get(meshLayout), shaderVertexLayouts.Get(shaderLayout));
        inputLayoutCache.insert({std::make_pair(meshLayout, shaderLayout), layout});
        return layout.Get();
    }

private:
    [[nodiscard]] static ComPtr<ID3D11InputLayout> CreateInputLayout(ID3D11Device* device, const VertexLayout& meshLayout,
                                                                     const ShaderVertexLayout& shaderLayout) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> inputDescriptions{};
        for (auto& att : shaderLayout.attributes) {
            const auto* meshAttribute = meshLayout.FindAttribute(att.semantic, att.semanticIndex);
            if (meshAttribute == nullptr) {
                spdlog::error("Failed to create input layout! Mesh vertex is missing attribute {} {}!", att.semantic,
                              att.semanticIndex);
                return {};
            }
            inputDescriptions.push_back(D3D11_INPUT_ELEMENT_DESC{
                .SemanticName = att.semantic.c_str(),
                .SemanticIndex = att.semanticIndex,
                .Format = meshAttribute->format,
                .InputSlot = 0, //todo: these should also come from VertexAtts
                .AlignedByteOffset = meshAttribute->offset,
                .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
                .InstanceDataStepRate = 0
            });
        }
        ComPtr<ID3D11InputLayout> result;
        const auto success = device->CreateInputLayout(
            inputDescriptions.data(),
            static_cast<uint32_t>(inputDescriptions.size()),
            shaderLayout.vertexShaderBlob->GetBufferPointer(),
            shaderLayout.vertexShaderBlob->GetBufferSize(),
            &result);
        if (FAILED(success)) {
            //We kindly expect those upper on the callstack to print some more helpful info
            spdlog::error("Failed to create input layout!");
        }
        return result;
    }


};

}
