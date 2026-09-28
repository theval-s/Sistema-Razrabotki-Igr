#include <engine/engine.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include <TaskScheduler.h>
#include <spdlog/spdlog.h>

namespace engine {

//TODO: Log system 

struct Engine::Impl {
    // Struct to store subsystems
    struct Entry {
        entt::id_type type;
        // Owned copy: the view passed in may point into the host module's memory.
        std::string typeName;
        std::unique_ptr<Subsystem> subsystem;
    };

    explicit Impl(EngineConfig engineConfig) : config(std::move(engineConfig)) {}

    EngineConfig config;
    // Registration order == Initialize/Update/Render order. Accessed by index
    // everywhere so subsystems may register more subsystems while running.
    std::vector<Entry> subsystems;
    std::size_t initializedCount = 0;
    bool initialized = false;

    bool exitRequested = false;
    bool paused = false;

    FrameContext frame{};
    std::uint64_t nextFrameIndex = 0;
    std::chrono::steady_clock::time_point lastTickTime{};

    entt::dispatcher events;
    Scene scene;
    enki::TaskScheduler scheduler;
};

std::string_view version() noexcept {
    return "0.1.0";
}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

// Defined here, not in the header: the destructor of unique_ptr<Impl> needs the
// complete type, and only this translation unit has it.
Engine::~Engine() {
    Shutdown();
}

Subsystem* Engine::RegisterSubsystem(const entt::id_type type, const std::string_view typeName,
                                     std::unique_ptr<Subsystem> subsystem) {
    const auto existing = std::ranges::find(impl_->subsystems, type, &Impl::Entry::type); //or using <algorithm>
    if (existing != impl_->subsystems.end()) {
        if (existing->typeName == typeName) {
            spdlog::error("Engine: subsystem '{}' is already registered", typeName);
        } else {
            spdlog::error("Engine: type hash collision between subsystems '{}' and '{}'", existing->typeName, typeName);
        }
        return nullptr;
    }
    subsystem->engine_ = this;
    impl_->subsystems.push_back({type, std::string(typeName), std::move(subsystem)});
    Subsystem* added = impl_->subsystems.back().subsystem.get();

    // Late registration: initialize right away. While Initialize() is still
    // walking the list the new entry is simply picked up by its loop instead.
    const bool allOthersInitialized = impl_->initializedCount + 1 == impl_->subsystems.size();
    if (impl_->initialized && allOthersInitialized) {
        spdlog::info("Initializing subsystem '{}'", added->GetName());
        if (!added->Initialize()) {
            spdlog::error("Subsystem '{}' failed to initialize", added->GetName());
            impl_->subsystems.pop_back();
            return nullptr;
        }
        ++impl_->initializedCount;
    }
    return added;
}

Subsystem* Engine::FindSubsystem(const entt::id_type type, const std::string_view typeName) const noexcept {
    const auto it = std::ranges::find(impl_->subsystems, type, &Impl::Entry::type);
    // A name mismatch means a hash collision with a different type: not ours.
    return it != impl_->subsystems.end() && it->typeName == typeName ? it->subsystem.get() : nullptr;
}

bool Engine::Initialize() {
    if (impl_->initialized) {
        return true;
    }
    spdlog::info("Engine {} initializing '{}'", version(), impl_->config.applicationName);

    // Up before any subsystem, so they can already schedule work in Initialize().
    if (impl_->config.taskThreads == 0) {
        impl_->scheduler.Initialize();
    } else {
        enki::TaskSchedulerConfig schedulerConfig;
        schedulerConfig.numTaskThreadsToCreate = impl_->config.taskThreads;
        impl_->scheduler.Initialize(schedulerConfig);
    }
    spdlog::info("Task scheduler running with {} threads", impl_->scheduler.GetNumTaskThreads());

    // Size is re-read every iteration: a subsystem may register others from its
    // Initialize(), those get initialized by this same loop.
    impl_->initialized = true;
    for (; impl_->initializedCount < impl_->subsystems.size(); ++impl_->initializedCount) {
        Subsystem& subsystem = *impl_->subsystems[impl_->initializedCount].subsystem;
        spdlog::info("Initializing subsystem '{}'", subsystem.GetName());
        if (!subsystem.Initialize()) {
            spdlog::error("Subsystem '{}' failed to initialize, shutting the engine down", subsystem.GetName());
            // Unwind whatever did come up, so a failed start leaves no half-initialized systems.
            Shutdown();
            return false;
        }
    }

    impl_->exitRequested = false;
    impl_->lastTickTime = std::chrono::steady_clock::now();
    return true;
}

void Engine::Shutdown() {
    if (!impl_->initialized) {
        return;
    }
    spdlog::info("Engine shutting down");

    while (impl_->initializedCount > 0) {
        impl_->subsystems[--impl_->initializedCount].subsystem->Shutdown();
    }
    impl_->scheduler.WaitforAllAndShutdown();
    impl_->initialized = false;
}

bool Engine::IsInitialized() const noexcept {
    return impl_->initialized;
}

void Engine::Tick() {
    if (!impl_->initialized) {
        spdlog::error("Engine::Tick() called before Initialize()");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    double realDelta = 0.0;
    if (impl_->nextFrameIndex > 0) {
        realDelta = std::chrono::duration<double>(now - impl_->lastTickTime).count();
        realDelta = std::clamp(realDelta, 0.0, impl_->config.maxDeltaSeconds);
    }
    impl_->lastTickTime = now;

    FrameContext& frame = impl_->frame;
    frame.frameIndex = impl_->nextFrameIndex++;
    frame.paused = impl_->paused;
    frame.realDeltaSeconds = realDelta;
    frame.realTimeSeconds += realDelta;
    frame.deltaSeconds = impl_->paused ? 0.0 : realDelta;
    frame.simulationTimeSeconds += frame.deltaSeconds;

    // Deliver events queued during the previous frame (or from outside Tick).
    impl_->events.update();

    // Copy: a subsystem reacting to the frame must not see it change under it.
    const FrameContext current = frame;
    for (std::size_t i = 0; i < impl_->subsystems.size(); ++i) {
        impl_->subsystems[i].subsystem->Update(current);
    }
    for (std::size_t i = 0; i < impl_->subsystems.size(); ++i) {
        impl_->subsystems[i].subsystem->Render(current);
    }
}

bool Engine::Run() {
    if (!Initialize()) {
        return false;
    }
    while (!impl_->exitRequested) {
        Tick();
    }
    return true;
}

void Engine::RequestExit() {
    if (impl_->exitRequested) {
        return;
    }
    impl_->exitRequested = true;
    impl_->events.trigger(ExitRequestedEvent{});
}

bool Engine::IsExitRequested() const noexcept {
    return impl_->exitRequested;
}

void Engine::SetPaused(const bool paused) {
    if (impl_->paused == paused) {
        return;
    }
    impl_->paused = paused;
    impl_->events.trigger(PauseChangedEvent{paused});
}

bool Engine::IsPaused() const noexcept {
    return impl_->paused;
}

void Engine::SetSurface(const NativeSurface& surface) {
    impl_->config.surface = surface;
    impl_->events.trigger(SurfaceChangedEvent{surface});
}

void Engine::ResizeSurface(const std::uint32_t width, const std::uint32_t height) {
    NativeSurface surface = impl_->config.surface;
    if (surface.width == width && surface.height == height) {
        return;
    }
    surface.width = width;
    surface.height = height;
    SetSurface(surface);
}

const NativeSurface& Engine::GetSurface() const noexcept {
    return impl_->config.surface;
}

const EngineConfig& Engine::GetConfig() const noexcept {
    return impl_->config;
}

const FrameContext& Engine::GetFrame() const noexcept {
    return impl_->frame;
}

entt::dispatcher& Engine::GetEvents() noexcept {
    return impl_->events;
}

Scene& Engine::GetScene() noexcept {
    return impl_->scene;
}

enki::TaskScheduler& Engine::GetTaskScheduler() noexcept {
    return impl_->scheduler;
}

}  // namespace engine
