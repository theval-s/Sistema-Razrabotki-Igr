#include <engine/scripting/ScriptSubsystem.hpp>

#include <string>
#include <exception>
#include <utility>
#include <cwchar>
#include <algorithm>
#include <iterator>

#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/core/Log.hpp>

namespace engine {
namespace {

// Runtime failures are already logged by LuaRuntime. Only orchestration/preflight errors use this.
bool StartupError(std::string& error, std::string message) {
    error = std::move(message);
    log::Lua().error("{}", error);
    return false;
}

// Match the runtime loader's component-wise, canonical containment check.
bool IsUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto inner = path.begin();
    for (const auto& component : root) {
        if (inner == path.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(inner->c_str(), component.c_str()) != 0) return false;
#else
        if (*inner != component) return false;
#endif
        ++inner;
    }
    return true;
}

}  // namespace

std::string_view ScriptSubsystem::GetName() const {
    return "ScriptSubsystem";
}

bool ScriptSubsystem::Initialize() {
    ready_ = true;
    return true;
}

void ScriptSubsystem::Shutdown() {
    EndPlay();
    ready_ = false;
}

bool ScriptSubsystem::BeginPlay(const std::filesystem::path& root, std::string& error) {
    return BeginPlay(root, std::span<const AutoloadDefinition>{}, error);
}

bool ScriptSubsystem::BeginPlay(const std::filesystem::path& root,
                                std::span<const AutoloadDefinition> autoloads, std::string& error, bool deferStart) {
    if (!ready_) {
        return StartupError(error, "ScriptSubsystem is not initialized");
    }
    if (playing_ || endingPlay_) {
        return StartupError(error, "A scripting Play session is already active or ending");
    }
    if (!ValidateAutoloadDefinitions(autoloads, error)) return StartupError(error, error);
    try {
        const auto absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(root));
        if (root.empty() || !std::filesystem::is_directory(absolute)) {
            return StartupError(error, "The scripting root must be an existing directory");
        }
        std::vector<ScriptInstanceDesc> resolved;
        resolved.reserve(autoloads.size());
        for (const auto& definition : autoloads) {
            if (!definition.enabled) continue;
            const std::string context = "Autoload '" + definition.name + "' [script=" +
                definition.script.id.ToString() + "]";
            // The shared catalog survives scene-assets teardown. The path hint is never a lookup key.
            if (!TryGetEngine()) return StartupError(error, "Autoloads require an owning Engine");
            const auto* entry = GetEngine().GetAssets().GetCatalog().Find(
                definition.script.id, CatalogAssetKind::Script);
            if (!entry) {
                return StartupError(error, context + ": missing Script UUID");
            }
            std::error_code sourceError;
            const auto source = std::filesystem::canonical(entry->source, sourceError);
            if (sourceError || !std::filesystem::is_regular_file(source, sourceError)) {
                error = context + ": cannot read script source " + ToUtf8(entry->source);
                if (sourceError) error += ": " + sourceError.message();
                return StartupError(error, error);
            }
            if (!IsUnderRoot(source, absolute)) {
                return StartupError(error, context + ": script source escapes project content root");
            }
            resolved.push_back({source, entry->metadata.id, {}, definition.name});
        }

        // Resolve every asset before executing Lua. Keep IDs before StartInstance so a failed
        // OnStart is included in cleanup, using the runtime's existing faulted-instance policy.
        autoloadInstances_.reserve(resolved.size());
        runtime_.Shutdown();
        if (!(TryGetEngine() ? runtime_.Initialize(absolute, *TryGetEngine(), error) : runtime_.Initialize(absolute, error))) return false;
        root_ = absolute;
        autoloadDescriptions_ = resolved;
        prepared_ = deferStart;
        for (const auto& definition : resolved) {
            InstanceId instance;
            if (!runtime_.CreateInstance(definition, instance, error)) {
                EndPlay();
                return false;
            }
            autoloadInstances_.push_back(instance);
            if (!deferStart && !runtime_.StartInstance(instance, error)) {
                EndPlay();
                return false;
            }
        }
        playing_ = true;
        if (!deferStart) runtime_.FlushSceneCommands();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        StartupError(error, exception.what());
        EndPlay();
        return false;
    }
}

