#pragma once

#include <cstdint>

namespace engine {

/// The native window the engine should present into.
///
/// The host owns the window: a standalone game creates its own Win32 window,
/// the editor passes the HWND of its viewport widget. The engine (i.e. the
/// rendering subsystem) never creates or destroys it.
struct NativeSurface {
    void* windowHandle = nullptr;  // HWND on Windows; nullptr means headless
    std::uint32_t width = 0;       // in physical pixels
    std::uint32_t height = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return windowHandle != nullptr && width > 0 && height > 0;
    }
};

}  // namespace engine
