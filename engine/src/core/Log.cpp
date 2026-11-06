#include <engine/core/Log.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace engine::log {

const std::shared_ptr<spdlog::sinks::dist_sink_mt>& Sinks() {
    static const std::shared_ptr<spdlog::sinks::dist_sink_mt> sinks = [] {
        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        dist->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("", dist));
        return dist;
    }();
    return sinks;
}

spdlog::logger& Channel(const std::string_view name) {
    // Kept here as well as in spdlog's registry, so spdlog::drop_all() can't free a logger
    // whose reference an accessor holds.
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<spdlog::logger>, std::less<>> channels;

    std::lock_guard lock(mutex);
    if (const auto it = channels.find(name); it != channels.end()) return *it->second;
    auto logger = std::make_shared<spdlog::logger>(std::string(name), Sinks());
    // The global level, flush level and pattern, and registration by name.
    spdlog::initialize_logger(logger);
    return *channels.emplace(std::string(name), std::move(logger)).first->second;
}

}  // namespace engine::log
