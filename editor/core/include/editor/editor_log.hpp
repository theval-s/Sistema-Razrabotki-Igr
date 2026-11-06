#pragma once

#include <engine/core/Log.hpp>

namespace editor {

/// The editor's log channel, beside the engine's (see engine/core/Log.hpp).
inline spdlog::logger& editorLog() {
    static spdlog::logger& logger = engine::log::Channel("Editor");
    return logger;
}

}  // namespace editor
