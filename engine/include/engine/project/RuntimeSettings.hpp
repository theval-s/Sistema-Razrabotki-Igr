#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <engine/engine_export.hpp>

namespace engine {

    struct RuntimeSettings {
        static constexpr const char* FileName = "game.json";
        std::string title; // empty -> project name
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        bool fullscreen = false; // borderless
        bool captureMouse = false; // hide and capture cursor while window has focus
    };

    [[nodiscard]] ENGINE_API bool ParseRuntimeSettings(std::string_view json, RuntimeSettings& value, std::string& error);
    [[nodiscard]] ENGINE_API bool SerializeRuntimeSettings(const RuntimeSettings& value, std::string& json, std::string& error);

}  // namespace engine