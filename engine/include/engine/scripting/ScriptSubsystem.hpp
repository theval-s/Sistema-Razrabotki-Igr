#pragma once

#include <string_view>
#include <span>
#include <vector>
#include <unordered_map>
#include <engine/scene/Scene.hpp>

#include <engine/core/Subsystem.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/AutoloadDefinition.hpp>

namespace engine {

/// Owns one scripting session. Lua state remains private to LuaRuntime.
class ENGINE_API ScriptSubsystem final : public Subsystem {
public:
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Update(const FrameContext& frame) override;
    void FixedUpdate(const FixedUpdateContext& step) override;
    void ResetSimulation() override { runtime_.DiscardSceneCommands(); }

    /// root is the project content directory (AssetPathResolver::GetGameRoot()).
    /// Pins require to that root and starts an empty session (compatibility overload).
    [[nodiscard]] bool BeginPlay(const std::filesystem::path& root, std::string& error);
    /// Resolves enabled autoloads by UUID in the owning Engine's shared catalog,
    /// then creates and starts them in list order. No entity or component is required.
    /// All sources must be inside root. Failure leaves no partially started session.
    [[nodiscard]] bool BeginPlay(const std::filesystem::path& root,
                                 std::span<const AutoloadDefinition> autoloads, std::string& error, bool deferStart = false);
    void EndPlay() noexcept;
    /// After scene population, bind attachments to this session. The scene must outlive
    /// this association; call ClearSceneScripts before unloading it. Autoloads survive.
    void StartSceneScripts(Scene& scene);
    void ClearSceneScripts() noexcept;
    [[nodiscard]] bool PrepareSceneScripts(Scene& scene, std::string& error);
    /// Restore before this call. Autoloads start in authored order, then entities.
    /// A false result requires EndPlay/ClearSceneScripts cleanup by the caller.
    [[nodiscard]] bool StartPreparedScripts(std::string& error);
    /// These operations do not flush scene commands. Capture commits game only
    /// after all hooks succeed; Restore requires prepared (not started) instances.
    [[nodiscard]] bool CaptureGameState(GameDocument& game, std::string& error);
    /// Checks configured enabled autoload identities and known asset hooks/versions
    /// without preparing/replacing the live session or executing Lua hooks.
    [[nodiscard]] bool PreflightGameState(const GameDocument& game, std::string& error) const;
    [[nodiscard]] bool ValidateGameState(const GameDocument& game, std::string& error);
    [[nodiscard]] bool RestoreGameState(const GameDocument& game, std::string& error);
    [[nodiscard]] bool IsPlaying() const noexcept { return playing_; }
    [[nodiscard]] const std::filesystem::path& GetRoot() const noexcept { return root_; }

    /// Execution errors use LuaRuntime's bool + error-string interface.
    [[nodiscard]] LuaRuntime& GetRuntime() noexcept { return runtime_; }
    [[nodiscard]] const LuaRuntime& GetRuntime() const noexcept { return runtime_; }

private:
    [[nodiscard]] InstanceId FindPhysicsInstance(entt::entity entity) const;
    void OnPhysicsActivity(const PhysicsActivityEvent& event);
    void OnPhysicsContact(const PhysicsContactEvent& event);
    bool ReconcileSceneScripts(std::string& error, bool start = true);
    void AssociateScene(Scene& scene);
    void RemoveSceneScript(entt::entity entity) noexcept;
    struct SceneInstance {
        AssetUuid asset;
        bool enabled = false;
        InstanceId instance{};
        bool created = false;
        std::shared_ptr<void> lifetime;
    };
    Scene* scene_ = nullptr;
    std::size_t observer_ = 0;
    std::unordered_map<entt::entity, SceneInstance> sceneInstances_;
    LuaRuntime runtime_;
    std::filesystem::path root_;
    std::vector<InstanceId> autoloadInstances_;
    std::vector<ScriptInstanceDesc> autoloadDescriptions_;
    bool prepared_ = false;
    bool ready_ = false;
    bool playing_ = false;
    bool endingPlay_ = false;
};

}  // namespace engine
