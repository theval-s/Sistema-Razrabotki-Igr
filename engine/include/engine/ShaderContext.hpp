#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <engine/engine_export.hpp>
#include <wrl/client.h>

#include "ResourceID.hpp"
#include "VertexLayoutCache.hpp"

namespace engine {

using Microsoft::WRL::ComPtr;

enum class ShaderType {
    Vertex,
    Pixel,
    Geometry,
};

ENGINE_API const char* GetEntryPoint(ShaderType type);
ENGINE_API const char* GetDX11Target(ShaderType type);

struct ENGINE_API ShaderBinding {
    std::string name;
    uint8_t slot;
    uint8_t type; // aka D3D_SHADER_INPUT_TYPE, but 8 bits
};

struct ENGINE_API DXShader {
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11GeometryShader> geometryShader;
};

struct ENGINE_API ShaderContext {
    ComPtr<ID3DBlob> blob;
    ShaderType type;
    ShaderLayoutHandle layoutHandle;

    // todo make a proper destructor
    DXShader shader;

    // debug only, later we'll add correctness checks based on names and slots
    std::vector<ShaderBinding> bindings{};

    static std::optional<ShaderContext> CompileShader(ID3D11Device* device, InputLayoutCache& layoutCache,
                                                       const std::wstring& fileName, ShaderType shaderType);

private:
    static ShaderVertexLayout ParseVertexShaderInputs(const ComPtr<ID3D11ShaderReflection>& reflection,
                                                       const D3D11_SHADER_DESC& shaderDesc,
                                                       const ComPtr<ID3DBlob>& blob);
};

using ShaderHandle = HandleMap<ShaderContext>::Handle;
}
