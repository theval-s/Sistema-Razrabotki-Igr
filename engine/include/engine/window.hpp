#pragma once
#include <cstdint>
#include <windows.h>

#include <engine/engine_export.hpp>

namespace engine {

//Some shitty window init, rewrite
ENGINE_API HWND makeWindow(std::uint32_t screenWidth, uint32_t screenHeight);

}
