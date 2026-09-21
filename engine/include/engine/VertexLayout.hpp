#pragma once
#include <algorithm>
#include <cstdint>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <string>
#include <utility>
#include <vector>
#include <wrl/client.h>

#include "HandleMap.hpp"

namespace engine {

using Microsoft::WRL::ComPtr;

struct VertexAttribute {
    std::string semantic;
    uint32_t semanticIndex;
    DXGI_FORMAT format;
    uint32_t offset;

    bool operator==(const VertexAttribute&) const = default;
};

struct VertexLayout {
    std::vector<VertexAttribute> attributes{};

    bool operator==(const VertexLayout&) const = default;

    void Add(const std::string& semantic,
             uint32_t semanticIndex,
             DXGI_FORMAT format,
             uint32_t offset) {
        attributes.emplace_back(semantic, semanticIndex, format, offset);
    }

    const VertexAttribute* FindAttribute(const std::string& semantic, uint32_t semanticIndex) const {
        const auto it = std::ranges::find_if(attributes,
                                       [&](auto& att){
                                           return att.semantic == semantic && att.semanticIndex == semanticIndex;
                                       });
        if (it != attributes.end()) {
            return &*it;
        }
        return nullptr;
    }
};

using MeshLayoutHandle = HandleMap<VertexLayout>::Handle;

struct ShaderVertexAttribute {
    std::string semantic;
    uint32_t semanticIndex;
    D3D_REGISTER_COMPONENT_TYPE type;
    uint8_t mask;

    bool operator==(const ShaderVertexAttribute&) const = default;
};

struct ShaderVertexLayout {
    std::vector<ShaderVertexAttribute> attributes{};
    //We need some shader blob to create DX11 input layouts
    ComPtr<ID3DBlob> vertexShaderBlob;

    explicit ShaderVertexLayout(ComPtr<ID3DBlob> shaderBlob) : vertexShaderBlob(std::move(shaderBlob)) {
    }
    
    bool operator==(const ShaderVertexLayout& other) const {
        return this->attributes == other.attributes;
    }
    
    void Add(const std::string& semantic,
           uint32_t semanticIndex,
           D3D_REGISTER_COMPONENT_TYPE type,
           uint8_t mask) {
        attributes.emplace_back(semantic, semanticIndex, type, mask);
    }
};

using ShaderLayoutHandle = HandleMap<ShaderVertexLayout>::Handle;
}
