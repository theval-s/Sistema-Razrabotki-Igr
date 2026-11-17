#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>

// Reading what a model file brings along, and bringing it into a project with its materials:
// base color textures as files, a material instance per material slot, and the mesh meta naming
// them. Editor-side like AssetAuthoring: rescan the catalog afterwards. Paths are absolute and
// must lie in the game root, which path hints in the written references are relative to.

namespace engine {

/// An image a material slot samples for its base color.
struct ModelTexture {
    std::string name;                 // file name to extract it under, extension included
    std::vector<std::uint8_t> bytes;  // embedded in the model: the image file as stored
    std::filesystem::path file;       // a file the model names instead; empty when embedded
    std::string problem;              // why it can't be used (missing, unsupported format); empty if it can
};

struct ModelSlotInfo {
    std::string name;
    std::optional<std::size_t> texture;  // into ModelInfo::textures
    unsigned uvChannel = 0;
};

struct ModelInfo {
    std::vector<ModelSlotInfo> materialSlots;  // numbered as MeshAsset::materialSlots
    std::vector<ModelTexture> textures;
    // Files the model loads by name, which an import copies along: .gltf buffers and images, .obj .mtl.
    std::vector<std::filesystem::path> companions;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
};

[[nodiscard]] ENGINE_API bool InspectModel(const std::filesystem::path& model, ModelInfo& info, std::string& error);

struct ModelMaterialSettings {
    bool extractTextures = true;                 // base color textures from outside the project or the file
    std::optional<AssetReference> baseMaterial;  // material instances of it per slot; none for no instances
    std::filesystem::path fallbackTexture;       // an image for slots without a usable texture; empty for none
};

/// Where ExtractModelMaterials puts textures/ and materials/: the model's folder when it is the
/// folder's model.<ext>, else a folder named after the model beside it.
[[nodiscard]] ENGINE_API std::filesystem::path ModelMaterialsFolder(const std::filesystem::path& model);

/// For a model with a meta in the project: writes its textures and a material instance per slot,
/// and names those in its meta. Textures already in the project are used in place, and files
/// already there are reused rather than overwritten, so running it again changes nothing.
/// `warnings` gets what was left out. Nothing stays behind on failure.
[[nodiscard]] ENGINE_API bool ExtractModelMaterials(const std::filesystem::path& model,
                                                    const ModelMaterialSettings& settings,
                                                    std::vector<std::string>& warnings, std::string& error);

/// Copies `model` into the new folder `folder` as model.<ext> with its companions, gives it a
/// meta, then extracts its materials. All or nothing.
[[nodiscard]] ENGINE_API bool ImportModel(const std::filesystem::path& model, const std::filesystem::path& folder,
                                          bool centerOnLoad, const ModelMaterialSettings& settings,
                                          std::vector<std::string>& warnings, std::string& error);

}  // namespace engine
