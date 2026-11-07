#pragma once

#include <SimpleMath.h>
#include <memory>
#include <vector>
#include <cstdint>

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

struct MeshAsset {
    MeshHandle mesh;
    Aabb bounds;
    std::shared_ptr<const MeshGeometry> geometry;
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