void ScriptSubsystem::EndPlay() noexcept {
    if (endingPlay_) return;
    endingPlay_ = true;
    playing_ = false;
    runtime_.SuspendUiSession();
    ClearSceneScripts();
    runtime_.DestroyEntityInstances();
    for (auto it = autoloadInstances_.rbegin(); it != autoloadInstances_.rend(); ++it) {
        try {
            std::string error;
            (void)runtime_.DestroyInstance(*it, error);
        } catch (...) {
            // Shutdown below still releases all remaining Lua references.
        }
    }
    autoloadInstances_.clear();
    autoloadDescriptions_.clear();
    prepared_ = false;
    runtime_.Shutdown();
    root_.clear();
    endingPlay_ = false;
}

InstanceId ScriptSubsystem::FindPhysicsInstance(entt::entity entity) const {
    if (!playing_ || !scene_) return {};
    const auto it = sceneInstances_.find(entity);
    if (it == sceneInstances_.end() || !it->second.created) return {};
    const auto* component = scene_->TryGetComponent<ScriptComponent>(entity);
    if (!component || !component->enabled || component->script.id != it->second.asset) return {};
    return it->second.instance;
}

void ScriptSubsystem::OnPhysicsActivity(const PhysicsActivityEvent& event) {
    if (const auto instance = FindPhysicsInstance(event.entity); instance.IsValid()) {
        std::string error;
        (void)runtime_.NotifyPhysicsActivity(instance, event.awake, error);
    }
}

void ScriptSubsystem::OnPhysicsContact(const PhysicsContactEvent& event) {
    const auto notify = [&](entt::entity entity, entt::entity other) {
        if (const auto instance = FindPhysicsInstance(entity); instance.IsValid()) {
            std::string error;
            (void)runtime_.NotifyPhysicsContact(instance, event, other, error);
        }
    };
    notify(event.first, event.second);
    notify(event.second, event.first);
}

void ScriptSubsystem::Update(const FrameContext& frame) {
    if (!playing_ || prepared_) return;
    runtime_.FlushSceneCommands();
    if (frame.paused || frame.deltaSeconds <= 0.0) return;
    std::string error;
    (void)ReconcileSceneScripts(error);
    runtime_.FlushSceneCommands();
    (void)runtime_.UpdateInstances(frame.deltaSeconds, error);  // Runtime logs instance diagnostics.
    runtime_.FlushSceneCommands();
}

void ScriptSubsystem::FixedUpdate(const FixedUpdateContext& step) {
    if (!playing_ || prepared_ || step.deltaSeconds <= 0) return;
    runtime_.FlushSceneCommands();
    std::string error;
    (void)ReconcileSceneScripts(error);
    runtime_.FlushSceneCommands();
    (void)runtime_.FixedUpdateInstances(step.deltaSeconds, error);
    runtime_.FlushSceneCommands();
}

void ScriptSubsystem::RemoveSceneScript(entt::entity entity) noexcept {
    const auto it = sceneInstances_.find(entity);
    if (it == sceneInstances_.end()) return;
    if (it->second.created) {
        try {
            std::string error;
            (void)runtime_.DestroyInstance(it->second.instance, error);
        } catch (...) {}
    }
    sceneInstances_.erase(it); // Expire owner handles after OnDestroy.
}

void ScriptSubsystem::ClearSceneScripts() noexcept {
    runtime_.DiscardSceneCommands();
    if (scene_ && TryGetEngine()) {
        auto& events = GetEngine().GetEvents();
        events.sink<PhysicsContactEvent>().disconnect<&ScriptSubsystem::OnPhysicsContact>(*this);
        events.sink<PhysicsActivityEvent>().disconnect<&ScriptSubsystem::OnPhysicsActivity>(*this);
    }
    while (!sceneInstances_.empty()) RemoveSceneScript(sceneInstances_.begin()->first);
    if (scene_) scene_->RemoveLifecycleObserver(observer_);
    scene_ = nullptr;
    runtime_.DiscardSceneCommands();
}

void ScriptSubsystem::StartSceneScripts(Scene& scene) {
    if (prepared_) {
        std::string error;
        if (PrepareSceneScripts(scene, error)) (void)StartPreparedScripts(error);
        return;
    }
    ClearSceneScripts();
    if (!playing_) return;
    AssociateScene(scene);
    std::string error;
    (void)ReconcileSceneScripts(error);
    runtime_.FlushSceneCommands();
}

void ScriptSubsystem::AssociateScene(Scene& scene) {
    scene_ = &scene;
    if (TryGetEngine()) {
        auto& events = GetEngine().GetEvents();
        events.sink<PhysicsContactEvent>().connect<&ScriptSubsystem::OnPhysicsContact>(*this);
        events.sink<PhysicsActivityEvent>().connect<&ScriptSubsystem::OnPhysicsActivity>(*this);
    }
    observer_ = scene.AddLifecycleObserver([this](Scene::LifecycleEvent event, entt::entity entity,
                                                 entt::id_type component) {
        if (event == Scene::LifecycleEvent::EntityDestroying ||
            component == entt::type_hash<ScriptComponent>::value()) RemoveSceneScript(entity);
    });
}

