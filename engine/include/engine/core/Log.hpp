#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>

#include <engine/engine_export.hpp>

// Logging by channel: each part of the engine logs through its own named spdlog logger,
// e.g. log::Lua().error("{}", message), so a log view can tell them apart and filter them.
namespace engine::log {

/// Where every channel writes: add and remove sinks here, not on a logger, which only
/// copies its sinks when created. Starts out with the console. The first call also points
/// spdlog's default logger here, so plain spdlog::info() lands too, without a channel name.
[[nodiscard]] ENGINE_API const std::shared_ptr<spdlog::sinks::dist_sink_mt>& Sinks();

ENGINE_API bool AddFileSink(const std::filesystem::path& file, std::size_t maxBytes = 5 * 1024 * 1024, std::size_t files = 3);

/// The logger for channel `name`, created on first use. Channels are registered with
/// spdlog, so spdlog::set_level() and spdlog::flush_on() reach them. Takes a lock: look a
/// channel up once and keep the reference, as the accessors below do.
[[nodiscard]] ENGINE_API spdlog::logger& Channel(std::string_view name);

// The engine's channels. Hosts add their own through Channel().

inline spdlog::logger& Engine() {
    static spdlog::logger& logger = Channel("Engine");
    return logger;
}

inline spdlog::logger& Render() {
    static spdlog::logger& logger = Channel("Render");
    return logger;
}

inline spdlog::logger& Ui() {
    static spdlog::logger& logger = Channel("UI");
    return logger;
}

inline spdlog::logger& Assets() {
    static spdlog::logger& logger = Channel("Assets");
    return logger;
}

inline spdlog::logger& Scene() {
    static spdlog::logger& logger = Channel("Scene");
    return logger;
}

inline spdlog::logger& Physics() {
    static spdlog::logger& logger = Channel("Physics");
    return logger;
}

/// The Lua runtime itself: script errors, property mismatches.
inline spdlog::logger& Lua() {
    static spdlog::logger& logger = Channel("Lua");
    return logger;
}

/// The game's own messages: Log.info() and friends in scripts, and the host's.
inline spdlog::logger& Game() {
    static spdlog::logger& logger = Channel("Game");
    return logger;
}

}  // namespace engine::log
