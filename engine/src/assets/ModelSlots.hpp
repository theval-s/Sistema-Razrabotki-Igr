#pragma once

#include <string>
#include <vector>

#include <assimp/postprocess.h>

struct aiMaterial;
struct aiMesh;
struct aiScene;

namespace engine::detail {

// What decides which meshes a static model keeps, so inspection sees the slots the importer makes.
constexpr unsigned StaticMeshStructureFlags = aiProcess_Triangulate | aiProcess_PreTransformVertices |
                                              aiProcess_SortByPType;

/// The texture a material's base color (or else diffuse) samples.
struct BaseColorTexture {
    std::string path;       // as the model names it: a file, or "*N" for an embedded image; empty for none
    unsigned uvChannel = 0;
    bool transformed = false;  // has a UV transform (KHR_texture_transform), which we don't apply
};

/// A source material that triangles use. Mesh importers and model inspection number slots the same way.
struct ModelSlot {
    unsigned sourceMaterial = 0;
    std::string name;
    BaseColorTexture texture;
};

struct ModelSlots {
    std::vector<ModelSlot> slots;           // in source material order, unused materials skipped
    std::vector<int> slotOfMaterial;        // per source material: its slot, or -1
};

/// A mesh the static importer draws: triangles with positions.
[[nodiscard]] bool IsTriangleMesh(const aiMesh* mesh);
[[nodiscard]] BaseColorTexture FindBaseColorTexture(const aiMaterial& material);
[[nodiscard]] ModelSlots CollectModelSlots(const aiScene& scene);
/// `preferred` when the mesh has it, else 0 when it has that, else -1 (no UVs at all).
[[nodiscard]] int UvChannelFor(const aiMesh& mesh, unsigned preferred);

} // namespace engine::detail
