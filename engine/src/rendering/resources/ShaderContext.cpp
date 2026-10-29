#include <engine/rendering/resources/ShaderContext.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <spdlog/spdlog.h>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {

// Editors such as Visual Studio save UTF-8 with a BOM; FXC rejects it in memory sources
// ("error X3000: Illegal character in shader file").
void StripUtf8Bom(std::string& text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
}

class ShaderIncludes final : public ID3DInclude {
public:
    explicit ShaderIncludes(std::filesystem::path sourcePath) : sourcePath_(std::move(sourcePath)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent,
                           LPCVOID* data, UINT* size) override {
        const std::string_view includeName{name};
        std::filesystem::path path;
        if (includeName.starts_with("engine/") || includeName.starts_with("game/")) {
            path = AssetPathResolver::ToAbsolute(AssetPathResolver::DefaultPath(includeName, AssetKind::Shader));
        } else if (type == D3D_INCLUDE_LOCAL) {
            const auto it = files_.find(parent);
            const auto& includingFile = it == files_.end() ? sourcePath_ : it->second->path;
            path = includingFile.parent_path() / FromUtf8(includeName);
        } else {
            return E_FAIL; // system includes must name an explicit root
        }
        path = path.lexically_normal();
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            spdlog::error("Cannot include shader file '{}' from '{}'", ToUtf8(path), ToUtf8(sourcePath_));
            return E_FAIL;
        }
        std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        StripUtf8Bom(contents);
        if (contents.size() > std::numeric_limits<UINT>::max()) {
            return E_FAIL;
        }
        auto file = std::make_unique<IncludeFile>();
        file->path = std::move(path);
        file->contents = std::make_unique<char[]>(std::max<std::size_t>(contents.size(), 1));
        std::copy(contents.begin(), contents.end(), file->contents.get());
        *size = static_cast<UINT>(contents.size());
        *data = file->contents.get();
        files_.emplace(*data, std::move(file));
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID data) override {
        return files_.erase(data) == 1 ? S_OK : E_FAIL;
    }

private:
    struct IncludeFile {
        std::filesystem::path path;
        std::unique_ptr<char[]> contents;
    };
    std::filesystem::path sourcePath_;
    std::unordered_map<const void*, std::unique_ptr<IncludeFile>> files_;
};

} // namespace

const char* GetEntryPoint(const ShaderType type) {
    switch (type) {
    case ShaderType::Vertex: return "VSMain";
    case ShaderType::Pixel: return "PSMain";
    case ShaderType::Geometry: return "GSMain";
    }
    return "";
}

const char* GetDX11Target(const ShaderType type) {
    switch (type) {
    case ShaderType::Vertex: return "vs_5_0";
    case ShaderType::Pixel: return "ps_5_0";
    case ShaderType::Geometry: return "gs_5_0";
    }
    return "";
}

ShaderVertexLayout ShaderContext::ParseVertexShaderInputs(
    const ComPtr<ID3D11ShaderReflection>& reflection, const D3D11_SHADER_DESC& shaderDesc,
    const ComPtr<ID3DBlob>& blob) {
    ShaderVertexLayout result{blob};
    for (UINT i = 0; i < shaderDesc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC desc{};
        if (const auto res = reflection->GetInputParameterDesc(i, &desc); FAILED(res)) {
            spdlog::error("Unexpected error cannot get input parameter desc {}", i);
            continue;
        }
        if (desc.SystemValueType != D3D_NAME_UNDEFINED) // Skip as it doesn't affect shader signature
            continue;
        result.Add(desc.SemanticName, desc.SemanticIndex, desc.ComponentType, desc.Mask);
    }
    return result;
}

std::optional<CompiledShader> ShaderContext::CompileCpu(const std::filesystem::path& fileName,
                                                        const ShaderType shaderType,
                                                        const ShaderCompileOptions& options) {
    std::ifstream input(fileName, std::ios::binary);
    if (!input) {
        spdlog::error("Missing shader file: {}", ToUtf8(fileName));
        return std::nullopt;
    }
    std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    StripUtf8Bom(source);
    return CompileSource(source, fileName, shaderType, options);
}

