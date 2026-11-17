#include <engine/rendering/resources/VertexLayoutCache.hpp>

#include <utility>
#include <vector>
#include "../LifecycleFailure.hpp"

namespace engine {

void InputLayoutCache::Clear() noexcept {
    inputLayoutCache_.clear();
    meshVertexLayouts_.Clear();
    shaderVertexLayouts_.Clear();
}

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
    if (!device || !meshVertexLayouts_.Contains(meshLayout) || !shaderVertexLayouts_.Contains(shaderLayout))
        rendering_detail::Check(E_INVALIDARG, "CreateInputLayout", device, "Missing device, mesh layout or vertex shader layout.");
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
            rendering_detail::Check(E_INVALIDARG, "CreateInputLayout", device,
                fmt::format("Mesh vertex is missing shader attribute {} {}.", att.semantic, att.semanticIndex));
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
    if (!shaderLayout.vertexShaderBlob)
        rendering_detail::Check(E_INVALIDARG, "CreateInputLayout", device, "Vertex shader bytecode is absent.");
    const auto success = device->CreateInputLayout(
        inputDescriptions.data(),
        static_cast<uint32_t>(inputDescriptions.size()),
        shaderLayout.vertexShaderBlob->GetBufferPointer(),
        shaderLayout.vertexShaderBlob->GetBufferSize(),
        &result);
    rendering_detail::Check(success, "CreateInputLayout", device,
        fmt::format("Mesh/shader input layout, {} elements", inputDescriptions.size()));
    return result;
}

}
