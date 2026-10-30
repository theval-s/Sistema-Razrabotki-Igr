#pragma once

#include <filesystem>
#include <string>

#include <editor/editor_core_export.hpp>

class QWidget;

namespace engine {
    class Engine;
    struct Project;
}

namespace editor {

    /// Creates a Lua script with a unique filename and fresh metadata.
    /// Returns the actual source path through `created`.
    [[nodiscard]] EDITOR_CORE_API bool createLuaScript(
        engine::Engine& engine,
        const std::filesystem::path& requested,
        std::filesystem::path& created,
        std::string& error);

    /// Asks for a destination and reports creation errors.
    [[nodiscard]] EDITOR_CORE_API bool createLuaScriptInteractive(
        QWidget* parent,
        engine::Engine& engine,
        const engine::Project& project);

}  // namespace editor