std::optional<CompiledShader> ShaderContext::CompileSource(const std::string_view source,
                                                            const std::filesystem::path& virtualPath,
                                                            const ShaderType shaderType,
                                                            const ShaderCompileOptions& options) {
    CompiledShader result{};
    result.type = shaderType;
    const auto sourcePath = AssetPathResolver::ToAbsolute(virtualPath);
    const auto displayName = ToUtf8(sourcePath);
    ShaderIncludes includes{sourcePath};
    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(options.defines.size() + 1);
    for (const auto& define : options.defines) {
        macros.push_back({define.name.c_str(), define.value.c_str()});
    }
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> errors;
    const auto res = D3DCompile(source.data(), source.size(), displayName.c_str(),
                                         macros.data(), &includes,
                                         options.entryPoint.empty() ? GetEntryPoint(shaderType) : options.entryPoint.c_str(),
                                         GetDX11Target(shaderType),
                                         D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, // todo: should be toggleable
                                         0,
                                         result.blob.GetAddressOf(),
                                         errors.GetAddressOf());
    if (FAILED(res)) {
        if (errors) {
            spdlog::error("Could not compile shader {}: {}", displayName,
                          static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            spdlog::error("Could not compile shader {}: {:08x}", displayName, static_cast<unsigned>(res));
        }
        return std::nullopt;
    }

    const auto reflectRes = D3DReflect(
        result.blob->GetBufferPointer(),
        result.blob->GetBufferSize(),
        IID_ID3D11ShaderReflection,
        reinterpret_cast<void**>(result.reflection.GetAddressOf()));
    if (FAILED(reflectRes)) {
        spdlog::error("Could not get shader reflection for shader: {}", displayName);
        return std::nullopt;
    }

    if (const auto r = result.reflection->GetDesc(&result.desc); FAILED(r)) {
        spdlog::error("Could not get shader description for shader: {}", displayName);
        return std::nullopt;
    }

    for (UINT i = 0; i < result.desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        if (const auto r = result.reflection->GetResourceBindingDesc(i, &bind); FAILED(r)) {
            spdlog::error("Could not get resource binding description for shader: {}", displayName);
            continue;
        }
        if (bind.BindCount != 1) {
            spdlog::error("Array bindings are unsupported: {}", displayName);
            continue;
        }
        result.bindings.emplace_back(bind.Name, static_cast<uint8_t>(bind.BindPoint),
                                     static_cast<uint8_t>(bind.Type));
    }

    return result;
}

std::optional<ShaderContext> ShaderContext::FinalizeOnDevice(const CompiledShader& compiled,
                                                              ID3D11Device* device,
                                                              InputLayoutCache& layoutCache) {
    ShaderContext result{};
    result.type = compiled.type;
    result.blob = compiled.blob;
    result.bindings = compiled.bindings;

    if (compiled.type == ShaderType::Vertex) {
        auto layout = ParseVertexShaderInputs(compiled.reflection, compiled.desc, result.blob);
        result.layoutHandle = layoutCache.RegisterShaderLayout(std::move(layout));
    }

    switch (compiled.type) {
    case ShaderType::Vertex: {
        ComPtr<ID3D11VertexShader> vs;
        if (const auto r = device->CreateVertexShader(
            result.blob->GetBufferPointer(),
            result.blob->GetBufferSize(),
            nullptr, &vs); FAILED(r)) {
            spdlog::error("Could not create vertex shader from bytecode: {:08x}", static_cast<unsigned>(r));
        }
        result.shader.vertexShader = std::move(vs);
        break;
    }
    case ShaderType::Pixel: {
        ComPtr<ID3D11PixelShader> ps;
        if (const auto r = device->CreatePixelShader(
            result.blob->GetBufferPointer(),
            result.blob->GetBufferSize(),
            nullptr, &ps); FAILED(r)) {
            spdlog::error("Could not create pixel shader from bytecode: {:08x}", static_cast<unsigned>(r));
        }
        result.shader.pixelShader = std::move(ps);
        break;
    }
    case ShaderType::Geometry: {
        ComPtr<ID3D11GeometryShader> gs;
        if (const auto r = device->CreateGeometryShader(
            result.blob->GetBufferPointer(),
            result.blob->GetBufferSize(),
            nullptr, &gs); FAILED(r)) {
            spdlog::error("Could not create geometry shader from bytecode: {:08x}", static_cast<unsigned>(r));
        }
        result.shader.geometryShader = std::move(gs);
        break;
    }
    }
    return result;
}

std::optional<ShaderContext> ShaderContext::CompileShader(ID3D11Device* device, InputLayoutCache& layoutCache,
                                                            const std::filesystem::path& fileName,
                                                            const ShaderType shaderType,
                                                            const ShaderCompileOptions& options) {
    const auto compiled = CompileCpu(fileName, shaderType, options);
    if (!compiled) {
        return std::nullopt;
    }
    return FinalizeOnDevice(*compiled, device, layoutCache);
}

}
