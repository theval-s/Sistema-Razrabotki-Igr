#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <engine/engine_export.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <wrl/client.h>

#include <engine/rendering/resources/VertexLayoutCache.hpp>

namespace engine {

using Microsoft::WRL::ComPtr;

enum class ShaderType {
    Vertex,
    Pixel,
    Geometry,
    Compute,
};

struct ShaderDefine {
    std::string name;
    std::string value = "1";
};

struct ShaderCompileOptions {
    /// Empty selects VSMain, PSMain, GSMain or CSMain for the requested stage.
    std::string entryPoint;
    std::vector<ShaderDefine> defines;
};

/// One compile of a shader file: the unit a cooked build's shader library stores.
struct ShaderVariant {
    std::filesystem::path file;
    ShaderType stage = ShaderType::Vertex;
    ShaderCompileOptions options;
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
    ComPtr<ID3D11ComputeShader> computeShader;
};

/// Bytecode and reflection of one compiled stage. Device-independent: it can be produced
/// on a worker thread; only FinalizeOnDevice touches the D3D device.
struct ENGINE_API CompiledShader {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC desc{};
    std::vector<ShaderBinding> bindings;
    ShaderType type = ShaderType::Vertex;
    // Retained across worker compilation -> device upload for creation diagnostics.
    std::filesystem::path sourcePath;
    ShaderDiagnosticContext diagnosticContext;
};

struct ENGINE_API ShaderContext {
    ComPtr<ID3DBlob> blob;
    ShaderType type;
    ShaderLayoutHandle layoutHandle;
    
    DXShader shader;

    // debug only, later we'll add correctness checks based on names and slots
    std::vector<ShaderBinding> bindings{};

    /// All compile/finalize methods return usable values or throw RenderingException,
    /// preserving read/include/compiler/reflection/device details and shader context.
    static ShaderContext CompileShader(ID3D11Device* device, InputLayoutCache& layoutCache,
                                                        const std::filesystem::path& fileName, ShaderType shaderType,
                                                        const ShaderCompileOptions& options = {});

    /// D3DCompile + reflection only; runs on any thread. A cooked build reads the bytecode from
    /// its shader library instead and never opens the file.
    static CompiledShader CompileCpu(const std::filesystem::path& fileName, ShaderType shaderType,
                                                    const ShaderCompileOptions& options = {});
    /// Compiles generated HLSL; virtualPath supplies its include location and diagnostic name.
    static CompiledShader CompileSource(std::string_view source,
                                                       const std::filesystem::path& virtualPath, ShaderType shaderType,
                                                       const ShaderCompileOptions& options = {});
    /// Optimized bytecode without debug info, for the shader library of a cooked build.
    static std::optional<std::vector<std::uint8_t>> CompileForShipping(const std::filesystem::path& fileName,
                                                                      ShaderType shaderType,
                                                                      const ShaderCompileOptions& options,
                                                                      std::string& error);
    /// Create*Shader and input-layout registration; main thread.
    static ShaderContext FinalizeOnDevice(const CompiledShader& compiled, ID3D11Device* device,
                                                          InputLayoutCache& layoutCache);

private:
    static ShaderVertexLayout ParseVertexShaderInputs(const ComPtr<ID3D11ShaderReflection>& reflection,
                                                       const D3D11_SHADER_DESC& shaderDesc,
                                                       const ComPtr<ID3DBlob>& blob);
};

using ShaderHandle = HandleMap<ShaderContext>::Handle;
}
