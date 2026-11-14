#include "engine/window.hpp"

#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <utility>

namespace engine {
namespace {
std::unordered_map<HWND, InputMessageHandler> inputHandlers;
}

void SetInputMessageHandler(HWND window, InputMessageHandler handler) {
    if (handler) inputHandlers[window] = std::move(handler);
    else inputHandlers.erase(window);
}

bool DispatchInputMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    const auto it = inputHandlers.find(window);
    if (it == inputHandlers.end()) return false;
    const auto handler = it->second;  // copy: the handler may detach itself
    const bool handled = handler(window, message, wparam, lparam, result);
    if (message == WM_NCDESTROY) inputHandlers.erase(window);
    return handled;
}

//this function is even shittier than you think, it uses local statics, hence only one window could be handled properly
HWND makeWindow(const std::uint32_t screenWidth, const uint32_t screenHeight, WindowResizedCallback * resizedCallback) {
    // Standalone surfaces and input use physical pixels. An embedding host that
    // already selected process awareness keeps its existing policy.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    static WindowResizedCallback * callback = resizedCallback;
    
    WNDPROC windowFunction = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)-> LRESULT{
        static int newWidth = 0;
        static int newHeight = 0;
        static bool isResizing = false;
        if (LRESULT result = 0; DispatchInputMessage(hwnd, message, wparam, lparam, result)) return result;
        if (message == WM_DESTROY) PostQuitMessage(0);
        if (message == WM_ENTERSIZEMOVE) {
            isResizing = true;
        } else if (message == WM_SIZE) {
            newWidth = LOWORD(lparam);
            newHeight = HIWORD(lparam);
            if (!isResizing && callback) {
                callback(newWidth, newHeight);
            }
        } else if (message == WM_EXITSIZEMOVE) {
            if (isResizing) {
                isResizing = false;
                if (callback) {
                    callback(newWidth, newHeight);
                }
            }
        }
        return DefWindowProc(hwnd, message, wparam, lparam);
    };

    HINSTANCE hinstance = GetModuleHandle(nullptr);
    LPCWSTR applicationName = L"ExampleGame";

    WNDCLASSEX windowClass{
        .cbSize = sizeof(WNDCLASSEX),
        .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
        .lpfnWndProc = windowFunction,
        .hInstance = hinstance,
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)),
        .lpszClassName = applicationName,
    };

    if (RegisterClassEx(&windowClass) == 0) {
        std::cout << "RegisterClassEx() failed:" << '\n' << GetLastError() << '\n';
        std::exit(-1);
    }

    constexpr DWORD windowExStyle = WS_EX_APPWINDOW;
    constexpr DWORD windowStyle = WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX | WS_THICKFRAME;

    RECT windowRect{
        .left = 0,
        .top = 0,
        .right = static_cast<LONG>(screenWidth),
        .bottom = static_cast<LONG>(screenHeight),
    };
    if (!AdjustWindowRectExForDpi(&windowRect, windowStyle, FALSE, windowExStyle, GetDpiForSystem())) {
        std::cout << "AdjustWindowRectEx() failed: " << '\n' << GetLastError() << '\n';
        exit(-1);
    }

    auto hwnd = CreateWindowEx(windowExStyle, applicationName, applicationName,
                               windowStyle,
                               0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, nullptr,
                               nullptr,
                               hinstance, nullptr);
    if (hwnd == nullptr) {
        std::cout << "CreateWindowEx() failed: " << '\n' << GetLastError() << '\n';
        exit(-1);
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    ShowCursor(true);
    return hwnd;
}

}
