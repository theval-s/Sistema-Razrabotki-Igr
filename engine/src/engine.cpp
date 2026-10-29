#include <engine/engine.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <set>
#include <optional>
#include <unordered_map>
#include <vector>

#include <TaskScheduler.h>
#include <spdlog/spdlog.h>

#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/ResourceID.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/GameDocument.hpp>

namespace engine {

//TODO: Log system 

namespace {
// path to file, what to write, `what` names the file in error messages.
bool WriteFileReplacing(const std::filesystem::path& file, const std::string& text,
                        std::string_view what, std::string& error) {
    const auto absolute = AssetPathResolver::ToAbsolute(file);
    auto temporary = absolute;
    temporary += ".tmp." + NewUuid();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << text << '\n')) {
            error = "cannot write " + std::string(what) + ": " + ToUtf8(temporary);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, absolute, ec);
    if (ec) {
        error = "cannot replace " + std::string(what) + ": " + ToUtf8(absolute) + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    error.clear();
    return true;
}
} // namespace

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
    // GPU clones belong to the active scene runtime, never to serializable components.
    SceneRuntimeResources sceneRuntime;
    Scene scene;
    SceneDocument sceneBaseline;
    AssetReference sceneReference;
    bool hasSceneReference = false;
    bool deferControllerActivation = false;
    std::optional<std::filesystem::path> queuedScene;
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
    impl_->scene.UpdateControllers(current.deltaSeconds);
    if (impl_->queuedScene) {
        const auto file = std::move(*impl_->queuedScene);
        impl_->queuedScene.reset();
        std::string error;
        if (!LoadScene(file, error) || !error.empty()) spdlog::error("Deferred scene load: {}", error);
    }
    for (std::size_t i = 0; i < impl_->subsystems.size(); ++i) {
        impl_->subsystems[i].subsystem->LateUpdate(current);
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
    if (impl_->scene.IsInControllerCallback()) {
        impl_->queuedScene = file;
        error.clear();
        return true;
    }
    if (!IsInitialized()) { error = "engine must be initialized before loading a scene"; return false; }
    const auto absolute = AssetPathResolver::ToAbsolute(file);
    std::ifstream input(absolute, std::ios::binary);
    if (!input) { error = "cannot open scene file: " + ToUtf8(absolute); return false; }
    const std::string text(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    SceneDocument document;
    if (!ParseSceneDocument(text, document, error)) return false;

    std::string diagnostics;
    for (const auto& entity : document.entities) if (entity.controller) {
        std::string detail;
        if (!impl_->scene.ValidateController(*entity.controller, detail))
            diagnostics += entity.name + " (" + entity.id + "): " + detail + "\n";
    }

    if (auto* rendering = GetSubsystem<RenderingSubsystem>()) rendering->GetWorld().sceneItems.clear();
    impl_->scene.SetControllersActive(false);
    impl_->scene.Clear();
    impl_->sceneAssets->UnloadAll();
    PopulateScene(document, *impl_->sceneAssets, impl_->scene);
    impl_->sceneBaseline = document;
    if (const auto* entry = impl_->sceneAssets->GetCatalog().FindPath(AssetPathResolver::ToName(absolute));
        entry && entry->metadata.kind == CatalogAssetKind::Scene) {
        impl_->sceneReference = {entry->metadata.id, entry->path};
        impl_->hasSceneReference = true;
    } else impl_->hasSceneReference = false;
    if (!impl_->deferControllerActivation) impl_->scene.SetControllersActive(true);
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
        spdlog::warn("Scene UUID '{}' moved from '{}' to '{}'", reference.id.ToString(), reference.path, entry->path);
    return LoadScene(entry->source, error);
}

bool Engine::SaveScene(const std::filesystem::path& file, std::string& error) {
    SceneDocument document;
    if (!CaptureSceneDocument(impl_->scene, *impl_->sceneAssets, document, error)) return false;
    std::string text;
    if (!SerializeSceneDocument(document, text, error)) return false;
    return WriteFileReplacing(file, text, "scene file", error);
}

bool Engine::SaveGame(const std::filesystem::path& file, std::string& error) {
    if (impl_->scene.IsInControllerCallback()) { error = "save game outside controller callbacks"; return false; }
    if (!impl_->hasSceneReference) { error = "save game requires a catalog-backed loaded scene"; return false; }
    SceneDocument current;
    if (!CaptureSceneDocument(impl_->scene, *impl_->sceneAssets, current, error, true)) return false;
    GameDocument game;
    game.scene = impl_->sceneReference;
    std::set<std::string> original, present;
    for (const auto& item : impl_->sceneBaseline.entities) original.insert(item.id);
    for (const auto& item : current.entities) present.insert(item.id);
    for (const auto& id : original) if (!present.contains(id)) game.deleted.push_back(id);
    for (auto item : current.entities) {
        const auto entity = impl_->scene.FindById(item.id);
        if (!item.persistence) continue;
        if (!original.contains(item.id)) {
            if (impl_->scene.HasUnsupportedComponents(entity)) {
                error = "persistent spawned entity has unsupported components: " + item.id;
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
            if (!detail.empty()) { error = item.id + ": " + detail; return false; }
            if (data) state.controller = GameControllerState{item.controller->type, item.controller->version, *data};
        }
        if (state.transform || state.controller) game.entities.push_back(std::move(state));
    }
    std::set<std::string> spawnedIds;
    for (const auto& item : game.spawned.entities) spawnedIds.insert(item.id);
    for (const auto& [id, parent] : game.spawnParents)
        if (!original.contains(parent) && !spawnedIds.contains(parent)) {
            error = "persistent spawn " + id + " has a non-persistent spawned parent: " + parent;
            return false;
        }
    std::string text;
    if (!SerializeGameDocument(game, text, error)) return false;
    return WriteFileReplacing(file, text, "save game", error);
}

bool Engine::LoadGame(const std::filesystem::path& file, std::string& error) {
    if (!IsInitialized()) { error = "engine must be initialized before loading a save game"; return false; }
    if (impl_->scene.IsInControllerCallback()) { error = "load game outside controller callbacks"; return false; }
    std::ifstream input(AssetPathResolver::ToAbsolute(file), std::ios::binary);
    if (!input) { error = "cannot open save game: " + ToUtf8(file); return false; }
    const std::string text(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    GameDocument game;
    if (!ParseGameDocument(text, game, error)) return false;
    const auto* entry = impl_->sceneAssets->GetCatalog().Find(game.scene.id, CatalogAssetKind::Scene);
    if (!entry) { error = "missing scene UUID in save game: " + game.scene.id.ToString(); return false; }
    std::ifstream sceneInput(entry->source, std::ios::binary);
    if (!sceneInput) { error = "cannot open referenced scene"; return false; }
    const std::string sceneText(std::istreambuf_iterator<char>{sceneInput}, std::istreambuf_iterator<char>{});
    SceneDocument base;
    if (!ParseSceneDocument(sceneText, base, error)) return false;
    std::set<std::string> authored, spawned, deleted(game.deleted.begin(), game.deleted.end());
    for (const auto& item : base.entities) authored.insert(item.id);
    for (const auto& item : game.spawned.entities) {
        if (authored.contains(item.id)) { error = "spawned entity collides with scene UUID: " + item.id; return false; }
        if (!item.persistence) { error = "spawned entity has no persistence policy: " + item.id; return false; }
        spawned.insert(item.id);
    }
    for (const auto& id : game.deleted) if (!authored.contains(id)) {
        error = "deleted entity is not in the scene: " + id; return false;
    }
    auto removed = [&](std::string id) {
        std::set<std::string> visited;
        while (!id.empty() && visited.insert(id).second) {
            if (deleted.contains(id)) return true;
            if (const auto it = game.spawnParents.find(id); it != game.spawnParents.end()) {
                id = it->second;
            } else {
                const auto found = std::find_if(base.entities.begin(), base.entities.end(), [&](const auto& e) { return e.id == id; });
                id = found != base.entities.end() && found->parent ? *found->parent : "";
            }
        }
        return false;
    };
    for (const auto& [id, parent] : game.spawnParents)
        if ((!authored.contains(parent) && !spawned.contains(parent)) || removed(parent)) {
            error = "missing spawned parent for " + id; return false;
        }
    for (const auto& state : game.entities) {
        if ((!authored.contains(state.id) && !spawned.contains(state.id)) || removed(state.id)) {
            error = "unknown saved entity: " + state.id; return false;
        }
        const auto* definition = [&]() -> const SceneEntityDefinition* {
            for (const auto& item : game.spawned.entities) if (item.id == state.id) return &item;
            for (const auto& item : base.entities) if (item.id == state.id) return &item;
            return nullptr;
        }();
        if (!definition->persistence || (state.transform && !definition->persistence->transform) ||
            (state.controller && !definition->persistence->controllerState)) {
            error = "save-game state is not enabled for " + state.id; return false;
        }
        if (state.controller && (!definition->controller ||
            definition->controller->type != state.controller->type ||
            definition->controller->version != state.controller->version)) {
            error = "saved controller type/version mismatch for " + state.id; return false;
        }
    }
    // Check parent cycles before touching the live scene.
    for (const auto& [id, parent] : game.spawnParents) {
        std::set<std::string> seen{id};
        std::string cursor = parent;
        while (spawned.contains(cursor) && game.spawnParents.contains(cursor)) {
            if (!seen.insert(cursor).second) { error = "spawn hierarchy cycle"; return false; }
            cursor = game.spawnParents.at(cursor);
        }
        if (!seen.insert(cursor).second) { error = "spawn hierarchy cycle"; return false; }
    }
    impl_->deferControllerActivation = true;
    const bool loaded = LoadScene(entry->source, error);
    impl_->deferControllerActivation = false;
    if (!loaded) return false;
    std::string diagnostics = std::move(error);
    for (const auto& id : game.deleted) {
        if (const auto entity = impl_->scene.FindById(id); entity != entt::null) impl_->scene.DestroyEntity(entity);
    }
    PopulateScene(game.spawned, *impl_->sceneAssets, impl_->scene);
    for (const auto& [id, parent] : game.spawnParents)
        if (!impl_->scene.SetParent(impl_->scene.FindById(id), impl_->scene.FindById(parent)))
            diagnostics += "cannot restore parent for " + id + "\n";
    for (const auto& state : game.entities) {
        const auto entity = impl_->scene.FindById(state.id);
        if (entity == entt::null) { diagnostics += "missing saved entity " + state.id + "\n"; continue; }
        if (state.transform) impl_->scene.GetTransform(entity) = *state.transform;
        if (state.controller) {
            std::string detail;
            if (!impl_->scene.RestoreControllerGameState(entity, state.controller->data, detail) || !detail.empty())
                diagnostics += state.id + ": " + detail + "\n";
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
    MaterialParameters parameters = source->parameters;
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
