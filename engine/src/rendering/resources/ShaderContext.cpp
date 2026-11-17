#include <engine/rendering/resources/ShaderContext.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include "../RenderingFailureUtils.hpp"

namespace engine {
namespace {

// Editors such as Visual Studio save UTF-8 with a BOM; FXC rejects it in memory sources
// ("error X3000: Illegal character in shader file").
class ShaderIncludes final : public ID3DInclude {
public:
    explicit ShaderIncludes(std::filesystem::path sourcePath) : sourcePath_(std::move(sourcePath)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent,
                           LPCVOID* data, UINT* size) override {
        try {
            if (!name || !data || !size) return E_POINTER;
            const std::string_view includeName{name};
            std::filesystem::path path;
            if (includeName.starts_with("engine/") || includeName.starts_with("game/")) {
                path = AssetPathResolver::ToAbsolute(AssetPathResolver::DefaultPath(includeName, AssetKind::Shader));
            } else if (type == D3D_INCLUDE_LOCAL) {
                const auto it = files_.find(parent);
                const auto& includingFile = it == files_.end() ? sourcePath_ : it->second->path;
                path = includingFile.parent_path() / FromUtf8(includeName);
            } else {
                error_ += "Unsupported system include: " + std::string(name) + "\n";
                return E_FAIL; // system includes must name an explicit root
            }
            path = path.lexically_normal();
            std::string contents, error;
            if (!ReadTextFile(path, contents, error)) {
                error_ += "Include '" + ToUtf8(path) + "': " + error + "\n";
                return E_FAIL;
            }
            if (contents.size() > std::numeric_limits<UINT>::max()) {
                error_ += "Include too large: " + ToUtf8(path) + "\n";
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
        } catch (const std::exception& e) {
            try { error_ += std::string("Include callback: ") + e.what() + "\n"; } catch (...) {}
            return E_FAIL;
        } catch (...) {
            return E_FAIL;
        }
    }

    HRESULT __stdcall Close(LPCVOID data) override {
        return files_.erase(data) == 1 ? S_OK : E_FAIL;
    }

    const std::string& Error() const noexcept { return error_; }

private:
    struct IncludeFile {
        std::filesystem::path path;
        std::unique_ptr<char[]> contents;
    };
    std::filesystem::path sourcePath_;
    std::string error_;
    std::unordered_map<const void*, std::unique_ptr<IncludeFile>> files_;
};

} // namespace

namespace {
ShaderDiagnosticContext DiagnosticContext(ShaderType type, const ShaderCompileOptions& options) {
    ShaderDiagnosticContext context;
    context.stage = type == ShaderType::Vertex ? "vertex" : type == ShaderType::Pixel ? "pixel" :
                    type == ShaderType::Geometry ? "geometry" : "unknown";
    context.entryPoint = options.entryPoint.empty() ? GetEntryPoint(type) : options.entryPoint;
    context.target = GetDX11Target(type);
    for (const auto& define : options.defines) context.defines.emplace_back(define.name, define.value);
    return context;
}

void CheckShader(HRESULT hr, const char* operation, const CompiledShader& compiled,
                 std::string detail = {}, ID3D11Device* device = nullptr) {
    if (SUCCEEDED(hr)) return;
    RenderingFailure failure{.operation = operation, .summary = std::string(operation) + " failed",
        .detail = std::move(detail), .path = compiled.sourcePath,
        .nativeError = static_cast<std::int64_t>(hr), .shader = compiled.diagnosticContext};
    if (device) {
        const auto reason = device->GetDeviceRemovedReason();
        if (FAILED(reason)) failure.deviceRemovalReason = static_cast<std::int64_t>(reason);
    }
    throw RenderingException(std::move(failure));
}
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
            rendering_failure::Check(res, "GetInputParameterDesc", nullptr, std::to_string(i));
        }
        if (desc.SystemValueType != D3D_NAME_UNDEFINED) // Skip as it doesn't affect shader signature
            continue;
        result.Add(desc.SemanticName, desc.SemanticIndex, desc.ComponentType, desc.Mask);
    }
    return result;
}

CompiledShader ShaderContext::CompileCpu(const std::filesystem::path& fileName,
                                                        const ShaderType shaderType,
                                                        const ShaderCompileOptions& options) {
    const auto sourcePath = AssetPathResolver::ToAbsolute(fileName);
    std::string source, error;
    if (!ReadTextFile(sourcePath, source, error)) {
        // ReadTextFile supplies owned detail but does not expose a native error code.
        throw RenderingException({.operation = "ReadShaderSource", .summary = "Cannot read shader source",
            .detail = std::move(error), .path = sourcePath, .shader = DiagnosticContext(shaderType, options)});
    }
    return CompileSource(source, sourcePath, shaderType, options);
}

