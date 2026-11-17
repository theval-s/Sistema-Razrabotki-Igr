#include <utility>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include "../rendering/RenderingFailureUtils.hpp"

namespace engine {

bool ShaderImporter::ReadCpu(const AssetDesc& desc, ID3D11Device* /*device*/, CpuPayload& payload,
                            std::string& error) const try {
    const auto path = AssetPathResolver::ResolveShader(desc);

    auto compiled = desc.shaderFromMemory
        ? ShaderContext::CompileSource(desc.shaderSource, path, desc.shaderStage, desc.shaderOptions)
        : ShaderContext::CompileCpu(path, desc.shaderStage, desc.shaderOptions);

    payload = CpuPayload{};
    payload.kind = CpuPayload::Kind::Shader;
    payload.shader = std::move(compiled);
    return true;
} catch (const RenderingException& e) {
    error = rendering_failure::Format(e.GetFailure());
    return false;
} catch (const std::exception& e) {
    error = e.what();
    return false;
} catch (...) {
    error = "ShaderImporter: unknown read failure";
    return false;
}

bool ShaderImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                               GpuPayload& result, std::string& error) const try {
    if (!transfer.resources) { error = "ShaderImporter: missing resource manager"; return false; }
    if (payload.kind != CpuPayload::Kind::Shader) {
        error = "ShaderImporter: payload is not compiled shader bytecode";
        return false;
    }

    const auto handle = transfer.resources->RegisterCompiledShader(payload.shader);
    if (!handle) {
        error = "shader registration on device failed";
        return false;
    }

    result.shader = handle;
    return true;
} catch (const RenderingException& e) {
    error = rendering_failure::Format(e.GetFailure());
    return false;
} catch (const std::exception& e) {
    error = e.what();
    return false;
} catch (...) {
    error = "ShaderImporter: unknown upload failure";
    return false;
}

}  // namespace engine
