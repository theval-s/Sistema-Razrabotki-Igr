#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {

using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Vector4;

// VertData is a flat triangle list with one UV set, so: triangulate, bake the node
// transforms into the geometry, keep only mesh primitives, and make sure every vertex
// has a normal (GenSmoothNormals only writes the vertices that came without one).
//
// FlipUVs and FlipWindingOrder are not cosmetic, they are what the old hand-written parser
// did by hand: OBJ stores V measured from the bottom of the image while our samplers address
// the first row as V=0, and the parser emitted every fan triangle as (i, 0, i+1), i.e. with
// the opposite winding to the file. Assimp hands the file's own conventions over unless
// asked, which rendered every texture upside-down and every surface back-to-front.
constexpr unsigned int ImportFlags = aiProcess_Triangulate | aiProcess_PreTransformVertices |
                                     aiProcess_GenSmoothNormals | aiProcess_SortByPType |
                                     aiProcess_FlipUVs | aiProcess_FlipWindingOrder |
                                     static_cast<unsigned int>(aiProcess_GenBoundingBoxes);

/// Recenters the geometry on its bounding-box center and scales it to a 0.5 half-extent,
/// the same convention AssetDesc::centerOnLoad had with the old parser.
void CenterAndNormalize(MeshCpu& mesh) {
    const Vector3 min = mesh.bounds.min;
    const Vector3 max = mesh.bounds.max;

    const Vector3 center = (min + max) * 0.5f;
    const Vector3 halfExtent = (max - min) * 0.5f;
    const float largest = (std::max)({halfExtent.x, halfExtent.y, halfExtent.z});
    const float scale = largest > 1e-6f ? 0.5f / largest : 1.0f;
    for (ugly_utils::VertData& vertex : mesh.vertices) {
        vertex.pos = Vector4{(vertex.pos.x - center.x) * scale, (vertex.pos.y - center.y) * scale,
                             (vertex.pos.z - center.z) * scale, 1.0f};
    }
    mesh.bounds.min = (min - center) * scale;
    mesh.bounds.max = (max - center) * scale;
}

}  // namespace

bool AssimpMeshImporter::ReadCpu(const AssetDesc& desc, ID3D11Device* /*device*/, CpuPayload& payload,
                                 std::string& error) const {
    const std::filesystem::path path = AssetPathResolver::ResolveMesh(desc);

    // A per-call Importer: Assimp is only safe for concurrent loads when instances are not
    // shared, and several assets can be reading on different workers at the same time.
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(ToUtf8(path), ImportFlags);
    if (scene == nullptr || scene->mRootNode == nullptr) {
        error = std::format("Assimp failed to read '{}': {}", ToUtf8(path), importer.GetErrorString());
        return false;
    }

    MeshCpu mesh;
    bool hasUVs = false;
    for (const aiMesh* source : std::span(scene->mMeshes, scene->mNumMeshes)) {
        if (source == nullptr || !source->HasPositions() || !source->HasFaces()) {
            continue;
        }
        const uint32_t baseVertex = static_cast<uint32_t>(mesh.vertices.size());
        const aiAABB& box = source->mAABB;
        const Aabb bounds{Vector3{box.mMin.x, box.mMin.y, box.mMin.z},
                          Vector3{box.mMax.x, box.mMax.y, box.mMax.z}};
        if (mesh.vertices.empty()) {
            mesh.bounds = bounds;
        } else {
            mesh.bounds.min = Vector3::Min(mesh.bounds.min, bounds.min);
            mesh.bounds.max = Vector3::Max(mesh.bounds.max, bounds.max);
        }
        hasUVs = hasUVs || source->HasTextureCoords(0);
        for (uint32_t index = 0; index < source->mNumVertices; ++index) {
            const aiVector3D& position = source->mVertices[index];
            const aiVector3D normal = source->HasNormals() ? source->mNormals[index] : aiVector3D{0.0f, 1.0f, 0.0f};
            const aiVector3D uv = source->HasTextureCoords(0) ? source->mTextureCoords[0][index]
                                                              : aiVector3D{0.0f, 0.0f, 0.0f};
            mesh.vertices.push_back(ugly_utils::VertData{
                .pos = Vector4{position.x, position.y, position.z, 1.0f},
                .norm = Vector3{normal.x, normal.y, normal.z},
                .uv = Vector2{uv.x, uv.y}
            });
        }
        for (uint32_t face = 0; face < source->mNumFaces; ++face) {
            const aiFace& source_face = source->mFaces[face];
            for (uint32_t index = 0; index < source_face.mNumIndices; ++index) {
                mesh.indices.push_back(baseVertex + source_face.mIndices[index]);
            }
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = std::format("Assimp read '{}' but it contains no triangles", ToUtf8(path));
        return false;
    }
    if (!hasUVs) {
        error = std::format("'{}' has no UV channel, the renderer needs one", ToUtf8(path));
        return false;
    }
    if (desc.centerOnLoad) {
        CenterAndNormalize(mesh);
    }

    payload = CpuPayload{};
    payload.kind = CpuPayload::Kind::Mesh;
    payload.mesh = std::move(mesh);
    return true;
}

bool AssimpMeshImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                   GpuPayload& result, std::string& error) const {
    if (payload.kind != CpuPayload::Kind::Mesh) {
        error = "AssimpMeshImporter: payload is not a parsed mesh";
        return false;
    }

    const auto vertexBuffer = transfer.resources->CreateVertexBuffer<ugly_utils::VertData>(
        payload.mesh.vertices);
    const auto indexBuffer = transfer.resources->CreateIndexBuffer<uint32_t>(payload.mesh.indices);

    VertexLayout layout;
    layout.Add("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    layout.Add("NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 16);
    layout.Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 + 12);

    result.mesh = transfer.resources->CreateMesh(
        vertexBuffer, indexBuffer, std::move(layout),
        static_cast<uint32_t>(payload.mesh.indices.size()));
    return true;
}

}  // namespace engine
