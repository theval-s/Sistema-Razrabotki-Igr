#pragma once
#include <cstdint>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include <engine/engine_export.hpp>

#include "HandleMap.hpp"

namespace engine {

using Microsoft::WRL::ComPtr;

struct VertexAttribute {
    std::string semantic;
    uint32_t semanticIndex;
    DXGI_FORMAT format;
    uint32_t offset;

    ENGINE_API bool operator==(const VertexAttribute&) const;
};

struct VertexLayout {
    std::vector<VertexAttribute> attributes{};

    ENGINE_API bool operator==(const VertexLayout&) const;

    ENGINE_API void Add(const std::string& semantic,
                        uint32_t semanticIndex,
                        DXGI_FORMAT format,
                        uint32_t offset);

    ENGINE_API const VertexAttribute* FindAttribute(const std::string& semantic, uint32_t semanticIndex) const;
};

using MeshLayoutHandle = HandleMap<VertexLayout>::Handle;

struct ShaderVertexAttribute {
    std::string semantic;
    uint32_t semanticIndex;
    D3D_REGISTER_COMPONENT_TYPE type;
    uint8_t mask;

    ENGINE_API bool operator==(const ShaderVertexAttribute&) const;
};

struct ShaderVertexLayout {
    std::vector<ShaderVertexAttribute> attributes{};
    //We need some shader blob to create DX11 input layouts
    ComPtr<ID3DBlob> vertexShaderBlob;

    ENGINE_API explicit ShaderVertexLayout(ComPtr<ID3DBlob> shaderBlob);
    
    ENGINE_API bool operator==(const ShaderVertexLayout& other) const;
    
    ENGINE_API void Add(const std::string& semantic,
                        uint32_t semanticIndex,
                        D3D_REGISTER_COMPONENT_TYPE type,
                        uint8_t mask);
};

using ShaderLayoutHandle = HandleMap<ShaderVertexLayout>::Handle;
}
