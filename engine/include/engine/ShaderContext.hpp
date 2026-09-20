#pragma once
#include <string>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <iostream>
#include <ostream>
#include <spdlog/spdlog.h>
#include <wrl/client.h>

#include "RenderingSystem.hpp"
#include "ResourceID.hpp"


namespace engine {

using Microsoft::WRL::ComPtr;

enum class ShaderType {
    Vertex,
    Pixel,
    Geometry,
};

static const char* GetEntryPoint(const ShaderType type) {
    switch (type) {
    case ShaderType::Vertex: return "VSMain";
    case ShaderType::Pixel: return "PSMain";
    case ShaderType::Geometry: return "GSMain";
    }
    return "";
}

static const char* GetDX11Target(const ShaderType type) {
    switch (type) {
    case ShaderType::Vertex: return "vs_5_0";
    case ShaderType::Pixel: return "ps_5_0";
    case ShaderType::Geometry: return "gs_5_0";
    }
    return "";
}

struct ShaderBinding {
    std::string name;
    uint8_t slot;
    uint8_t type; //aka D3D_SHADER_INPUT_TYPE, but 8bits
};

union DXShader {
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11GeometryShader> geometryShader;
};

struct ShaderContext {
    ComPtr<ID3DBlob> blob;
    ShaderType type;
    ShaderLayoutHandle layoutHandle;

    //todo make a proper destructor
    DXShader shader;

    //debug only, later we'll add correctness checks based on names and slots
    std::vector<ShaderBinding> bindings{};


    static ShaderVertexLayout ParseVertexShaderInputs(const ComPtr<ID3D11ShaderReflection>& reflection,
                                                      const D3D11_SHADER_DESC& shaderDesc, const ComPtr<ID3DBlob>& blob) {
        ShaderVertexLayout result{blob};
        for (UINT i = 0; i < shaderDesc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC desc{};
            if (const auto res = reflection->GetInputParameterDesc(i, &desc); FAILED(res)) {
                spdlog::error("Unexpected error cannot get input parameter desc {}", i);
                continue;
            }
            if (desc.SystemValueType != D3D_NAME_UNDEFINED) //Skip as it doesn't affect shader signature
                continue;
            result.Add(desc.SemanticName, desc.SemanticIndex, desc.ComponentType, desc.Mask);
        }
        return result;
    }

    static std::optional<ShaderContext> CompileShader(ID3D11Device* device, InputLayoutCache  &layoutCache, const std::wstring& fileName,
                                                      const ShaderType shaderType) {
        ShaderContext result{};
        ID3DBlob* errorCode = nullptr;
        auto res = D3DCompileFromFile(fileName.c_str(),
                                      nullptr,
                                      nullptr,
                                      GetEntryPoint(shaderType),
                                      GetDX11Target(shaderType),
                                      D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, //todo: should be toggleable
                                      0,
                                      result.blob.GetAddressOf(),
                                      &errorCode);
        if (res < 0) {
            if (errorCode) {
                auto compileErrors = static_cast<char*>(errorCode->GetBufferPointer());
                spdlog::error("Could not compile shader: {}", compileErrors);
            } else {
                spdlog::error("Missing Shader File: {}", fileName);
            }
        }

        ComPtr<ID3D11ShaderReflection> reflection;
        res = D3DReflect(
            result.blob->GetBufferPointer(),
            result.blob->GetBufferSize(),
            IID_ID3D11ShaderReflection,
            reinterpret_cast<void**>(reflection.GetAddressOf()));
        if (FAILED(res)) {
            spdlog::error("Could not get shader reflection for shader: {}", fileName);
            return std::nullopt;
        }

        D3D11_SHADER_DESC shaderDesc{};
        if (const auto r = res = reflection->GetDesc(&shaderDesc); FAILED(r)) {
            spdlog::error("Could not get shader description for shader: {}", fileName);
        }

        for (UINT i = 0; i < shaderDesc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bind{};
            if (const auto r = reflection->GetResourceBindingDesc(i, &bind); FAILED(r)) {
                spdlog::error("Could not get resource binding description for shader: {}", fileName);
                continue;
            }
            if (bind.BindCount != 1) {
                spdlog::error("Array bindings are unsupported: {}", fileName);
                continue;
            }
            result.bindings.emplace_back(bind.Name, static_cast<uint8_t>(bind.BindPoint), static_cast<uint8_t>(bind.Type));
        }


        if (shaderType == ShaderType::Vertex) {
            auto layout = ParseVertexShaderInputs(reflection, shaderDesc, result.blob);
            result.layoutHandle = layoutCache.RegisterShaderLayout(std::move(layout));
        }

        switch (shaderType) {
        case ShaderType::Vertex:
            ComPtr<ID3D11VertexShader> vs;
            if (const auto r = device->CreateVertexShader(
                result.blob->GetBufferPointer(),
                result.blob->GetBufferSize(),
                nullptr, &vs); FAILED(r)) {
                spdlog::error("Could make vertex shader for: {}", fileName);
            }
            result.shader.vertexShader = vs;
            break;
        case ShaderType::Pixel:
            ComPtr<ID3D11PixelShader> ps;
            if (const auto r = device->CreatePixelShader(
                result.blob->GetBufferPointer(),
                result.blob->GetBufferSize(),
                nullptr, &ps); FAILED(r)) {
                spdlog::error("Could make pixel shader for: {}", fileName);
            }
            result.shader.pixelShader = ps;
            break;
        case ShaderType::Geometry:
            ComPtr<ID3D11GeometryShader> gs;
            if (const auto r = device->CreateGeometryShader(
                result.blob->GetBufferPointer(),
                result.blob->GetBufferSize(),
                nullptr, &gs); FAILED(r)) {
                spdlog::error("Could make geometry shader for: {}", fileName);
                }
            result.shader.geometryShader = gs;
            break;
        }
        return result;
    }
};

using ShaderHandle = HandleMap<ShaderContext>::Handle;
}
