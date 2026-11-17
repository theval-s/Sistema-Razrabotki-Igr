#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <entt/entity/fwd.hpp>
#include <engine/core/Subsystem.hpp>
#include <engine/animation/AnimationComponents.hpp>
#include <engine/rendering/resources/Skinning.hpp>

namespace engine {

struct AnimationPose {
    std::uint64_t skeletonSignature = 0;
    std::vector<DirectX::SimpleMath::Matrix> jointModels;
};

struct AnimationPlaybackState {
    bool ready = false;
    bool playing = false;
    bool transitioning = false;
    double timeSeconds = 0.0;
    float duration = 0.0f;
    std::string error;
    /// Runtime transport values, distinct from the Animator's authored defaults.
    AssetReference clip;
    AssetReference targetClip;
    float speed = 1.0f;
    bool loop = true;
};

/// Main-thread control API. Evaluates local poses before render extraction.
/// Returned snapshots own their memory and never alias mutable sampling scratch.
class ENGINE_API AnimationSubsystem final : public Subsystem {
public:
    AnimationSubsystem();
    ~AnimationSubsystem() override;
    std::string_view GetName() const override;
    bool Initialize() override;
    void Shutdown() override;
    void ResetSimulation() override;
    void LateUpdate(const FrameContext& frame) override;

    /// Starts the target clip at time zero once loaded. A positive fade blends
    /// from the current player; another fade is rejected until it finishes.
    bool Play(entt::entity entity, const AssetReference& clip, float fadeSeconds,
              bool loop, std::string& error);
    bool SetPaused(entt::entity entity, bool paused);
    bool Stop(entt::entity entity);
    bool Seek(entt::entity entity, double seconds, std::string& error);
    bool SetSpeed(entt::entity entity, float speed, std::string& error);
    /// Edit-mode audition of one scene entity. Uses real time while simulation stays
    /// frozen. Subsequent Pause/Seek/SetSpeed controls affect this transient player.
    /// Replacing the entity ends its previous preview; clip changes may crossfade.
    /// Requires an editor host outside Play. Authored components are never changed.
    bool Preview(entt::entity entity, const AssetReference& clip, float fadeSeconds,
                 bool loop, std::string& error);
    /// Restores the normal Edit pose (authored starting clip at zero, or rest pose).
    /// Also called on simulation reset, scene replacement, and entering Play.
    void EndPreview();
    [[nodiscard]] entt::entity GetPreviewEntity() const;
    [[nodiscard]] std::shared_ptr<const AnimationPose> GetPose(entt::entity entity) const;
    [[nodiscard]] std::optional<AnimationPlaybackState> GetPlaybackState(entt::entity entity) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Uses the rest pose when pose is null or incompatible. Each call produces a
/// frame-owned palette; a shared mesh is never modified by an instance's pose.
[[nodiscard]] ENGINE_API std::shared_ptr<const SkinningPalette> BuildSkinningPalette(
    const SkinBinding& binding, std::uint64_t skeletonSignature, const AnimationPose* pose);
[[nodiscard]] ENGINE_API Aabb SkinnedMeshBounds(const SkinnedMeshAsset& mesh, const AnimationPose* pose);

} // namespace engine
