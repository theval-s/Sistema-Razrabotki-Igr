#pragma once

#include <string>
#include <editor/editor_core_export.hpp>

namespace engine { struct Project; }

namespace editor {
/// Writes editor-only API/name definitions under <project>/.sri/lua and adds that
/// library to LuaLS configuration. Never executes scripts or edits project assets.
[[nodiscard]] EDITOR_CORE_API bool refreshLuaDefinitions(const engine::Project& project, std::string& error);
}
