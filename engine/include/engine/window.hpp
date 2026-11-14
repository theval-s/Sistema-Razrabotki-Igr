#pragma once
#include <cstdint>
#include <functional>
#include <windows.h>

#include <engine/engine_export.hpp>

namespace engine {


using WindowResizedCallback = void(int newWidth, int newHeight);
using InputMessageHandler = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT&)>;
/// Called by the window procedure for both sent and queued messages. Return true
/// to supply the result and bypass default handling. Empty detaches the listener.
ENGINE_API void SetInputMessageHandler(HWND window, InputMessageHandler handler);
/// For custom window procedures: call first, return `result` when it returns true.
ENGINE_API bool DispatchInputMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);

//Some shitty window init, rewrite
//only one window could be handled
ENGINE_API HWND makeWindow(std::uint32_t screenWidth, uint32_t screenHeight, WindowResizedCallback * resizedCallback);

}
