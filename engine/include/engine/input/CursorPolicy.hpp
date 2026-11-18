#pragma once

#include <engine/input/InputTypes.hpp>

namespace engine {

/// Host ownership and UI requirements arbitrate the persistent gameplay request.
/// This policy has no platform or UI subsystem dependencies.
struct CursorPolicy {
    CursorMode mode = CursorMode::Normal;
    /// Mouse-look only. UI drags use the host's button-held capture, so a menu never blocks the window frame.
    bool grab = false;
    bool interactiveUi = false;
};

[[nodiscard]] constexpr CursorPolicy ResolveCursorPolicy(CursorMode requested, bool ownsInput,
    bool pointerCapture, bool wantsVisibleCursor, bool textInput) noexcept {
    if (!ownsInput) return {};
    const bool interactive = pointerCapture || wantsVisibleCursor || textInput;
    const auto mode = interactive ? CursorMode::Normal : requested;
    return {mode, mode == CursorMode::Locked, interactive};
}

} // namespace engine
