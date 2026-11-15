#pragma once

#include <engine/assets/AssetTypes.hpp>
#include <engine/assets/AssetHandle.hpp>
#include <optional>

namespace engine {

/// Authored starting configuration. Playback clocks, fades, and sampling caches
/// belong to AnimationSubsystem and are deliberately absent from scene snapshots.
struct Animator {
    AssetReference skeleton;
    AssetReference clip;
    float speed = 1.0f;
    bool loop = true;
    bool playOnStart = true;
    bool operator==(const Animator&) const = default;
};

/// One material instance is applied to all imported sections. Sections retain
/// their source material-slot indices and independent skin bindings.
struct SkinnedMeshRenderer {
    AssetHandle<SkinnedMeshAsset> mesh;
    AssetHandle<MaterialInstanceAsset> materialInstance;
    AssetReference meshReference;
    AssetReference materialReference;
    /// Last evaluated conservative asset-local bounds; transient, never saved.
    std::optional<Aabb> posedBounds;
};

} // namespace engine
