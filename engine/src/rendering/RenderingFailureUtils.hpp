#pragma once

#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/core/PathUtils.hpp>
#include <d3d11.h>
#include <format>

namespace engine::rendering_failure {

inline std::string Format(const RenderingFailure& failure) {
    std::string text = failure.summary + "\nOperation: " + failure.operation;
    if (!failure.pass.empty()) text += "\nPass: " + failure.pass;
    if (!failure.path.empty()) text += "\nPath: " + ToUtf8(failure.path);
    if (failure.nativeError)
        text += std::format("\nNative error: {} (0x{:08X})", *failure.nativeError,
                            static_cast<std::uint32_t>(*failure.nativeError));
    if (failure.deviceRemovalReason)
        text += std::format("\nDevice removal reason: {} (0x{:08X})", *failure.deviceRemovalReason,
                            static_cast<std::uint32_t>(*failure.deviceRemovalReason));
    if (failure.shader) {
        text += "\nShader stage: " + failure.shader->stage + "\nEntry: " + failure.shader->entryPoint +
                "\nTarget: " + failure.shader->target;
        for (const auto& [name, value] : failure.shader->defines)
            text += "\nDefine: " + name + "=" + value;
    }
    if (!failure.detail.empty()) text += "\n" + failure.detail;
    return text;
}

inline void Check(HRESULT result, const char* operation, ID3D11Device* device = nullptr,
                  std::string detail = {}) {
    if (SUCCEEDED(result)) return;
    RenderingFailure failure{.operation = operation, .summary = std::string(operation) + " failed",
                             .detail = std::move(detail), .nativeError = static_cast<std::int64_t>(result)};
    if (device) {
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (FAILED(reason)) failure.deviceRemovalReason = static_cast<std::int64_t>(reason);
    }
    throw RenderingException(std::move(failure));
}

inline void RequireDevice(ID3D11Device* device, const char* operation) {
    if (!device) Check(E_POINTER, operation, nullptr, "No D3D11 device is available.");
}

} // namespace engine::rendering_failure