bool ScriptSubsystem::ReconcileSceneScripts(std::string& error, bool startInstances) {
    error.clear();
    if (!scene_) return true;
    std::vector<entt::entity> removed;
    for (const auto& [entity, record] : sceneInstances_) {
        const auto* component = scene_->TryGetComponent<ScriptComponent>(entity);
        if (!component || component->script.id != record.asset || component->enabled != record.enabled)
            removed.push_back(entity);
    }
    for (const auto entity : removed) RemoveSceneScript(entity);
    std::vector<InstanceId> start;
    scene_->ForEach<ScriptComponent>([&](entt::entity entity, const ScriptComponent& component) {
        if (sceneInstances_.contains(entity)) return;
        auto& record = sceneInstances_[entity];
        record.asset = component.script.id;
        record.enabled = component.enabled;
        if (!component.enabled) return;
        if (!component.script.id.IsValid()) {
            if (error.empty()) error = "Enabled script attachment has an invalid asset UUID";
            return;
        }
        const auto* entry = TryGetEngine() ? GetEngine().GetAssets().GetCatalog().Find(record.asset, CatalogAssetKind::Script) : nullptr;
        if (!entry) {
            if (error.empty()) error = "Entity " + scene_->GetId(entity).ToString() + ": missing Script UUID";
            log::Lua().error("Entity {} [script={}]: missing Script UUID", scene_->GetId(entity).ToString(),
                          record.asset.ToString());
            return; // Remember the failed attachment; do not repeat the error every frame.
        }
        record.lifetime = std::make_shared<int>(0);
        ScriptInstanceDesc desc{entry->source, record.asset, scene_->GetId(entity).ToString()};
        desc.owner = LuaEntityHandle::FromScriptOwner(*scene_, entity, record.lifetime);
        desc.properties = component.properties;
        std::string failure;
        record.created = runtime_.CreateInstance(desc, record.instance, failure);
        if (!record.created && error.empty()) error = std::move(failure);
        if (record.created) start.push_back(record.instance);
    });
    if (startInstances) for (const auto instance : start) {
        std::string failure;
        if (!runtime_.StartInstance(instance, failure) && error.empty()) error = std::move(failure);
    }
    return error.empty();
}

bool ScriptSubsystem::PrepareSceneScripts(Scene& scene, std::string& error) {
    if (!playing_) return StartupError(error, "No scripting Play session is active");
    ClearSceneScripts();
    prepared_ = true;
    AssociateScene(scene);
    return ReconcileSceneScripts(error, false);
}

bool ScriptSubsystem::StartPreparedScripts(std::string& error) {
    if (!playing_) return StartupError(error, "No scripting Play session is active");
    for (const auto& [entity, record] : sceneInstances_) {
        if (record.enabled && !record.created)
            return StartupError(error, "Cannot start a failed prepared script attachment");
    }
    // Do not flush until every record/iterator used by startup has been released.
    for (const auto instance : autoloadInstances_)
        if (!runtime_.StartInstance(instance, error)) return false;
    std::vector<InstanceId> instances;
    if (scene_) scene_->ForEach<ScriptComponent>([&](entt::entity entity, const ScriptComponent&) {
        const auto it = sceneInstances_.find(entity);
        if (it != sceneInstances_.end() && it->second.created) instances.push_back(it->second.instance);
    });
    for (const auto instance : instances)
        if (!runtime_.StartInstance(instance, error)) return false;
    prepared_ = false;
    runtime_.FlushSceneCommands();
    error.clear();
    return true;
}

