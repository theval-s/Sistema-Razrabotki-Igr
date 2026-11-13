#pragma once

#include <windows.h>

#include <engine/core/Subsystem.hpp>
#include <engine/input/InputTypes.hpp>

namespace engine {

class InputSubsystem;

/// Standalone Win32 message pump and input adapter. Register before InputSubsystem;
/// it feeds the input frame before InputSubsystem::Update() latches it.
/// Qt/editor hosts can continue to feed InputSubsystem through their own event loop.
/// Applies InputSubsystem's cursor mode while the window is in the foreground, and again
/// whenever it comes back. Locked centers the cursor and feeds relative motion.
class ENGINE_API Win32InputHost final : public Subsystem {
public:
    explicit Win32InputHost(HWND window) : window_(window) {}
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Update(const FrameContext& frame) override;

private:
    void ApplyCursorMode(CursorMode mode);

    HWND window_ = nullptr;
    InputSubsystem* input_ = nullptr;
    /// What the cursor does now; Normal while the window is in the background.
    CursorMode cursorMode_ = CursorMode::Normal;
    bool hasCursorPosition_ = false;
    POINT lastCursor_{};
};

}  // namespace engine
