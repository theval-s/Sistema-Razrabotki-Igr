#include <engine/engine.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <TaskScheduler.h>
#include <engine/core/Log.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/ResourceID.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/navigation/NavigationMesh.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/GameDocument.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>

namespace engine {

//TODO: Log system 


struct Engine::Impl {
    struct SceneRuntimeResources {
        std::unordered_map<entt::entity, MaterialInstanceHandle> overrides;

        [[nodiscard]] MaterialInstanceHandle Find(entt::entity entity) const {
            const auto it = overrides.find(entity);
            return it == overrides.end() ? MaterialInstanceHandle{} : it->second;
        }
        void Release(entt::entity entity, RenderingSubsystem* rendering) {
            const auto it = overrides.find(entity);
            if (it == overrides.end()) return;
            if (rendering) rendering->GetResourceManager().DestroyMaterialInstance(it->second);
            overrides.erase(it);
        }
    };

    // Struct to store subsystems
    struct Entry {
        entt::id_type type;
        // Owned copy: the view passed in may point into the host module's memory.
        std::string typeName;
        std::unique_ptr<Subsystem> subsystem;
    };

    explicit Impl(EngineConfig engineConfig) : config(std::move(engineConfig)) {}

    EngineConfig config;
    std::unordered_map<EntityUuid, NavigationMesh, EntityUuidHash> navigation;
    // Registration order == Initialize/Update/Render order. Accessed by index
    // everywhere so subsystems may register more subsystems while running.
    std::vector<Entry> subsystems;
    std::size_t initializedCount = 0;
    bool initialized = false;

    bool exitRequested = false;
    bool paused = false;

    FrameContext frame{};
    double fixedAccumulator = 0.0;
    FixedUpdateContext fixedStep{};
    std::uint64_t simulationRevision = 0;
    std::uint64_t nextFrameIndex = 0;
    std::chrono::steady_clock::time_point lastTickTime{};
    std::chrono::steady_clock::time_point lastTickEnd{};
    std::chrono::steady_clock::time_point lastBacklogWarning{};

