#pragma once

#include <vector>

#include <entt/resource/resource.hpp>

#include <engine/engine_export.hpp>
#include <engine/assets/AssetHandle.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetTypes.hpp>
#include <engine/rendering/RenderWorld.hpp>
#include <engine/rendering/resources/Material.hpp>

// Scene components the renderer understands. RenderExtractionSubsystem copies them into
// the RenderWorld every frame.

namespace engine {

class AssetSystem;

/// One material slot's own material on a MeshRenderer. Both empty leave the slot alone.
struct SlotMaterial {
    AssetHandle<MaterialInstanceAsset> materialInstance;
    AssetReference materialReference;
};

/// Draws a mesh asset with the entity's world matrix. Assets come from
/// Engine::GetAssets(); until they are loaded the entity is simply not drawn.
/// A slot draws with its slot material, else the renderer's material instance, else the
/// mesh's default for that slot (see MeshAsset).
struct MeshRenderer {
    AssetHandle<MeshAsset> mesh;
    AssetHandle<MaterialInstanceAsset> materialInstance; // for every slot; none for the mesh's own
    // Retain authored UUIDs even when an asynchronous request fails and its
    // generational handle retires. Runtime handles are never written to JSON.
    AssetReference meshReference;
    AssetReference materialReference;
    std::vector<SlotMaterial> slotMaterials; // by material slot; may be shorter than the mesh's
};

/// Whether the renderer names a material of its own, loaded or not.
[[nodiscard]] inline bool HasOwnMaterial(const MeshRenderer& renderer) {
    return static_cast<bool>(renderer.materialInstance) || renderer.materialReference.id.IsValid();
}
/// Whether the slot names a material, loaded or not.
[[nodiscard]] inline bool HasOwnMaterial(const SlotMaterial& slot) {
    return static_cast<bool>(slot.materialInstance) || slot.materialReference.id.IsValid();
}
/// What one section draws with: its slot's material, else the renderer's material instance, else
/// the mesh's default for the slot. Empty until that material is ready.
[[nodiscard]] ENGINE_API entt::resource<MaterialInstanceAsset> SectionMaterial(AssetSystem& assets,
    const MeshRenderer& renderer, const MeshAsset& mesh, const MeshSection& section);
/// What runtime material edits start from: the first section's material, else the renderer's
/// while the mesh loads.
[[nodiscard]] ENGINE_API entt::resource<MaterialInstanceAsset> PrimaryMaterial(AssetSystem& assets,
                                                                               const MeshRenderer& renderer);

/// Renders the scene from the entity's world position, looking along its local -Z
/// (Vector3::Forward); roll is ignored. The first entity with a Camera is the active one.
/// The aspect ratio follows the engine's surface.
struct Camera {
    float fovY = 1.04719755f;  // 60 degrees
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    bool operator==(const Camera&) const = default;
};

/// Sun-like light shining along the entity's local -Z (Vector3::Forward); its position
/// doesn't matter. Only the first one is used, the renderer supports a single one.
struct DirectionalLight {
    DirectX::SimpleMath::Vector3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool operator==(const DirectionalLight&) const = default;
};

/// Omnidirectional light centered at the entity's world position.
struct PointLight {
    DirectX::SimpleMath::Vector3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 5.0f;
    bool operator==(const PointLight&) const = default;
};

/// Cone of light centered at the entity's world position, pointing along its
/// local -Z. Half angles are in radians (not shader-space cosines).
struct SpotLight {
    DirectX::SimpleMath::Vector3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerHalfAngle = 0.2617994f;
    float outerHalfAngle = 0.44879895f;
    bool operator==(const SpotLight&) const = default;
};

/// Asset names of the primitives shipped in engine/primitives (copied next to the executable).
namespace BuiltinAssets {
inline constexpr const char* CubeMesh = "engine/primitives/cube";
/// 1x1 in XZ, facing +Y.
inline constexpr const char* PlaneMesh = "engine/primitives/plane";
inline constexpr const char* WhiteTexture = "engine/primitives/white";
inline constexpr const char* DefaultMaterialInstance = "engine/primitives/default.material-instance.json";
}  // namespace BuiltinAssets

}  // namespace engine
