#include <engine/scripting/ScriptSubsystem.hpp>

#include <string>
#include <exception>
#include <utility>
#include <cwchar>

#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>

namespace engine {
namespace {

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

struct ResolvedAutoload {
    std::string name;
    ScriptInstanceDesc script;
};

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
        error = "ScriptSubsystem is not initialized";
        return false;
    }
    if (playing_) {
        error = "A scripting Play session is already active";
        return false;
    }
    if (!ValidateAutoloadDefinitions(autoloads, error)) return false;
    try {
        const auto absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(root));
        if (root.empty() || !std::filesystem::is_directory(absolute)) {
            error = "The scripting root must be an existing directory";
            return false;
        }
        std::vector<ResolvedAutoload> resolved;
        resolved.reserve(autoloads.size());
        for (const auto& definition : autoloads) {
            if (!definition.enabled) continue;
            // The shared catalog survives scene-assets teardown. The path hint is never a lookup key.
            const auto* entry = GetEngine().GetAssets().GetCatalog().Find(
                definition.script.id, CatalogAssetKind::Script);
            if (!entry) {
                error = "Autoload '" + definition.name + "': missing Script UUID " + definition.script.id.ToString();
                return false;
            }
            std::error_code sourceError;
            const auto source = std::filesystem::canonical(entry->source, sourceError);
            if (sourceError || !std::filesystem::is_regular_file(source)) {
                error = "Autoload '" + definition.name + "': cannot read script source " + ToUtf8(entry->source);
                if (sourceError) error += ": " + sourceError.message();
                return false;
            }
            if (!IsUnderRoot(source, absolute)) {
                error = "Autoload '" + definition.name + "': script source escapes project content root";
                return false;
            }
            resolved.push_back({definition.name, {source, entry->metadata.id, {}}});
        }

        // Resolve every asset before executing Lua. Keep IDs before StartInstance so a failed
        // OnStart is included in cleanup, using the runtime's existing faulted-instance policy.
        autoloadInstances_.reserve(resolved.size());
        runtime_.Shutdown();
        if (!runtime_.Initialize(absolute, error)) return false;
        root_ = absolute;
        for (const auto& definition : resolved) {
            InstanceId instance;
            if (!runtime_.CreateInstance(definition.script, instance, error)) {
                error = "Autoload '" + definition.name + "': " + error;
                EndPlay();
                return false;
            }
            autoloadInstances_.push_back(instance);
            if (!runtime_.StartInstance(instance, error)) {
                error = "Autoload '" + definition.name + "': " + error;
                EndPlay();
                return false;
            }
        }
        playing_ = true;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        EndPlay();
        return false;
    }
}

void ScriptSubsystem::EndPlay() noexcept {
    playing_ = false;
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
}

void ScriptSubsystem::Update(const FrameContext& frame) {
    if (!playing_ || frame.paused || frame.deltaSeconds <= 0.0) return;
    std::string error;
    (void)runtime_.UpdateInstances(frame.deltaSeconds, error);  // Runtime logs instance diagnostics.
}

}  // namespace engine
