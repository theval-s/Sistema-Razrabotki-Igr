#pragma once

#include <engine/rendering/resources/RenderedMesh.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include <engine/rendering/resources/Texture.hpp>
#include <engine/assets/AssetCommon.hpp>
#include <engine/assets/MaterialDefinition.hpp>

namespace engine {

/// Thin records: they name GPU resources owned by RenderingResourceManager, they do not
/// hold D3D objects or free anything. The handles they carry stay valid for the process.

struct MeshAsset {
    MeshHandle mesh;
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
