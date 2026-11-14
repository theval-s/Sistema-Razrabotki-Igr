#include "GameWindow.hpp"
#include <engine/core/Surface.hpp>

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

    DWORD windowStyle = 0;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    if (options.fullscreen) {
        windowStyle = WS_POPUP;

        x = 0;
        y = 0;

        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
    }
    else {
        windowStyle = WS_OVERLAPPEDWINDOW;

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

        x = CW_USEDEFAULT;
        y = CW_USEDEFAULT;

        width = windowRect.right - windowRect.left;
        height = windowRect.bottom - windowRect.top;
    }

    HWND hwnd = CreateWindowExW(
        windowExStyle,
        window->className_.c_str(),
        options.title.c_str(),
        windowStyle,
        x,
        y,
        width,
        height,
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