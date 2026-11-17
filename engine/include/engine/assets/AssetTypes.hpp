#pragma once

#include <SimpleMath.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetHandle.hpp>

#include <engine/rendering/resources/RenderedMesh.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include <engine/rendering/resources/Texture.hpp>
#include <engine/assets/AssetCommon.hpp>
#include <engine/assets/MaterialDefinition.hpp>

namespace engine {

/// Records name GPU resources owned by RenderingResourceManager, without owning D3D
/// objects. UnloadAll invalidates GPU handles; shared CPU metadata may outlive them.

/// Axis-aligned bounds in imported mesh-local space (after centerOnLoad, if enabled).
struct Aabb {
    DirectX::SimpleMath::Vector3 min{};
    DirectX::SimpleMath::Vector3 max{};
    bool operator==(const Aabb&) const = default;
};

/// Imported mesh-local positions and CCW triangles, retained for physics cooking.
/// Shared once per asset; independent of GPU resource lifetime.
struct MeshGeometry {
    std::vector<DirectX::SimpleMath::Vector3> positions;
    std::vector<uint32_t> indices;
};

/// One draw of a mesh asset: the triangles of one material slot.
struct MeshSection {
    MeshHandle mesh;
    std::uint32_t materialSlot = 0;
};

struct MaterialInstanceAsset;

struct MeshAsset {
    std::vector<MeshSection> sections;
    std::vector<std::string> materialSlots; // names, indexed by MeshSection::materialSlot
    // Per slot: the default from the mesh's metadata, else the engine default instance.
    // Requested with the mesh, not awaited: a section waits for its own material.
    std::vector<AssetHandle<MaterialInstanceAsset>> materials;
    Aabb bounds;
    std::shared_ptr<const MeshGeometry> geometry;
};

namespace detail {
struct SkeletonData;
struct AnimationData;
}

/// Immutable CPU assets. The ozz representation stays private to the engine.
struct SkeletonAsset {
    std::shared_ptr<const detail::SkeletonData> data;
    std::uint64_t signature = 0;
    std::uint32_t jointCount = 0;
    // Engine-order hierarchy for editor inspection; -1 denotes a root.
    std::vector<std::string> jointNames;
    std::vector<std::int32_t> jointParents;
    std::vector<DirectX::SimpleMath::Matrix> restModels;
};

struct AnimationClipAsset {
    std::shared_ptr<const detail::AnimationData> data;
    std::uint64_t skeletonSignature = 0;
    float duration = 0.0f;
};

/// All arrays use the section's palette order, not skeleton order. Bounds are
/// in joint bind-local space and conservatively enclose every influenced vertex.
struct SkinBinding {
    std::vector<std::uint16_t> joints;
    std::vector<DirectX::SimpleMath::Matrix> inverseBind;
    std::vector<DirectX::SimpleMath::Matrix> restPalette;
    std::vector<Aabb> jointBounds;
};

struct SkinnedMeshSection {
    MeshHandle mesh;
    std::uint32_t materialSlot = 0;
    std::shared_ptr<const SkinBinding> binding;
};

struct SkinnedMeshAsset {
    std::vector<SkinnedMeshSection> sections;
    Aabb bounds; // asset-local rest-pose bounds
    std::uint64_t skeletonSignature = 0;
    AssetReference skeletonReference;
};

struct TextureAsset {
    TextureHandle texture;
};

/// One compiled shader variant and stage, owned by the RenderingResourceManager.
struct ShaderAsset {
    ShaderHandle shader;
    ShaderType stage;
};

struct MaterialAsset {
    MaterialHandle material;
    MaterialDefinition definition; // authored references/defaults for editor inspection and saving
};

struct MaterialInstanceAsset {
    MaterialInstanceHandle instance;
    MaterialInstanceDefinition definition; // sparse overrides, not flattened runtime parameters
};

/// Which table each asset type lives in; used by the templated lookup helpers.
template <typename T>
struct AssetValueKind;

template <>
struct AssetValueKind<MeshAsset> {
    static constexpr AssetKind value = AssetKind::Mesh;
};

template <>
struct AssetValueKind<SkeletonAsset> {
    static constexpr AssetKind value = AssetKind::Skeleton;
};
template <>
struct AssetValueKind<AnimationClipAsset> {
    static constexpr AssetKind value = AssetKind::AnimationClip;
};
template <>
struct AssetValueKind<SkinnedMeshAsset> {
    static constexpr AssetKind value = AssetKind::SkinnedMesh;
};

template <>
struct AssetValueKind<TextureAsset> {
    static constexpr AssetKind value = AssetKind::Texture;
};

template <>
struct AssetValueKind<ShaderAsset> {
    static constexpr AssetKind value = AssetKind::Shader;
};

template <>
struct AssetValueKind<MaterialAsset> {
    static constexpr AssetKind value = AssetKind::Material;
};

template <>
struct AssetValueKind<MaterialInstanceAsset> {
    static constexpr AssetKind value = AssetKind::MaterialInstance;
};

}  // namespace engine
