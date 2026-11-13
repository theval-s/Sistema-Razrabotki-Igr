#pragma once

#include <windows.h>
#include <memory>

#include <engine/core/Subsystem.hpp>
#include <engine/ui/UiHost.hpp>
#include <engine/input/InputUiEvent.hpp>

namespace engine {

class InputSubsystem;

/// Standalone Win32 message pump and input adapter. Register before InputSubsystem;
/// it feeds the input frame before InputSubsystem::Update() latches it.
/// Qt/editor hosts can continue to feed InputSubsystem through their own event loop.
class ENGINE_API Win32InputHost final : public Subsystem {
public:
    explicit Win32InputHost(HWND window) : window_(window) {}
    ~Win32InputHost() override;
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Update(const FrameContext& frame) override;

    /// Centers/hides the cursor and feeds relative mouse motion until disabled or focus lost.
    void SetRelativeMouseMode(bool enabled);
    [[nodiscard]] bool IsRelativeMouseMode() const noexcept { return relativeMouse_; }
    [[nodiscard]] std::shared_ptr<UiHost> GetUiHost();
    /// Query after the host Update, including DPI-only changes.
    [[nodiscard]] UiSurfaceMetrics GetSurfaceMetrics() const;

private:
    HWND window_ = nullptr;
    InputSubsystem* input_ = nullptr;
    bool relativeMouse_ = false;
    bool hasCursorPosition_ = false;
    POINT lastCursor_{};
    class HostBridge;
    std::shared_ptr<HostBridge> uiHost_;
    bool focused_ = false;
    bool composing_ = false;
    wchar_t highSurrogate_ = 0;
    std::wstring imeEcho_;
    bool HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    void Queue(InputUiEvent event);
    void TextUnit(wchar_t unit);
};

}  // namespace engine
