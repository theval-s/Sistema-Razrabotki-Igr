#include "GameWindow.hpp"
#include <engine/core/Surface.hpp>
#include <engine/window.hpp>

#include <cstdint>
#include <string>


GameWindow::~GameWindow() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (classRegistered_) {
        UnregisterClassW(className_.c_str(), hInstance_);
        classRegistered_ = false;
    }
}


std::unique_ptr<GameWindow> GameWindow::Create(const Options& options, std::string& error) {
    error.clear();

    if (options.width == 0 || options.height == 0) {
        error = "Window dimensions must be greater than zero.";
        return nullptr;
    }

    auto window = std::unique_ptr<GameWindow>(new GameWindow());

    window->hInstance_ = GetModuleHandleW(nullptr);

    if (window->hInstance_ == nullptr) {
        error = "GetModuleHandleW failed. Error: " + std::to_string(GetLastError());
        return nullptr;
    }

    window->className_ = L"SRI_GameWindow_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(window.get()));

    WNDCLASSEXW windowClass{};

    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    windowClass.lpfnWndProc = &GameWindow::Proc;
    windowClass.hInstance = window->hInstance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = window->className_.c_str();

    if (RegisterClassExW(&windowClass) == 0) {
        error = "RegisterClassExW failed. Error: " + std::to_string(GetLastError());
        return nullptr;
    }

    window->classRegistered_ = true;

    constexpr DWORD windowExStyle = WS_EX_APPWINDOW;
    constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW;

    // Fullscreen starts from a real window too, so Alt+Enter has one to go back to.
    RECT windowRect{
        0,
        0,
        static_cast<LONG>(options.width),
        static_cast<LONG>(options.height)
    };

    if (!AdjustWindowRectEx(
        &windowRect,
        windowStyle,
        FALSE,
        windowExStyle
    )) {
        error = "AdjustWindowRectEx failed. Error: " + std::to_string(GetLastError());
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(
        windowExStyle,
        window->className_.c_str(),
        options.title.c_str(),
        windowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        window->hInstance_,
        window.get()
    );

    if (hwnd == nullptr) {
        error = "CreateWindowExW failed. Error: " + std::to_string(GetLastError());
        return nullptr;
    }

    window->hwnd_ = hwnd;

    if (options.fullscreen) {
        window->SetFullscreen(true);
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    return window;
}

engine::NativeSurface GameWindow::Surface() const {
    engine::NativeSurface surface{};

    surface.windowHandle = hwnd_;

    if (hwnd_ == nullptr || minimized_) {
        return surface;
    }

    RECT clientRect{};

    if (!GetClientRect(hwnd_, &clientRect)) {
        return surface;
    }

    surface.width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
    surface.height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);

    return surface;
}

void GameWindow::NotifyResize() {
    if (!onResize_) {
        return;
    }

    if (minimized_ || hwnd_ == nullptr) {
        onResize_(0, 0);
        return;
    }

    RECT clientRect{};

    if (!GetClientRect(hwnd_, &clientRect)) {
        return;
    }

    const auto width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
    const auto height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);
    onResize_(width, height);
}

void GameWindow::SetFullscreen(const bool fullscreen) {
    if (hwnd_ == nullptr || fullscreen == fullscreen_) {
        return;
    }

    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));

    if (fullscreen) {
        if (!GetWindowPlacement(hwnd_, &windowedPlacement_)) {
            return;
        }

        // A window that was hidden or minimized comes back as a normal one.
        if (windowedPlacement_.showCmd != SW_SHOWMAXIMIZED) {
            windowedPlacement_.showCmd = SW_SHOWNORMAL;
        }

        SetWindowLongPtrW(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        fullscreen_ = true;
        FitToMonitor();
    }
    else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        fullscreen_ = false;
        SetWindowPlacement(hwnd_, &windowedPlacement_);
        SetWindowPos(
            hwnd_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );
    }
}

void GameWindow::FitToMonitor() {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(MONITORINFO);

    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }

    const RECT& area = monitor.rcMonitor;

    SetWindowPos(
        hwnd_,
        nullptr,
        area.left,
        area.top,
        area.right - area.left,
        area.bottom - area.top,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );
}

LRESULT CALLBACK GameWindow::Proc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp
) {
    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lp);

        auto* self = static_cast<GameWindow*>(createStruct->lpCreateParams);

        if (self == nullptr) {
            return FALSE;
        }

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self)
        );

        self->hwnd_ = hwnd;

        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<GameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // Window keys go before the input host: the game never sees the Enter of Alt+Enter.
    if (self != nullptr) {
        switch (msg) {

        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN) != 0) {
                if ((HIWORD(lp) & KF_REPEAT) == 0) {
                    self->SetFullscreen(!self->fullscreen_);
                }
                return 0;
            }
            break;

        case WM_SYSCOMMAND:
            // Alt or F10 on its own would enter the modal menu loop and stall the game.
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            // The system's suggested rect is for a framed window and would shrink a fullscreen one.
            if (self->fullscreen_) {
                self->FitToMonitor();
                return 0;
            }
            break;

        default:
            break;
        }
    }

    // Win32InputHost reads keys, buttons, focus and text from here.
    if (LRESULT result = 0; engine::DispatchInputMessage(hwnd, msg, wp, lp, result)) {
        return result;
    }

    if (self != nullptr) {
        switch (msg) {

        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;

        case WM_ENTERSIZEMOVE:
            self->resizing_ = true;
            break;

        case WM_EXITSIZEMOVE:
            self->resizing_ = false;
            self->NotifyResize();
            break;

        case WM_SIZE:
            self->minimized_ = (wp == SIZE_MINIMIZED);
            if (!self->resizing_) {
                self->NotifyResize();
            }
            break;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            break;

        default:
            break;
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}