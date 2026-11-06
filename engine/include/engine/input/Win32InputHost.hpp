#pragma once

#include <windows.h>

#include <engine/core/Subsystem.hpp>

namespace engine {

class InputSubsystem;

/// Standalone Win32 message pump and input adapter. Register before InputSubsystem;
/// it feeds the input frame before InputSubsystem::Update() latches it.
/// Qt/editor hosts can continue to feed InputSubsystem through their own event loop.
class ENGINE_API Win32InputHost final : public Subsystem {
public:
    explicit Win32InputHost(HWND window) : window_(window) {}
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Update(const FrameContext& frame) override;

    /// Centers/hides the cursor and feeds relative mouse motion until disabled or focus lost.
    void SetRelativeMouseMode(bool enabled);
    [[nodiscard]] bool IsRelativeMouseMode() const noexcept { return relativeMouse_; }

private:
    HWND window_ = nullptr;
    InputSubsystem* input_ = nullptr;
    bool relativeMouse_ = false;
    bool hasCursorPosition_ = false;
    POINT lastCursor_{};
};

}  // namespace engine
