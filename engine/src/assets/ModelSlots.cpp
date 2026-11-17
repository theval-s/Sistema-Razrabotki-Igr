#include "ModelSlots.hpp"

#include <format>

#include <assimp/material.h>
#include <assimp/scene.h>

namespace engine::detail {

bool IsTriangleMesh(const aiMesh* mesh) {
    return mesh != nullptr && mesh->HasPositions() && mesh->HasFaces() &&
           (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) != 0;
}

BaseColorTexture FindBaseColorTexture(const aiMaterial& material) {
    BaseColorTexture result;
    for (const aiTextureType type : {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}) {
        aiString path;
        if (material.GetTexture(type, 0, &path) != AI_SUCCESS || path.length == 0) continue;
        result.path = path.C_Str();
        aiUVTransform transform;
        result.transformed = aiGetMaterialUVTransform(&material, AI_MATKEY_UVTRANSFORM(type, 0), &transform) ==
                                 AI_SUCCESS &&
                             (transform.mTranslation != aiVector2D(0, 0) || transform.mScaling != aiVector2D(1, 1) ||
                              transform.mRotation != 0);
        int channel = 0;
        if (aiGetMaterialInteger(&material, AI_MATKEY_UVWSRC(type, 0), &channel) == AI_SUCCESS && channel > 0 &&
            channel < AI_MAX_NUMBER_OF_TEXTURECOORDS) {
            result.uvChannel = static_cast<unsigned>(channel);
        }
        break;
    }
    return result;
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
            slot.texture = FindBaseColorTexture(*source);
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
