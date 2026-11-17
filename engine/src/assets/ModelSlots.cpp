#include "ModelSlots.hpp"

#include <format>

#include <assimp/material.h>
#include <assimp/scene.h>

namespace engine::detail {

bool IsTriangleMesh(const aiMesh* mesh) {
    return mesh != nullptr && mesh->HasPositions() && mesh->HasFaces() &&
           (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) != 0;
}

unsigned BaseColorUvChannel(const aiMaterial& material) {
    for (const aiTextureType type : {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}) {
        if (material.GetTextureCount(type) == 0) continue;
        int channel = 0;
        if (aiGetMaterialInteger(&material, AI_MATKEY_UVWSRC(type, 0), &channel) == AI_SUCCESS && channel > 0 &&
            channel < AI_MAX_NUMBER_OF_TEXTURECOORDS) {
            return static_cast<unsigned>(channel);
        }
        return 0;
    }
    return 0;
}

ModelSlots CollectModelSlots(const aiScene& scene) {
    ModelSlots result;
    result.slotOfMaterial.assign(scene.mNumMaterials, -1);
    std::vector<bool> used(scene.mNumMaterials, false);
    for (unsigned i = 0; i < scene.mNumMeshes; ++i) {
        const aiMesh* mesh = scene.mMeshes[i];
        if (IsTriangleMesh(mesh) && mesh->mMaterialIndex < scene.mNumMaterials) used[mesh->mMaterialIndex] = true;
    }
    for (unsigned material = 0; material < scene.mNumMaterials; ++material) {
        if (!used[material]) continue;
        ModelSlot slot;
        slot.sourceMaterial = material;
        if (const aiMaterial* source = scene.mMaterials[material]) {
            aiString name;
            if (source->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) slot.name = name.C_Str();
            slot.uvChannel = BaseColorUvChannel(*source);
        }
        if (slot.name.empty()) slot.name = std::format("Slot {}", result.slots.size());
        result.slotOfMaterial[material] = static_cast<int>(result.slots.size());
        result.slots.push_back(std::move(slot));
    }
    return result;
}

int UvChannelFor(const aiMesh& mesh, const unsigned preferred) {
    if (mesh.HasTextureCoords(preferred)) return static_cast<int>(preferred);
    return mesh.HasTextureCoords(0) ? 0 : -1;
}

} // namespace engine::detail
