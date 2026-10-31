#include <engine/input/Win32InputHost.hpp>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32PhysicalKey.hpp>

namespace engine {

std::string_view Win32InputHost::GetName() const { return "Win32InputHost"; }

bool Win32InputHost::Initialize() {
    input_ = GetEngine().GetSubsystem<InputSubsystem>();
    return window_ && input_;
}

void Win32InputHost::SetRelativeMouseMode(bool enabled) {
    if (enabled == relativeMouse_) return;
    if (enabled) {
        if (GetForegroundWindow() != window_) return;
        SetCapture(window_);
        while (ShowCursor(FALSE) >= 0) {}
        relativeMouse_ = true;
        RECT client{};
        GetClientRect(window_, &client);
        POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(window_, &center);
        SetCursorPos(center.x, center.y);
    } else {
        relativeMouse_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        while (ShowCursor(TRUE) < 0) {}
    }
    hasCursorPosition_ = false;
}

void Win32InputHost::Shutdown() {
    SetRelativeMouseMode(false);
    input_ = nullptr;
}

void Win32InputHost::Update(const FrameContext&) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            GetEngine().RequestExit();
            return;
        }
        if (message.hwnd == window_) {
            switch (message.message) {
                case WM_KEYDOWN: case WM_SYSKEYDOWN:
                case WM_KEYUP: case WM_SYSKEYUP:
                    input_->OnPhysicalKey(PhysicalKeyFromWin32Scan(
                        (static_cast<unsigned long long>(message.lParam) >> 16) & 0xff,
                        (static_cast<unsigned long long>(message.lParam) & 0x01000000) != 0),
                        message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN);
                    break;
                case WM_LBUTTONDOWN: input_->OnMouseButton(MouseButton::Left, true); break;
                case WM_LBUTTONUP: input_->OnMouseButton(MouseButton::Left, false); break;
                case WM_RBUTTONDOWN: input_->OnMouseButton(MouseButton::Right, true); break;
                case WM_RBUTTONUP: input_->OnMouseButton(MouseButton::Right, false); break;
                case WM_MBUTTONDOWN: input_->OnMouseButton(MouseButton::Middle, true); break;
                case WM_MBUTTONUP: input_->OnMouseButton(MouseButton::Middle, false); break;
                case WM_MOUSEWHEEL:
                    input_->OnMouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(message.wParam)) / WHEEL_DELTA);
                    break;
                case WM_KILLFOCUS:
                    input_->OnFocusLost();
                    SetRelativeMouseMode(false);
                    hasCursorPosition_ = false;
                    break;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (!IsWindow(window_) || GetForegroundWindow() != window_) return;
    if (relativeMouse_) {
        RECT client{};
        GetClientRect(window_, &client);
        POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(window_, &center);
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            input_->OnMouseMove(static_cast<float>(cursor.x - center.x), static_cast<float>(cursor.y - center.y));
            SetCursorPos(center.x, center.y);
        }
    } else {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            if (hasCursorPosition_)
                input_->OnMouseMove(static_cast<float>(cursor.x - lastCursor_.x),
                                    static_cast<float>(cursor.y - lastCursor_.y));
            lastCursor_ = cursor;
            hasCursorPosition_ = true;
        }
    }
}

}  // namespace engine
