#include <engine/engine.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <vector>

#include <TaskScheduler.h>
#include <spdlog/spdlog.h>

#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/ResourceID.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>

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
    // Kernel state, so it is reachable before any subsystem is registered.
    std::unique_ptr<AssetSystem> assets;
    std::unique_ptr<AssetSystem> sceneAssets;
};

std::string_view version() noexcept {
    return "0.1.0";
}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
    // Part of the kernel, so it exists from the start: subsystems reach it from their
    // Initialize() regardless of registration order.
    impl_->assets = std::make_unique<AssetSystem>(*this);
    impl_->sceneAssets = std::make_unique<AssetSystem>(*this);
}

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

    // Up before the subsystems: they already request assets from their Initialize().
    impl_->assets->Initialize();
    impl_->sceneAssets->Initialize();

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
    // After the subsystems (they may still read assets in their Shutdown()), before the
    // scheduler goes away (in-flight loads have to be waited for).
    impl_->sceneAssets->UnloadAll();
    impl_->sceneAssets->Shutdown();
    impl_->assets->Shutdown();
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
    // Finished background loads become usable before anything can ask for them this frame.
    impl_->assets->Update(current);
    impl_->sceneAssets->Update(current);
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

bool Engine::LoadScene(const std::filesystem::path& file, std::string& error) {
    if (!IsInitialized()) { error = "engine must be initialized before loading a scene"; return false; }
    const auto absolute = AssetPathResolver::ToAbsolute(file);
    std::ifstream input(absolute, std::ios::binary);
    if (!input) { error = "cannot open scene file: " + absolute.string(); return false; }
    const std::string text(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    SceneDocument document;
    if (!ParseSceneDocument(text, document, error)) return false;

    if (auto* rendering = GetSubsystem<RenderingSubsystem>()) rendering->GetWorld().sceneItems.clear();
    impl_->scene.Clear();
    impl_->sceneAssets->UnloadAll();
    PopulateScene(document, *impl_->sceneAssets, impl_->scene);
    error.clear();
    return true;
}

bool Engine::LoadScene(const AssetReference& reference, std::string& error) {
    const auto* entry = impl_->sceneAssets->GetCatalog().Find(reference.id, CatalogAssetKind::Scene);
    if (!entry) {
        error = "missing scene UUID: " + reference.id + " (hint: " + reference.path + ")";
        return false;
    }
    if (NormalizeName(reference.path) != NormalizeName(entry->path))
        spdlog::warn("Scene UUID '{}' moved from '{}' to '{}'", reference.id, reference.path, entry->path);
    return LoadScene(entry->source, error);
}

bool Engine::SaveScene(const std::filesystem::path& file, std::string& error) {
    SceneDocument document;
    if (!CaptureSceneDocument(impl_->scene, *impl_->sceneAssets, document, error)) return false;
    std::string text;
    if (!SerializeSceneDocument(document, text, error)) return false;
    const auto absolute = AssetPathResolver::ToAbsolute(file);
    auto temporary = absolute;
    temporary += ".tmp." + NewUuid();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << text << '\n')) {
            error = "cannot write scene file: " + temporary.string();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, absolute, ec);
    if (ec) {
        error = "cannot replace scene file: " + absolute.string() + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    error.clear();
    return true;
}

AssetSystem& Engine::GetAssets() noexcept {
    return *impl_->assets;
}

AssetSystem& Engine::GetSceneAssets() noexcept {
    return *impl_->sceneAssets;
}

enki::TaskScheduler& Engine::GetTaskScheduler() noexcept {
    return impl_->scheduler;
}

}  // namespace engine
