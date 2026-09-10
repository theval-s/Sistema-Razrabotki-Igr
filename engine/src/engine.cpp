#include <engine/engine.hpp>

#include <spdlog/spdlog.h>

namespace engine {

struct Engine::Impl {
    bool running = false;
    double elapsed_seconds = 0.0;
};

std::string_view version() noexcept {
    return "0.1.0";
}

Engine::Engine() : impl_(std::make_unique<Impl>()) {
    spdlog::info("aboba");
}

// Defined here, not in the header: the destructor of unique_ptr<Impl> needs the
// complete type, and only this translation unit has it.
Engine::~Engine() = default;

Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;
}  // namespace engine
