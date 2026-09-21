#include "engine/VertexLayoutCache.hpp"

#include <cassert>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

namespace engine {

bool InputLayoutCache::LayoutKey::operator==(const LayoutKey& other) const {
    return mesh == other.mesh && shader == other.shader;
}

size_t InputLayoutCache::LayoutKeyHash::operator()(const LayoutKey& key) const noexcept {
    return (static_cast<size_t>(key.mesh.id) << 32) ^ key.shader.id;
}

MeshLayoutHandle InputLayoutCache::RegisterMeshLayout(VertexLayout&& vertexLayout) {
    if (const auto result = meshVertexLayouts_.Find(vertexLayout)) {
        return result.value();
    }
    return meshVertexLayouts_.Add(std::move(vertexLayout));
}

ShaderLayoutHandle InputLayoutCache::RegisterShaderLayout(ShaderVertexLayout&& vertexLayout) {
    if (const auto result = shaderVertexLayouts_.Find(vertexLayout)) {
        return result.value();
    }
    return shaderVertexLayouts_.Add(std::move(vertexLayout));
}

ID3D11InputLayout* InputLayoutCache::GetInputLayout(ID3D11Device* device, const MeshLayoutHandle meshLayout,
                                                     const ShaderLayoutHandle shaderLayout) {
    assert(meshLayout);
    assert(shaderLayout);
    const LayoutKey key{meshLayout, shaderLayout};
    if (const auto result = inputLayoutCache_.find(key); result != inputLayoutCache_.end()) {
        return result->second.Get();
    }
    auto layout = CreateInputLayout(device, meshVertexLayouts_.Get(meshLayout),
                                    shaderVertexLayouts_.Get(shaderLayout));
    inputLayoutCache_.insert({key, layout});
    return layout.Get();
}

ComPtr<ID3D11InputLayout> InputLayoutCache::CreateInputLayout(ID3D11Device* device,
                                                               const VertexLayout& meshLayout,
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
            .InputSlot = 0, // todo: these should also come from VertexAtts
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
        // We kindly expect those upper on the callstack to print some more helpful info
        spdlog::error("Failed to create input layout!");
    }
    return result;
}

}