CompiledShader ShaderContext::CompileSource(const std::string_view source,
                                                            const std::filesystem::path& virtualPath,
                                                            const ShaderType shaderType,
                                                            const ShaderCompileOptions& options) {
    CompiledShader result{};
    result.type = shaderType;
    const auto sourcePath = AssetPathResolver::ToAbsolute(virtualPath);
    result.sourcePath = sourcePath;
    result.diagnosticContext = DiagnosticContext(shaderType, options);
    CheckShader(result.diagnosticContext.target.empty() ? E_INVALIDARG : S_OK,
                "ValidateShaderStage", result, "Unsupported shader stage.");
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
        std::string detail;
        if (errors) {
            detail.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            if (!detail.empty() && detail.back() == '\0') detail.pop_back();
        }
        detail += "\n" + includes.Error();
        CheckShader(res, "D3DCompile", result, std::move(detail));
    }

    const auto reflectRes = D3DReflect(
        result.blob->GetBufferPointer(),
        result.blob->GetBufferSize(),
        IID_ID3D11ShaderReflection,
        reinterpret_cast<void**>(result.reflection.GetAddressOf()));
    CheckShader(reflectRes, "D3DReflect", result);

    if (const auto r = result.reflection->GetDesc(&result.desc); FAILED(r)) {
        CheckShader(r, "GetShaderDesc", result);
    }

    for (UINT i = 0; i < result.desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        if (const auto r = result.reflection->GetResourceBindingDesc(i, &bind); FAILED(r)) {
            CheckShader(r, "GetResourceBindingDesc", result, std::to_string(i));
        }
        if (bind.BindCount != 1) {
            CheckShader(E_NOTIMPL, "ValidateShaderBinding", result, "Array bindings are unsupported: " + std::string(bind.Name));
        }
        result.bindings.emplace_back(bind.Name, static_cast<uint8_t>(bind.BindPoint),
                                     static_cast<uint8_t>(bind.Type));
    }

    return result;
}

ShaderContext ShaderContext::FinalizeOnDevice(const CompiledShader& compiled,
                                                              ID3D11Device* device,
                                                              InputLayoutCache& layoutCache) {
    ShaderContext result{};
    CheckShader(device ? S_OK : E_POINTER, "FinalizeShader", compiled, "No D3D11 device is available.");
    CheckShader(compiled.blob && compiled.reflection ? S_OK : E_INVALIDARG,
                "FinalizeShader", compiled, "Compiled shader bytecode/reflection is missing.");
    result.type = compiled.type;
    result.blob = compiled.blob;
    result.bindings = compiled.bindings;

    switch (compiled.type) {
    case ShaderType::Vertex: {
        ComPtr<ID3D11VertexShader> vs;
        if (const auto r = device->CreateVertexShader(
            result.blob->GetBufferPointer(),
            result.blob->GetBufferSize(),
            nullptr, &vs); FAILED(r)) {
            CheckShader(r, "CreateVertexShader", compiled, {}, device);
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
            CheckShader(r, "CreatePixelShader", compiled, {}, device);
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
            CheckShader(r, "CreateGeometryShader", compiled, {}, device);
        }
        result.shader.geometryShader = std::move(gs);
        break;
    }
    default:
        CheckShader(E_INVALIDARG, "FinalizeShader", compiled, "Unsupported shader stage.");
    }
    if (compiled.type == ShaderType::Vertex) {
        try {
            auto layout = ParseVertexShaderInputs(compiled.reflection, compiled.desc, result.blob);
            result.layoutHandle = layoutCache.RegisterShaderLayout(std::move(layout));
        } catch (const RenderingException& e) {
            auto failure = e.GetFailure();
            failure.path = compiled.sourcePath;
            failure.shader = compiled.diagnosticContext;
            throw RenderingException(std::move(failure));
        }
    }
    return result;
}

ShaderContext ShaderContext::CompileShader(ID3D11Device* device, InputLayoutCache& layoutCache,
                                                            const std::filesystem::path& fileName,
                                                            const ShaderType shaderType,
                                                            const ShaderCompileOptions& options) {
    const auto compiled = CompileCpu(fileName, shaderType, options);
    return FinalizeOnDevice(compiled, device, layoutCache);
}

}