    entt::dispatcher events;
    // GPU clones belong to the active scene runtime, never to serializable components.
    SceneRuntimeResources sceneRuntime;
    Scene scene;
    SceneDocument sceneBaseline;
    AssetReference sceneReference;
    bool hasSceneReference = false;
    bool deferControllerActivation = false;
    bool editorMode = false;
    bool transitioning = false;
    bool inCallbacks = false;
    bool standalonePlaying = false;
    bool restoringGame = false;
    bool savingGame = false;
    bool stoppingSession = false;
    std::filesystem::path sessionRoot;
    std::filesystem::path saveDirectory;
    std::vector<AutoloadDefinition> autoloads;
    struct GameRequest {
        std::filesystem::path file;
        bool load;
        std::shared_ptr<SaveGameRequest> completion;
    };
    std::deque<GameRequest> gameRequests;
    struct PlaySnapshot {
        std::unordered_map<EntityUuid, NavigationMesh, EntityUuidHash> navigation;
        SceneDocument scene, baseline;
        AssetReference reference;
        bool hasReference = false;
    };
    std::optional<PlaySnapshot> playSnapshot;
    std::optional<std::filesystem::path> queuedScene;
    enki::TaskScheduler scheduler;
    // Kernel state, so it is reachable before any subsystem is registered.
    std::unique_ptr<AssetSystem> assets;
    std::unique_ptr<AssetSystem> sceneAssets;
};

std::string_view version() noexcept {
    return "0.3.1";
}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
    // Part of the kernel, so it exists from the start: subsystems reach it from their
    // Initialize() regardless of registration order.
    impl_->assets = std::make_unique<AssetSystem>(*this);
    impl_->sceneAssets = std::make_unique<AssetSystem>(*this);
    [[maybe_unused]] const auto observer = impl_->scene.AddLifecycleObserver([this](Scene::LifecycleEvent event, entt::entity entity, entt::id_type component) {
        if (event != Scene::LifecycleEvent::EntityDestroying &&
            component != entt::type_hash<MeshRenderer>::value()) return;
        impl_->sceneRuntime.Release(entity, GetSubsystem<RenderingSubsystem>());
    });
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
            log::Engine().error("Subsystem '{}' is already registered", typeName);
        } else {
            log::Engine().error("Type hash collision between subsystems '{}' and '{}'", existing->typeName, typeName);
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
        log::Engine().info("Initializing subsystem '{}'", added->GetName());
        if (!added->Initialize()) {
            log::Engine().error("Subsystem '{}' failed to initialize", added->GetName());
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
    if (!std::isfinite(impl_->config.fixedStepSeconds) || impl_->config.fixedStepSeconds <= 0 ||
        impl_->config.maxFixedStepsPerFrame == 0) {
        log::Engine().error("Invalid fixed update configuration");
        return false;
    }
    impl_->fixedStep.deltaSeconds = impl_->config.fixedStepSeconds;
    log::Engine().info("Engine {} initializing '{}'", version(), impl_->config.applicationName);

    // Up before any subsystem, so they can already schedule work in Initialize().
    if (impl_->config.taskThreads == 0) {
        impl_->scheduler.Initialize();
    } else {
        enki::TaskSchedulerConfig schedulerConfig;
        schedulerConfig.numTaskThreadsToCreate = impl_->config.taskThreads;
        impl_->scheduler.Initialize(schedulerConfig);
    }
    log::Engine().info("Task scheduler running with {} threads", impl_->scheduler.GetNumTaskThreads());

    // Up before the subsystems: they already request assets from their Initialize().
    impl_->assets->Initialize();
    impl_->sceneAssets->Initialize();

    // Size is re-read every iteration: a subsystem may register others from its
    // Initialize(), those get initialized by this same loop.
    impl_->initialized = true;
    for (; impl_->initializedCount < impl_->subsystems.size(); ++impl_->initializedCount) {
        Subsystem& subsystem = *impl_->subsystems[impl_->initializedCount].subsystem;
        log::Engine().info("Initializing subsystem '{}'", subsystem.GetName());
        if (!subsystem.Initialize()) {
            log::Engine().error("Subsystem '{}' failed to initialize, shutting the engine down", subsystem.GetName());
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
    log::Engine().info("Engine shutting down");
    StopPlay();
    StopGame();
    CancelGameRequests();
    ResetSimulation();

    impl_->scene.SetControllersActive(false);
    impl_->scene.Clear();

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
        log::Engine().error("Engine::Tick() called before Initialize()");
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
    frame.deltaSeconds = (impl_->paused || (impl_->editorMode && !IsPlaying())) ? 0.0 : realDelta;
    frame.simulationTimeSeconds += frame.deltaSeconds;

    // Deliver events queued during the previous frame (or from outside Tick).
    impl_->inCallbacks = true;
    impl_->events.update();
    impl_->inCallbacks = false;

    // Copy: a subsystem reacting to the frame must not see it change under it.
    const FrameContext current = frame;
    const auto frameRevision = impl_->simulationRevision;
    impl_->inCallbacks = true;
    // Finished background loads become usable before anything can ask for them this frame.
    impl_->assets->Update(current);
    impl_->sceneAssets->Update(current);
    for (std::size_t i = 0; i < impl_->subsystems.size(); ++i) {
        impl_->subsystems[i].subsystem->Update(current);
    }
    impl_->scene.UpdateControllers(current.deltaSeconds);
    impl_->inCallbacks = false;
    if (impl_->queuedScene) {
        const auto file = std::move(*impl_->queuedScene);
        impl_->queuedScene.reset();
        std::string error;
        if (!LoadScene(file, error) || !error.empty()) log::Engine().error("Deferred scene load: {}", error);
    }
    const auto revision = impl_->simulationRevision;
    using Milliseconds = std::chrono::duration<double, std::milli>;
    const auto fixedStart = std::chrono::steady_clock::now();
    double crowdMilliseconds = 0;
    bool discardedBacklog = false;
    // A newly committed scene starts accumulating time on the next frame.
    if (!impl_->paused && (!impl_->editorMode || IsPlaying()) && revision == frameRevision) {
        impl_->fixedAccumulator += current.deltaSeconds;
        std::uint32_t steps = 0;
        while (impl_->fixedAccumulator >= impl_->config.fixedStepSeconds &&
               steps < impl_->config.maxFixedStepsPerFrame) {
            impl_->fixedAccumulator -= impl_->config.fixedStepSeconds;
            impl_->fixedStep.simulationTimeSeconds += impl_->config.fixedStepSeconds;
            impl_->inCallbacks = true;
            const auto step = impl_->fixedStep;
            for (std::size_t i = 0; i < impl_->subsystems.size(); ++i)
                impl_->subsystems[i].subsystem->FixedUpdate(step);
            impl_->inCallbacks = false;
            const auto crowdStart = std::chrono::steady_clock::now();
            impl_->scene.ForEach<NavigationRegion>([&](entt::entity region, const NavigationRegion&) {
                if (const auto* mesh = GetNavigationMesh(region))
                    mesh->UpdateCrowd(static_cast<float>(step.deltaSeconds), impl_->scene);
            });
            crowdMilliseconds += Milliseconds(std::chrono::steady_clock::now() - crowdStart).count();
            ++impl_->fixedStep.stepIndex;
            ++steps;
            if (impl_->queuedScene) {
                const auto file = std::move(*impl_->queuedScene);
                impl_->queuedScene.reset();
                std::string error;
                if (!LoadScene(file, error) || !error.empty()) log::Engine().error("Deferred scene load: {}", error);
                if (revision != impl_->simulationRevision) break;
            }
            if (impl_->paused || impl_->exitRequested) break;
        }
        if (impl_->fixedAccumulator >= impl_->config.fixedStepSeconds) {
            impl_->fixedAccumulator = std::fmod(impl_->fixedAccumulator, impl_->config.fixedStepSeconds);
            discardedBacklog = true;
        }
    }
    const auto renderStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < impl_->subsystems.size(); ++i) {
        impl_->subsystems[i].subsystem->LateUpdate(current);
    }
    for (std::size_t i = 0; i < impl_->subsystems.size(); ++i) {
        impl_->subsystems[i].subsystem->Render(current);
    }
    if (auto* scripting = GetSubsystem<ScriptSubsystem>()) scripting->GetRuntime().FlushSceneCommands();
    ProcessGameRequests();
    const auto frameEnd = std::chrono::steady_clock::now();
    const auto previousEnd = std::exchange(impl_->lastTickEnd, frameEnd);
    if (discardedBacklog && frameEnd - impl_->lastBacklogWarning >= std::chrono::seconds(1)) {
        impl_->lastBacklogWarning = frameEnd;
        const double outsideTick = previousEnd.time_since_epoch().count()
            ? Milliseconds(now - previousEnd).count() : 0;
        log::Engine().warn("FixedUpdate catch-up limit reached; discarded simulation backlog "
                           "(between ticks {:.1f} ms, update {:.1f} ms, fixed updates {:.1f} ms "
                           "including crowd {:.1f} ms, late update/render {:.1f} ms)",
            outsideTick, Milliseconds(fixedStart - now).count(),
            Milliseconds(renderStart - fixedStart).count(), crowdMilliseconds,
            Milliseconds(frameEnd - renderStart).count());
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
    impl_->fixedAccumulator = 0;
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

void Engine::SetEditorMode() {
    if (IsPlaying()) return;
    impl_->editorMode = true;
    impl_->paused = true;
    impl_->scene.SetControllerInstantiationEnabled(false);
}

bool Engine::IsEditorMode() const noexcept { return impl_->editorMode; }

bool Engine::IsPlaying() const noexcept { return impl_->playSnapshot.has_value() || impl_->standalonePlaying; }

bool Engine::PlayCurrentScene(const std::filesystem::path& root,
                             std::span<const AutoloadDefinition> autoloads, std::string& error) {
    if (!IsInitialized() || !impl_->editorMode || IsPlaying() || impl_->transitioning || impl_->inCallbacks) {
        error = "Play requires an initialized editor in Edit mode";
        return false;
    }
    auto* scripting = GetSubsystem<ScriptSubsystem>();
    if (!scripting) {
        error = "ScriptSubsystem is not registered";
        return false;
    }
    Impl::PlaySnapshot snapshot;
    if (!CaptureSceneDocument(impl_->scene, *impl_->sceneAssets, snapshot.scene, error)) return false;
    snapshot.navigation = impl_->navigation;
    snapshot.baseline = impl_->sceneBaseline;
    snapshot.reference = impl_->sceneReference;
    snapshot.hasReference = impl_->hasSceneReference;
    impl_->playSnapshot = std::move(snapshot);
    impl_->transitioning = true;
    impl_->sessionRoot = std::filesystem::absolute(root);
    impl_->autoloads.assign(autoloads.begin(), autoloads.end());
    impl_->queuedScene.reset();
    CommitScene(impl_->playSnapshot->scene);
    impl_->navigation = impl_->playSnapshot->navigation;
    scripting->GetRuntime().SetLevelLoader([this](std::string_view name, std::string& failure) {
        return RequestOpenLevel(name, failure);
    });
    if (!scripting->BeginPlay(root, autoloads, error)) {
        impl_->transitioning = false;
        StopPlay();
        return false;
    }
    scripting->StartSceneScripts(impl_->scene);
    impl_->scene.SetControllerInstantiationEnabled(true);
    impl_->scene.SetControllersActive(true);
    impl_->frame.simulationTimeSeconds = 0;
    impl_->lastTickTime = std::chrono::steady_clock::now();
    impl_->transitioning = false;
    SetPaused(false);
    error.clear();
    return true;
}

void Engine::StopPlay() {
    if (!impl_->playSnapshot || impl_->transitioning || impl_->inCallbacks) return;
    impl_->transitioning = true;
    impl_->stoppingSession = true;
    CancelGameRequests();
    impl_->queuedScene.reset();
    if (auto* scripting = GetSubsystem<ScriptSubsystem>()) {
        scripting->EndPlay();
        scripting->GetRuntime().SetLevelLoader({});
    }
    CancelGameRequests();
    impl_->queuedScene.reset();
    CommitScene(impl_->playSnapshot->scene);
    impl_->navigation = impl_->playSnapshot->navigation;
    // Reset after both Lua and native teardown callbacks have run.
    if (auto* input = GetSubsystem<InputSubsystem>()) input->SetCursorMode(CursorMode::Normal);
    impl_->sceneBaseline = impl_->playSnapshot->baseline;
    impl_->sceneReference = impl_->playSnapshot->reference;
    impl_->hasSceneReference = impl_->playSnapshot->hasReference;
    for (const auto& entry : impl_->navigation) entry.second.ClearCrowd();
    impl_->playSnapshot.reset();
    impl_->sessionRoot.clear();
    impl_->autoloads.clear();
    impl_->queuedScene.reset();
    impl_->transitioning = false;
    impl_->frame.simulationTimeSeconds = 0;
    SetPaused(true);
    impl_->stoppingSession = false;
}

bool Engine::StartGame(const std::filesystem::path& root,
                       std::span<const AutoloadDefinition> autoloads, std::string& error) {
    if (!IsInitialized() || impl_->editorMode || IsPlaying() || impl_->inCallbacks || impl_->transitioning || !impl_->hasSceneReference) {
        error = "StartGame requires an initialized standalone Engine with a loaded catalog-backed scene and no active session";
        return false;
    }
    auto* scripting = GetSubsystem<ScriptSubsystem>();
    if (!scripting) { error = "ScriptSubsystem is not registered"; return false; }
    impl_->transitioning = true;
    impl_->standalonePlaying = true;
    impl_->sessionRoot = std::filesystem::absolute(root);
    impl_->autoloads.assign(autoloads.begin(), autoloads.end());
    scripting->GetRuntime().SetLevelLoader([this](std::string_view name, std::string& failure) {
        return RequestOpenLevel(name, failure);
    });
    if (!scripting->BeginPlay(root, autoloads, error)) {
        impl_->transitioning = false;
        StopGame();
        return false;
    }
    scripting->StartSceneScripts(impl_->scene);
    impl_->scene.SetControllerInstantiationEnabled(true);
    impl_->scene.SetControllersActive(true);
    ResetSimulation();
    impl_->frame.simulationTimeSeconds = 0;
    impl_->lastTickTime = std::chrono::steady_clock::now();
    impl_->transitioning = false;
    SetPaused(false);
    error.clear();
    return true;
}

void Engine::StopGame() {
    if (!impl_->standalonePlaying || impl_->inCallbacks || impl_->transitioning) return;
    impl_->transitioning = true;
    impl_->stoppingSession = true;
    CancelGameRequests();
    impl_->queuedScene.reset();
    if (auto* scripting = GetSubsystem<ScriptSubsystem>()) {
        scripting->EndPlay();
        scripting->GetRuntime().SetLevelLoader({});
    }
    impl_->scene.SetControllersActive(false);
    ResetSimulation();
    for (const auto& entry : impl_->navigation) entry.second.ClearCrowd();
    if (auto* input = GetSubsystem<InputSubsystem>()) input->SetCursorMode(CursorMode::Normal);
    impl_->standalonePlaying = false;
    CancelGameRequests();
    impl_->queuedScene.reset();
    impl_->sessionRoot.clear();
    impl_->autoloads.clear();
    impl_->transitioning = false;
    impl_->stoppingSession = false;
}

void Engine::SetSaveDirectory(const std::filesystem::path& directory) {
    impl_->saveDirectory = directory.empty() ? directory : std::filesystem::absolute(directory);
}

std::shared_ptr<SaveGameRequest> Engine::QueueGameRequest(std::string_view slot, bool load, std::string& error) {
    if (!IsPlaying() || impl_->savingGame || impl_->restoringGame || impl_->stoppingSession) {
        error = "SaveGame requests require an active session outside persistence hooks"; return {};
    }
    if (slot.empty() || slot.size() > 64 || !std::all_of(slot.begin(), slot.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
    })) { error = "Save slot must contain 1-64 ASCII letters, digits, underscores or hyphens"; return {}; }
    const auto directory = impl_->saveDirectory.empty()
        ? impl_->sessionRoot.parent_path() / ".sri" / "saves" : impl_->saveDirectory;
    auto completion = std::make_shared<SaveGameRequest>();
    impl_->gameRequests.push_back({directory / (std::string(slot) + ".save.json"), load, completion});
    error.clear();
    return completion;
}

std::shared_ptr<SaveGameRequest> Engine::RequestSave(std::string_view slot, std::string& error) {
    return QueueGameRequest(slot, false, error);
}
std::shared_ptr<SaveGameRequest> Engine::RequestLoad(std::string_view slot, std::string& error) {
    return QueueGameRequest(slot, true, error);
}
void Engine::CancelGameRequests() {
    for (auto& request : impl_->gameRequests) {
        request.completion->complete_ = true;
        request.completion->error_ = "Request cancelled because the scripting session ended";
    }
    impl_->gameRequests.clear();
}
void Engine::ProcessGameRequests() {
    // Process only the requests that existed at this boundary. A successful load
    // cancels remaining old-session requests before starting new script callbacks.
    auto remaining = impl_->gameRequests.size();
    while (remaining-- && !impl_->gameRequests.empty()) {
        auto request = std::move(impl_->gameRequests.front());
        impl_->gameRequests.pop_front();
        std::string error;
        bool ok = false;
        try {
            if (!request.load) std::filesystem::create_directories(request.file.parent_path());
            ok = request.load ? LoadGame(request.file, error) : SaveGame(request.file, error);
        } catch (const std::exception& exception) { error = exception.what(); }
        request.completion->success_ = ok;
        request.completion->error_ = std::move(error);
        request.completion->complete_ = true;
    }
}

bool Engine::RequestOpenLevel(std::string_view name, std::string& error) {
    auto fail = [&](std::string message) {
        error = std::move(message);
        log::Engine().error("OpenLevel: {}", error);
        return false;
    };
    if (!IsPlaying() || impl_->stoppingSession) return fail("OpenLevel requires an active play session that is not stopping");
    std::string logical(name);
    if (logical.empty() || logical.find('\\') != std::string::npos || logical.find(':') != std::string::npos)
        return fail("Use a project scene name or game/scenes/name.scene.json");
    for (const auto& part : FromUtf8(logical))
        if (part == ".." || part == ".") return fail("Scene traversal is not allowed");
    if (!logical.starts_with("game/")) logical = (logical.find('/') == std::string::npos ? "game/scenes/" : "game/") + logical;
    if (!logical.ends_with(".scene.json")) logical += ".scene.json";
    const auto* entry = impl_->assets->GetCatalog().FindPath(logical);
    if (!entry || entry->metadata.kind != CatalogAssetKind::Scene) return fail("Scene not found: " + logical);
    std::error_code ec;
    const auto* scripting = GetSubsystem<ScriptSubsystem>();
    if (!scripting || scripting->GetRoot().empty()) return fail("Scripting session is not ready");
    const auto relative = std::filesystem::relative(entry->source, scripting->GetRoot(), ec);
    if (ec || relative.empty() || relative.is_absolute()) return fail("Scene is outside project content");
    for (const auto& part : relative) if (part == "..") return fail("Scene is outside project content");
    std::string text;
    SceneDocument checked;
    if (!ReadTextFile(entry->source, text, error) || !ParseSceneDocument(text, checked, error)) return fail(error);
    impl_->queuedScene = entry->source;
    error.clear();
    return true;
}

void Engine::ResetSimulation() {
    impl_->fixedAccumulator = 0;
    impl_->fixedStep = {0, impl_->config.fixedStepSeconds, 0};
    ++impl_->simulationRevision;
    for (std::size_t i = 0; i < impl_->initializedCount; ++i)
        impl_->subsystems[i].subsystem->ResetSimulation();
}

void Engine::CommitScene(const SceneDocument& document) {
    impl_->navigation.clear();
    ResetSimulation();
    if (auto* scripting = GetSubsystem<ScriptSubsystem>()) {
        scripting->ClearSceneScripts();
        scripting->GetRuntime().ClearEntityInstances();
    }
    if (auto* rendering = GetSubsystem<RenderingSubsystem>()) rendering->GetWorld().sceneItems.clear();
    impl_->scene.SetControllerInstantiationEnabled(false);
    impl_->scene.Clear();
    impl_->sceneAssets->UnloadAll();
    PopulateScene(document, *impl_->sceneAssets, impl_->scene);
    impl_->sceneBaseline = document;
    if (auto* scripting = GetSubsystem<ScriptSubsystem>(); scripting && scripting->IsPlaying() && !impl_->restoringGame)
        scripting->StartSceneScripts(impl_->scene);
    const auto* scripting = GetSubsystem<ScriptSubsystem>();
    const bool standaloneReady = !impl_->editorMode && (!scripting || scripting->IsPlaying());
    if (!impl_->restoringGame && (standaloneReady || (IsPlaying() && !impl_->transitioning))) {
        impl_->scene.SetControllerInstantiationEnabled(true);
        if (!impl_->deferControllerActivation) impl_->scene.SetControllersActive(true);
    }
}

bool Engine::LoadScene(const std::filesystem::path& file, std::string& error) {
    if (impl_->scene.IsInControllerCallback() || impl_->inCallbacks) {
        impl_->queuedScene = file;
        error.clear();
        return true;
    }
    if (!IsInitialized()) { error = "engine must be initialized before loading a scene"; return false; }
    const auto absolute = AssetPathResolver::ToAbsolute(file);
    std::string text;
    if (!ReadTextFile(absolute, text, error)) return false;
    SceneDocument document;
    if (!ParseSceneDocument(text, document, error)) return false;

    std::string diagnostics;
    for (const auto& entity : document.entities) if (entity.controller) {
        std::string detail;
        if (!impl_->scene.ValidateController(*entity.controller, detail))
            diagnostics += entity.name + " (" + entity.id.ToString() + "): " + detail + "\n";
    }

    CommitScene(document);
    if (const auto* entry = impl_->sceneAssets->GetCatalog().FindPath(AssetPathResolver::ToName(absolute));
        entry && entry->metadata.kind == CatalogAssetKind::Scene) {
        impl_->sceneReference = {entry->metadata.id, entry->path};
        impl_->hasSceneReference = true;
    } else impl_->hasSceneReference = false;
    error = std::move(diagnostics);
    return true;
}

bool Engine::LoadScene(const AssetReference& reference, std::string& error) {
    const auto* entry = impl_->sceneAssets->GetCatalog().Find(reference.id, CatalogAssetKind::Scene);
    if (!entry) {
        error = "missing scene UUID: " + reference.id.ToString() + " (hint: " + reference.path + ")";
        return false;
    }
    if (NormalizeName(reference.path) != NormalizeName(entry->path))
        log::Engine().warn("Scene UUID '{}' moved from '{}' to '{}'", reference.id.ToString(), reference.path, entry->path);
    return LoadScene(entry->source, error);
}

bool Engine::SaveScene(const std::filesystem::path& file, std::string& error) {
    SceneDocument document;
    if (!CaptureSceneDocument(impl_->scene, *impl_->sceneAssets, document, error)) return false;
    std::string text;
    if (!SerializeSceneDocument(document, text, error)) return false;
    return WriteFileAtomic(AssetPathResolver::ToAbsolute(file), text + '\n', error);
}

bool Engine::SaveGame(const std::filesystem::path& file, std::string& error) {
    if (impl_->savingGame || impl_->restoringGame) { error = "Persistence operations are not reentrant"; return false; }
    struct SavingScope { bool& active; explicit SavingScope(bool& flag) : active(flag) { active = true; } ~SavingScope() { active = false; } } scope(impl_->savingGame);
    if (impl_->scene.IsInControllerCallback()) { error = "save game outside controller callbacks"; return false; }
    if (!impl_->hasSceneReference) { error = "save game requires a catalog-backed loaded scene"; return false; }
    SceneDocument current;
    if (!CaptureSceneDocument(impl_->scene, *impl_->sceneAssets, current, error, true)) return false;
    GameDocument game;
    game.scene = impl_->sceneReference;
    std::unordered_set<EntityUuid, EntityUuidHash> original, present;
    for (const auto& item : impl_->sceneBaseline.entities) original.insert(item.id);
    for (const auto& item : current.entities) present.insert(item.id);
    for (const auto& id : original) if (!present.contains(id)) game.deleted.push_back(id);
    std::sort(game.deleted.begin(), game.deleted.end());
    for (auto item : current.entities) {
        const auto entity = impl_->scene.FindByUuid(item.id);
        if (!item.persistence) continue;
        if (!original.contains(item.id)) {
            if (impl_->scene.HasUnsupportedComponents(entity)) {
                error = "persistent spawned entity has unsupported components: " + item.id.ToString();
                return false;
            }
            if (item.parent) game.spawnParents[item.id] = *item.parent;
            item.parent.reset();
            game.spawned.entities.push_back(item);
        }
        GameEntityState state;
        state.id = item.id;
        if (item.persistence->transform) state.transform = impl_->scene.GetTransform(entity);
        if (item.persistence->controllerState && item.controller) {
            std::string detail;
            auto data = impl_->scene.CaptureControllerGameState(entity, detail);
            if (!detail.empty()) { error = item.id.ToString() + ": " + detail; return false; }
            if (data) state.controller = GameControllerState{item.controller->type, item.controller->version, *data};
        }
        if (state.transform || state.controller) game.entities.push_back(std::move(state));
    }
    std::unordered_set<EntityUuid, EntityUuidHash> spawnedIds;
    for (const auto& item : game.spawned.entities) spawnedIds.insert(item.id);
    for (const auto& [id, parent] : game.spawnParents)
        if (!original.contains(parent) && !spawnedIds.contains(parent)) {
            error = "persistent spawn " + id.ToString() + " has a non-persistent spawned parent: " + parent.ToString();
            return false;
        }
    if (auto* scripting = GetSubsystem<ScriptSubsystem>(); scripting && scripting->IsPlaying()) {
        if (!scripting->CaptureGameState(game, error)) { scripting->GetRuntime().DiscardSceneCommands(); return false; }
        // Persistence hooks describe state. Structural commands are discarded;
        // they must not change what the already captured snapshot represents.
        scripting->GetRuntime().DiscardSceneCommands();
    }
    std::string text;
    if (!SerializeGameDocument(game, text, error)) return false;
    return WriteFileAtomic(AssetPathResolver::ToAbsolute(file), text + '\n', error);
}

bool Engine::LoadGame(const std::filesystem::path& file, std::string& error) {
    if (impl_->savingGame || impl_->restoringGame || impl_->inCallbacks) { error = "Load game requires a safe engine boundary"; return false; }
    if (!IsInitialized()) { error = "engine must be initialized before loading a save game"; return false; }
    if (impl_->scene.IsInControllerCallback()) { error = "load game outside controller callbacks"; return false; }
    std::string text;
    if (!ReadTextFile(AssetPathResolver::ToAbsolute(file), text, error)) return false;
    GameDocument game;
    if (!ParseGameDocument(text, game, error)) return false;
    const auto* entry = impl_->assets->GetCatalog().Find(game.scene.id, CatalogAssetKind::Scene);
    if (!entry) { error = "missing scene UUID in save game: " + game.scene.id.ToString(); return false; }
    std::string sceneText;
    if (!ReadTextFile(entry->source, sceneText, error)) return false;
    SceneDocument base;
    if (!ParseSceneDocument(sceneText, base, error)) return false;
    std::unordered_set<EntityUuid, EntityUuidHash> authored, spawned, deleted(game.deleted.begin(), game.deleted.end());
    for (const auto& item : base.entities) authored.insert(item.id);
    for (const auto& item : game.spawned.entities) {
        if (authored.contains(item.id)) { error = "spawned entity collides with scene UUID: " + item.id.ToString(); return false; }
        if (!item.persistence) { error = "spawned entity has no persistence policy: " + item.id.ToString(); return false; }
        spawned.insert(item.id);
    }
    for (const auto& id : game.deleted) if (!authored.contains(id)) {
        error = "deleted entity is not in the scene: " + id.ToString(); return false;
    }
    auto removed = [&](EntityUuid id) {
        std::unordered_set<EntityUuid, EntityUuidHash> visited;
        while (visited.insert(id).second) {
            if (deleted.contains(id)) return true;
            if (const auto it = game.spawnParents.find(id); it != game.spawnParents.end()) {
                id = it->second;
            } else {
                const auto found = std::find_if(base.entities.begin(), base.entities.end(), [&](const auto& e) { return e.id == id; });
                if (found == base.entities.end() || !found->parent) break;
                id = *found->parent;
            }
        }
        return false;
    };
    for (const auto& [id, parent] : game.spawnParents)
        if ((!authored.contains(parent) && !spawned.contains(parent)) || removed(parent)) {
            error = "missing spawned parent for " + id.ToString(); return false;
        }
    for (const auto& state : game.entities) {
        if ((!authored.contains(state.id) && !spawned.contains(state.id)) || removed(state.id)) {
            error = "unknown saved entity: " + state.id.ToString(); return false;
        }
        const auto* definition = [&]() -> const SceneEntityDefinition* {
            for (const auto& item : game.spawned.entities) if (item.id == state.id) return &item;
            for (const auto& item : base.entities) if (item.id == state.id) return &item;
            return nullptr;
        }();
        if (!definition->persistence || (state.transform && !definition->persistence->transform) ||
            (state.controller && !definition->persistence->controllerState) ||
            (state.script && !definition->persistence->scriptState)) {
            error = "save-game state is not enabled for " + state.id.ToString(); return false;
        }
        if (state.controller && (!definition->controller ||
            definition->controller->type != state.controller->type ||
            definition->controller->version != state.controller->version)) {
            error = "saved controller type/version mismatch for " + state.id.ToString(); return false;
        }
        if (state.script && (!definition->script || !definition->script->enabled ||
            definition->script->script.id != state.script->asset)) {
            error = "saved script asset mismatch for " + state.id.ToString(); return false;
        }
    }
    // Check parent cycles before touching the live scene.
    for (const auto& [id, parent] : game.spawnParents) {
        std::unordered_set<EntityUuid, EntityUuidHash> seen{id};
        EntityUuid cursor = parent;
        while (spawned.contains(cursor) && game.spawnParents.contains(cursor)) {
            if (!seen.insert(cursor).second) { error = "spawn hierarchy cycle"; return false; }
            cursor = game.spawnParents.at(cursor);
        }
        if (!seen.insert(cursor).second) { error = "spawn hierarchy cycle"; return false; }
    }
    // Validate the composed scene too: save-game transforms/parents can turn a
    // valid authored collider into an unsupported dynamic hierarchy or shear.
    SceneDocument restored = base;
    std::erase_if(restored.entities, [&](const auto& item) { return removed(item.id); });
    restored.entities.insert(restored.entities.end(), game.spawned.entities.begin(), game.spawned.entities.end());
    for (auto& item : restored.entities) {
        if (const auto it = game.spawnParents.find(item.id); it != game.spawnParents.end()) item.parent = it->second;
        for (const auto& state : game.entities)
            if (state.id == item.id && state.transform) item.transform = *state.transform;
    }
    if (restored.activeCamera && removed(*restored.activeCamera)) restored.activeCamera.reset();
    if (restored.directionalLight && removed(*restored.directionalLight)) restored.directionalLight.reset();
    std::string checkedScene;
    if (!SerializeSceneDocument(restored, checkedScene, error)) return false;
    auto* scripting = GetSubsystem<ScriptSubsystem>();
    const bool restoreScripts = scripting && scripting->IsPlaying();
    const bool containsScriptState = !game.autoloads.empty() || std::any_of(game.entities.begin(), game.entities.end(), [](const auto& state) { return state.script.has_value(); });
    if (containsScriptState && !restoreScripts) { error = "Saved Lua state requires an active scripting session"; return false; }
    if (restoreScripts && !scripting->PreflightGameState(game, error)) return false;
    if (restoreScripts) {
        // Copy catalog identity before scene asset teardown. The shared catalog
        // remains available throughout a fresh runtime/session restoration.
        const AssetReference reference = game.scene;
        const auto root = impl_->sessionRoot.empty() ? scripting->GetRoot() : impl_->sessionRoot;
        const auto autoloads = impl_->autoloads;
        impl_->restoringGame = true;
        struct RestoreScope { bool& active; ~RestoreScope() { active = false; } } scope{impl_->restoringGame};
        CancelGameRequests();
        impl_->queuedScene.reset();
        scripting->EndPlay();
        CommitScene(restored);
        if (auto* input = GetSubsystem<InputSubsystem>()) input->SetCursorMode(CursorMode::Normal);
        impl_->sceneBaseline = base;
        impl_->sceneReference = reference;
        impl_->hasSceneReference = true;
        if (!scripting->BeginPlay(root, autoloads, error, true) ||
            !scripting->PrepareSceneScripts(impl_->scene, error)) {
            scripting->EndPlay();
            return false;
        }
        // Both native and Lua restoration happen while controllers are dormant.
        impl_->scene.SetControllersActive(false);
        impl_->scene.SetControllerInstantiationEnabled(true);
        for (const auto& state : game.entities) if (state.controller) {
            if (!impl_->scene.RestoreControllerGameState(impl_->scene.FindByUuid(state.id), state.controller->data, error) || !error.empty()) {
                scripting->EndPlay(); return false;
            }
        }
        if (!scripting->RestoreGameState(game, error)) {
            // Hook errors fail this operation, without faulting the prepared
            // script. Start the fresh scene normally so gameplay can continue.
            const auto failure = error;
            scripting->GetRuntime().DiscardSceneCommands();
            std::string startupError;
            if (!scripting->StartPreparedScripts(startupError)) scripting->EndPlay();
            impl_->scene.SetControllerInstantiationEnabled(true);
            impl_->scene.SetControllersActive(true);
            error = failure;
            return false;
        }
        scripting->GetRuntime().DiscardSceneCommands();
        if (!scripting->StartPreparedScripts(error)) { scripting->EndPlay(); return false; }
        impl_->scene.SetControllerInstantiationEnabled(true);
        impl_->scene.SetControllersActive(true);
        impl_->frame.simulationTimeSeconds = 0;
        impl_->lastTickTime = std::chrono::steady_clock::now();
        error.clear();
        return true;
    }
    impl_->deferControllerActivation = true;
    const bool loaded = LoadScene(entry->source, error);
    impl_->deferControllerActivation = false;
    if (!loaded) return false;
    std::string diagnostics = std::move(error);
    for (const auto& id : game.deleted) {
        if (const auto entity = impl_->scene.FindByUuid(id); entity != entt::null) impl_->scene.DestroyEntity(entity);
    }
    PopulateScene(game.spawned, *impl_->sceneAssets, impl_->scene);
    for (const auto& [id, parent] : game.spawnParents)
        if (!impl_->scene.SetParent(impl_->scene.FindByUuid(id), impl_->scene.FindByUuid(parent)))
            diagnostics += "cannot restore parent for " + id.ToString() + "\n";
    for (const auto& state : game.entities) {
        const auto entity = impl_->scene.FindByUuid(state.id);
        if (entity == entt::null) { diagnostics += "missing saved entity " + state.id.ToString() + "\n"; continue; }
        if (state.transform) impl_->scene.GetTransform(entity) = *state.transform;
        if (state.controller) {
            std::string detail;
            if (!impl_->scene.RestoreControllerGameState(entity, state.controller->data, detail) || !detail.empty())
                diagnostics += state.id.ToString() + ": " + detail + "\n";
        }
    }
    impl_->scene.SetControllersActive(true);
    error = std::move(diagnostics);
    return true;
}

bool Engine::EditEntityMaterial(entt::entity entity, const std::function<void(MaterialParameters&)>& edit,
                                std::string& error) {
    auto* renderer = impl_->scene.TryGetComponent<MeshRenderer>(entity);
    auto* rendering = GetSubsystem<RenderingSubsystem>();
    if (!renderer || !rendering || !impl_->sceneAssets->Peek(renderer->materialInstance)) {
        error = "entity has no ready scene material instance";
        return false;
    }
    auto& resources = rendering->GetResourceManager();
    const auto base = impl_->sceneAssets->Peek(renderer->materialInstance)->instance;
    const auto override = GetEntityMaterialOverride(entity);
    const auto* source = resources.TryGetMaterialInstance(override ? override : base);
    if (!source) { error = "material instance is no longer available"; return false; }
    const auto* material = resources.TryGetMaterial(source->material);
    if (!material) { error = "material is no longer available"; return false; }
    MaterialParameters parameters = ResolveMaterialParameters(*material, *source);
    edit(parameters);
    if (!override) {
        const auto clone = resources.CreateMaterialInstance(source->material);
        if (!clone) { error = "cannot clone material instance"; return false; }
        if (!resources.SetMaterialInstanceParameters(clone, parameters)) {
            resources.DestroyMaterialInstance(clone);
            error = "cannot set clone parameters";
            return false;
        }
        impl_->sceneRuntime.overrides[entity] = clone;
    } else if (!resources.SetMaterialInstanceParameters(override, parameters)) {
        error = "cannot set clone parameters";
        return false;
    }
    error.clear();
    return true;
}

bool Engine::SetEntityAlbedoTexture(entt::entity entity, AssetHandle<TextureAsset> texture, std::string& error) {
    const auto ready = impl_->sceneAssets->Peek(texture);
    if (!ready) { error = "texture asset is not ready in this scene"; return false; }
    return EditEntityMaterial(entity, [handle = ready->texture](MaterialParameters& parameters) {
        parameters.albedoTexture = handle;
    }, error);
}

MaterialInstanceHandle Engine::GetEntityMaterialOverride(entt::entity entity) const {
    return impl_->sceneRuntime.Find(entity);
}

void Engine::ClearEntityMaterialOverride(entt::entity entity) {
    impl_->sceneRuntime.Release(entity, GetSubsystem<RenderingSubsystem>());
}

AssetSystem& Engine::GetAssets() noexcept {
    return *impl_->assets;
}

const NavigationMesh* Engine::GetNavigationMesh(entt::entity region) const {
    const auto& scene = impl_->scene;
    if (!scene.HasComponent<NavigationRegion>(region)) return nullptr;
    const auto found = impl_->navigation.find(scene.GetId(region));
    return found == impl_->navigation.end() ? nullptr : &found->second;
}

void Engine::DrawNavigationMeshes(DebugDraw& draw) const {
    impl_->scene.ForEach<NavigationRegion>([&](entt::entity region, const NavigationRegion&) {
        if (const auto* mesh = GetNavigationMesh(region)) mesh->Draw(draw);
    });
}

std::vector<DirectX::SimpleMath::Vector3> Engine::FindNavigationPath(
    DirectX::SimpleMath::Vector3 start, DirectX::SimpleMath::Vector3 goal, entt::entity region) const {
    if (region != entt::null) {
        const auto* mesh = GetNavigationMesh(region);
        return mesh ? mesh->FindPath(start, goal) : std::vector<DirectX::SimpleMath::Vector3>{};
    }

    std::vector<DirectX::SimpleMath::Vector3> best;
    float bestLength = std::numeric_limits<float>::infinity();
    EntityUuid bestId{};
    impl_->scene.ForEach<NavigationRegion>([&](entt::entity candidate, const NavigationRegion&) {
        const auto* mesh = GetNavigationMesh(candidate);
        if (!mesh) return;
        auto path = mesh->FindPath(start, goal);
        if (path.empty()) return;
        float length = (path.front() - start).Length() + (goal - path.back()).Length();
        for (std::size_t i = 1; i < path.size(); ++i) length += (path[i] - path[i - 1]).Length();
        const auto id = impl_->scene.GetId(candidate);
        if (length < bestLength || (length == bestLength && id < bestId)) {
            bestLength = length;
            bestId = id;
            best = std::move(path);
        }
    });
    return best;
}

bool Engine::BuildNavigation(entt::entity entity, std::string& error) {
    using DirectX::SimpleMath::Vector3;
    error.clear();
    const Scene& scene = GetScene();
    const auto* region = scene.TryGetComponent<NavigationRegion>(entity);
    if (!region) {
        error = "Select an entity with a Navigation Region";
        return false;
    }
    const auto center = scene.GetWorldMatrix(entity).Translation();
    const auto minimum = center - region->halfExtents;
    const auto maximum = center + region->halfExtents;
    std::vector<Vector3> vertices;
    std::vector<int> indices;
    auto& assets = GetSceneAssets();
    auto* physics = GetSubsystem<PhysicsSubsystem>();
    constexpr std::size_t limit = 3000000;
    scene.ForEach<NavigationSource>([&](entt::entity source, const NavigationSource& settings) {
        if (!error.empty()) return;
        const auto* body = scene.TryGetComponent<RigidBody>(source);
        const auto* collider = scene.TryGetComponent<Collider>(source);
        if ((body && body->motionType != MotionType::Static) || (collider && collider->isTrigger)) return;

        if (settings.useCollider) {
            const auto first = vertices.size();
            if (!physics || !physics->CollectColliderTriangles(source, vertices, error)) {
                if (error.empty()) error = "A Navigation Source requires a valid static collider";
                return;
            }
            if (vertices.size() > limit || vertices.size() - first > limit - indices.size()) {
                error = "Too many navigation triangles";
                return;
            }
            for (auto i = first; i < vertices.size(); ++i) indices.push_back(static_cast<int>(i));
            return;
        }
        const auto* renderer = scene.TryGetComponent<MeshRenderer>(source);
        if (!renderer) {
            error = "A Navigation Source requires a Mesh Renderer";
            return;
        }
        const auto mesh = assets.Peek(renderer->mesh);
        if (!mesh || !mesh->geometry) {
            error = "A navigation source mesh is not loaded";
            return;
        }
        const auto& geometry = *mesh->geometry;
        if (geometry.positions.size() > limit - vertices.size() ||
            geometry.indices.size() > limit - indices.size()) {
            error = "Too many navigation triangles";
            return;
        }
        const int base = static_cast<int>(vertices.size());
        const auto world = scene.GetWorldMatrix(source);
        for (const auto position : geometry.positions)
            vertices.push_back(Vector3::Transform(position, world));
        for (const auto index : geometry.indices)
            indices.push_back(base + static_cast<int>(index));
    });
    if (!error.empty()) return false;
    if (indices.empty()) {
        error = "Add a Navigation Source to the floor and any walls that affect movement";
        return false;
    }
    NavigationMesh mesh;
    if (!mesh.BuildRegion(vertices, indices, region->settings, minimum, maximum)) {
        error = "No walkable mesh was built. Check the region bounds, geometry and agent settings.";
        return false;
    }
    impl_->navigation.insert_or_assign(scene.GetId(entity), mesh);
    return true;
}

AssetSystem& Engine::GetSceneAssets() noexcept {
    return *impl_->sceneAssets;
}

bool Engine::RescanAssetCatalogs(std::string& error) {
    // Same roots, so the same diagnostics: report one of them.
    std::string hostError;
    const bool host = impl_->assets->RescanCatalog(hostError);
    const bool scene = impl_->sceneAssets->RescanCatalog(error);
    if (error.empty()) error = std::move(hostError);
    return host && scene;
}

enki::TaskScheduler& Engine::GetTaskScheduler() noexcept {
    return impl_->scheduler;
}

}  // namespace engine
