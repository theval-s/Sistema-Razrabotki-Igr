#pragma once

#include <string>
#include <vector>

struct aiMaterial;
struct aiMesh;
struct aiScene;

namespace engine::detail {

/// A source material that triangles use. Mesh importers and model inspection number slots the same way.
struct ModelSlot {
    unsigned sourceMaterial = 0;
    std::string name;
    unsigned uvChannel = 0; // the UV set its base color texture samples
};

struct ModelSlots {
    std::vector<ModelSlot> slots;           // in source material order, unused materials skipped
    std::vector<int> slotOfMaterial;        // per source material: its slot, or -1
};

/// A mesh the static importer draws: triangles with positions.
[[nodiscard]] bool IsTriangleMesh(const aiMesh* mesh);
/// The UV set a material's base color (or diffuse) texture samples; 0 without one.
[[nodiscard]] unsigned BaseColorUvChannel(const aiMaterial& material);
[[nodiscard]] ModelSlots CollectModelSlots(const aiScene& scene);
/// `preferred` when the mesh has it, else 0 when it has that, else -1 (no UVs at all).
[[nodiscard]] int UvChannelFor(const aiMesh& mesh, unsigned preferred);

} // namespace engine::detail