bool ScriptSubsystem::CaptureGameState(GameDocument& game, std::string& error) {
    if (!playing_ || prepared_) return StartupError(error, "Script state requires a started Play session");
    // Commit only after all hooks succeed; a failed hook does not damage the document.
    GameDocument captured = game;
    captured.autoloads.clear();
    for (std::size_t i = 0; i < autoloadInstances_.size(); ++i) {
        const auto instance = autoloadInstances_[i];
        if (!runtime_.HasPersistenceHooks(instance)) continue;
        GameScriptState state;
        state.asset = autoloadDescriptions_[i].assetId;
        if (!runtime_.CaptureSaveState(instance, state.version, state.data, error)) return false;
        captured.autoloads.emplace(autoloadDescriptions_[i].autoloadName, std::move(state));
    }
    for (auto& entity : captured.entities) entity.script.reset();
    if (scene_) {
        bool clean = true;
        scene_->ForEach<GamePersistence>([&](entt::entity entity, const GamePersistence& persistence) {
            if (!clean || !persistence.scriptState) return;
            const auto it = sceneInstances_.find(entity);
            if (it == sceneInstances_.end() || !it->second.created) {
                error = "Persistent entity has no script instance: " + scene_->GetId(entity).ToString(); clean = false; return;
            }
            GameScriptState state;
            state.asset = it->second.asset;
            if (!runtime_.CaptureSaveState(it->second.instance, state.version, state.data, error)) { clean = false; return; }
            const auto id = scene_->GetId(entity);
            auto saved = std::find_if(captured.entities.begin(), captured.entities.end(), [&](const GameEntityState& item) { return item.id == id; });
            if (saved == captured.entities.end()) {
                captured.entities.push_back({});
                saved = std::prev(captured.entities.end());
                saved->id = id;
            }
            saved->script = std::move(state);
        });
        if (!clean) return false;
    }
    game = std::move(captured);
    error.clear();
    return true;
}

bool ScriptSubsystem::PreflightGameState(const GameDocument& game, std::string& error) const {
    for (const auto& [name, state] : game.autoloads) {
        const auto configured = std::find_if(autoloadDescriptions_.begin(), autoloadDescriptions_.end(),
            [&](const ScriptInstanceDesc& desc) { return desc.autoloadName == name; });
        // Descriptions contain only enabled, resolved autoload definitions.
        if (configured == autoloadDescriptions_.end() || configured->assetId != state.asset) {
            error = "Saved autoload is unavailable, disabled or has a different asset: " + name;
            return false;
        }
        if (!runtime_.ValidateAssetSaveVersion(state.asset, state.version, error)) return false;
    }
    for (const auto& entity : game.entities)
        if (entity.script && !runtime_.ValidateAssetSaveVersion(entity.script->asset, entity.script->version, error))
            return false;
    error.clear();
    return true;
}

bool ScriptSubsystem::ValidateGameState(const GameDocument& game, std::string& error) {
    if (!playing_ || !prepared_) return StartupError(error, "Script restore requires prepared instances");
    for (const auto& [name, state] : game.autoloads) {
        bool found = false;
        for (std::size_t i = 0; i < autoloadDescriptions_.size(); ++i) {
            const auto& desc = autoloadDescriptions_[i];
            if (desc.autoloadName != name) continue;
            found = true;
            if (desc.assetId != state.asset) { error = "Saved autoload asset mismatch: " + name; return false; }
            if (!runtime_.ValidateSaveVersion(autoloadInstances_[i], state.version, error)) return false;
        }
        if (!found) { error = "Saved autoload is unavailable: " + name; return false; }
    }
    for (const auto& saved : game.entities) {
        if (!saved.script) continue;
        const auto entity = scene_ ? scene_->FindByUuid(saved.id) : entt::null;
        const auto it = sceneInstances_.find(entity);
        const auto* persistence = scene_ ? scene_->TryGetComponent<GamePersistence>(entity) : nullptr;
        if (it == sceneInstances_.end() || !it->second.created || !persistence || !persistence->scriptState ||
            it->second.asset != saved.script->asset) {
            error = "Saved entity script identity/opt-in mismatch: " + saved.id.ToString(); return false;
        }
        if (!runtime_.ValidateSaveVersion(it->second.instance, saved.script->version, error)) return false;
    }
    error.clear();
    return true;
}

bool ScriptSubsystem::RestoreGameState(const GameDocument& game, std::string& error) {
    if (!ValidateGameState(game, error)) return false;
    for (std::size_t i = 0; i < autoloadDescriptions_.size(); ++i) {
        const auto saved = game.autoloads.find(autoloadDescriptions_[i].autoloadName);
        if (saved != game.autoloads.end() && !runtime_.RestoreSaveState(autoloadInstances_[i], saved->second.version, saved->second.data, error)) return false;
    }
    for (const auto& saved : game.entities) {
        if (!saved.script) continue;
        const auto it = sceneInstances_.find(scene_->FindByUuid(saved.id));
        if (!runtime_.RestoreSaveState(it->second.instance, saved.script->version, saved.script->data, error)) return false;
    }
    error.clear();
    return true;
}

}  // namespace engine
