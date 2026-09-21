#include "engine/VertexLayout.hpp"

#include <algorithm>
#include <utility>

namespace engine {

bool VertexAttribute::operator==(const VertexAttribute&) const = default;

bool VertexLayout::operator==(const VertexLayout&) const = default;

void VertexLayout::Add(const std::string& semantic,
                       const uint32_t semanticIndex,
                       const DXGI_FORMAT format,
                       const uint32_t offset) {
    attributes.emplace_back(semantic, semanticIndex, format, offset);
}

const VertexAttribute* VertexLayout::FindAttribute(const std::string& semantic, const uint32_t semanticIndex) const {
    const auto it = std::ranges::find_if(attributes,
                                         [&](auto& att){
                                             return att.semantic == semantic && att.semanticIndex == semanticIndex;
                                         });
    if (it != attributes.end()) {
        return &*it;
    }
    return nullptr;
}

ShaderVertexLayout::ShaderVertexLayout(ComPtr<ID3DBlob> shaderBlob) : vertexShaderBlob(std::move(shaderBlob)) {
}

bool ShaderVertexAttribute::operator==(const ShaderVertexAttribute&) const = default;

bool ShaderVertexLayout::operator==(const ShaderVertexLayout& other) const {
    return this->attributes == other.attributes;
}

void ShaderVertexLayout::Add(const std::string& semantic,
                             const uint32_t semanticIndex,
                             const D3D_REGISTER_COMPONENT_TYPE type,
                             const uint8_t mask) {
    attributes.emplace_back(semantic, semanticIndex, type, mask);
}

}
