#pragma once

#include <d3d11.h>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <fmt/format.h>

namespace engine::rendering_detail {
inline void Check(HRESULT result, const char* operation, ID3D11Device* device = nullptr,
                  std::string detail = {}) {
    if (SUCCEEDED(result)) return;
    RenderingFailure failure;
    failure.operation = operation;
    failure.summary = fmt::format("{} failed (HRESULT 0x{:08X})", operation, static_cast<unsigned long>(result));
    failure.detail = std::move(detail);
    failure.nativeError = static_cast<std::int64_t>(result);
    if (device) {
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (FAILED(reason)) failure.deviceRemovalReason = static_cast<std::int64_t>(reason);
    }
    throw RenderingException(std::move(failure));
}

template<class F>
void InPass(const char* name, F&& operation) {
    try { operation(); }
    catch (const RenderingException& error) {
        auto failure = error.GetFailure();
        if (failure.pass.empty()) failure.pass = name;
        throw RenderingException(std::move(failure));
    }
}
} // namespace engine::rendering_detail
