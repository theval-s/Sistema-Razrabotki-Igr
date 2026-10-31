#include <engine/scripting/ScriptSubsystem.hpp>

#include <string>
#include <exception>
#include <utility>
#include <cwchar>

#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <spdlog/spdlog.h>

namespace engine {
namespace {

// Runtime failures are already logged by LuaRuntime. Only orchestration/preflight errors use this.
bool StartupError(std::string& error, std::string message) {
    error = std::move(message);
    spdlog::error("[Lua] {}", error);
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
                                std::span<const AutoloadDefinition> autoloads, std::string& error) {
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
        if (!runtime_.Initialize(absolute, error)) return false;
        root_ = absolute;
        for (const auto& definition : resolved) {
            InstanceId instance;
            if (!runtime_.CreateInstance(definition, instance, error)) {
                EndPlay();
                return false;
            }
            autoloadInstances_.push_back(instance);
            if (!runtime_.StartInstance(instance, error)) {
                EndPlay();
                return false;
            }
        }
        playing_ = true;
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
    runtime_.Shutdown();
    root_.clear();
    endingPlay_ = false;
}

void ScriptSubsystem::Update(const FrameContext& frame) {
    if (!playing_ || frame.paused || frame.deltaSeconds <= 0.0) return;
    ReconcileSceneScripts();
    std::string error;
    (void)runtime_.UpdateInstances(frame.deltaSeconds, error);  // Runtime logs instance diagnostics.
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
    while (!sceneInstances_.empty()) RemoveSceneScript(sceneInstances_.begin()->first);
    if (scene_) scene_->RemoveLifecycleObserver(observer_);
    scene_ = nullptr;
}

void ScriptSubsystem::StartSceneScripts(Scene& scene) {
    ClearSceneScripts();
    if (!playing_) return;
    scene_ = &scene;
    observer_ = scene.AddLifecycleObserver([this](Scene::LifecycleEvent event, entt::entity entity,
                                                 entt::id_type component) {
        if (event == Scene::LifecycleEvent::EntityDestroying ||
            component == entt::type_hash<ScriptComponent>::value()) RemoveSceneScript(entity);
    });
    ReconcileSceneScripts();
}

void ScriptSubsystem::ReconcileSceneScripts() {
    if (!scene_) return;
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
        if (!component.enabled || !component.script.id.IsValid()) return;
        const auto* entry = GetEngine().GetAssets().GetCatalog().Find(record.asset, CatalogAssetKind::Script);
        if (!entry) {
            spdlog::error("[Lua] Entity {} [script={}]: missing Script UUID", scene_->GetId(entity).ToString(),
                          record.asset.ToString());
            return; // Remember the failed attachment; do not repeat the error every frame.
        }
        record.lifetime = std::make_shared<int>(0);
        ScriptInstanceDesc desc{entry->source, record.asset, scene_->GetId(entity).ToString()};
        desc.owner = ScriptEntityHandle{scene_, entity, record.lifetime};
        desc.properties = component.properties;
        std::string error;
        record.created = runtime_.CreateInstance(desc, record.instance, error);
        if (record.created) start.push_back(record.instance);
    });
    for (const auto instance : start) {
        std::string error;
        (void)runtime_.StartInstance(instance, error); // A failed entity instance does not stop other scripts.
    }
}

}  // namespace engine
