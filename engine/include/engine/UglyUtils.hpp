#pragma once
#include <SimpleMath.h>
#include <WICTextureLoader.h>
#include <filesystem>
#include <fstream>

#include "RenderingResourceManager.hpp"


namespace engine {
namespace ugly_utils {
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Vector4;
using DirectX::SimpleMath::Matrix;

struct ObjVertex {
    Vector3 position{};
    Vector3 normal{};
    Vector2 uv{};
};

struct ObjModel {
    std::vector<ObjVertex> vertices{};
    std::vector<uint32_t> indices{};
    ComPtr<ID3D11ShaderResourceView> textureSRV{};
};

struct RenderingState {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
};


struct ObjIndexTriplet {
    int positionIndex = 0;
    int uvIndex = 0;
    int normalIndex = 0;
};

inline void CenterModel(ObjModel& model, bool normalize = true) {
    float maxF = 1e30;
    Vector3 minV{maxF, maxF, maxF}, maxV{};
    maxV = -minV;
    for (auto& vertex : model.vertices) {
        minV = Vector3::Min(minV, vertex.position);
        maxV = Vector3::Max(maxV, vertex.position);
    }
    Vector3 center = (minV + maxV) * 0.5f;
    if ((center - Vector3::Zero).Length() < 1e-6) {
        return; // already centered
    }
    Vector3 offset = center - Vector3::Zero;
    for (auto& vertex : model.vertices) {
        vertex.position -= offset;
    }
    if (!normalize) {
        return;
    }
    Vector3 maxV2{};
    for (auto& vertex : model.vertices) {
        maxV2 = Vector3::Max(maxV2, vertex.position);
    }
    float maxDim = max(max(maxV2.x, maxV2.y), maxV2.z);
    if (abs(maxDim - 0.5) < 1e-6) {
        return; //already normalized
    }
    float divisor = maxDim / 0.5;
    for (auto& vertex : model.vertices) {
        vertex.position /= divisor;
    }
}

inline void ObjFailAndExit(const char* message) {
    std::cout << "OBJ parser error: " << message << std::endl;
    std::exit(-1);
}

inline int ResolveObjIndex(int rawIndex, size_t count) {
    if (rawIndex > 0) {
        return rawIndex - 1;
    }
    if (rawIndex < 0) {
        return static_cast<int>(count) + rawIndex;
    }
    return -1;
}

inline ObjIndexTriplet ParseObjFaceVertexToken(const std::string& token) {
    ObjIndexTriplet result{};

    const size_t firstSlash = token.find('/');
    if (firstSlash == std::string::npos) {
        ObjFailAndExit("Face token has no UV/normal data.");
    }

    const size_t secondSlash = token.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos) {
        ObjFailAndExit("Face token must be in v/vt/vn format.");
    }

    const std::string p = token.substr(0, firstSlash);
    const std::string t = token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
    const std::string n = token.substr(secondSlash + 1);

    if (p.empty() || t.empty() || n.empty()) {
        ObjFailAndExit("Face token must contain position, UV and normal indices.");
    }

    result.positionIndex = std::stoi(p);
    result.uvIndex = std::stoi(t);
    result.normalIndex = std::stoi(n);

    return result;
}

inline uint32_t GetOrCreateObjVertexIndex(
    const ObjIndexTriplet& triplet,
    const std::vector<Vector3>& positions,
    const std::vector<Vector2>& uvs,
    const std::vector<Vector3>& normals,
    std::unordered_map<std::string, uint32_t>& vertexMap,
    std::vector<ObjVertex>& outVertices) {
    const int pIdx = ResolveObjIndex(triplet.positionIndex, positions.size());
    const int tIdx = ResolveObjIndex(triplet.uvIndex, uvs.size());
    const int nIdx = ResolveObjIndex(triplet.normalIndex, normals.size());

    if (pIdx < 0 || pIdx >= static_cast<int>(positions.size())) {
        ObjFailAndExit("Position index is out of range.");
    }
    if (tIdx < 0 || tIdx >= static_cast<int>(uvs.size())) {
        ObjFailAndExit("UV index is out of range (model must have UVs).");
    }
    if (nIdx < 0 || nIdx >= static_cast<int>(normals.size())) {
        ObjFailAndExit("Normal index is out of range.");
    }

    const std::string key =
        std::to_string(pIdx) + "/" + std::to_string(tIdx) + "/" + std::to_string(nIdx);

    const auto it = vertexMap.find(key);
    if (it != vertexMap.end()) {
        return it->second;
    }

    const uint32_t newIndex = static_cast<uint32_t>(outVertices.size());
    outVertices.push_back(ObjVertex{
        .position = positions[pIdx],
        .normal = normals[nIdx],
        .uv = uvs[tIdx]
    });
    vertexMap.emplace(key, newIndex);
    return newIndex;
}

inline void LoadTexture(RenderingState& state, const std::filesystem::path& texturePath,
                        ComPtr<ID3D11ShaderResourceView>& texture) {
    HRESULT textureHr = DirectX::CreateWICTextureFromFileEx(
        state.device,
        state.context,
        texturePath.c_str(),
        0,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE,
        0,
        0,
        DirectX::WIC_LOADER_FORCE_SRGB,
        nullptr,
        texture.GetAddressOf()
    );
    if (FAILED(textureHr)) {
        ObjFailAndExit("Failed to load texture.png via CreateWICTextureFromFile.");
    }
}

inline void LoadTexture(RenderingState& state, const wchar_t* path, ComPtr<ID3D11ShaderResourceView>& texture) {
    const std::filesystem::path filePath(path);
    LoadTexture(state, filePath, texture);
}

inline ObjModel LoadObjModel(RenderingState& state, const wchar_t* folder, bool doCentering = false) {
    if (folder == nullptr) {
        ObjFailAndExit("Folder path is null.");
    }

    const std::filesystem::path folderPath(folder);
    const std::filesystem::path objPath = folderPath / L"model.obj";
    const std::filesystem::path texturePath = folderPath / L"texture.png";

    std::ifstream objFile(objPath);
    if (!objFile.is_open()) {
        ObjFailAndExit("Failed to open model.obj.");
    }

    std::vector<Vector3> positions;
    std::vector<Vector2> uvs;
    std::vector<Vector3> normals;
    std::unordered_map<std::string, uint32_t> vertexMap;
    ObjModel model{};

    std::string line;
    while (std::getline(objFile, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream lineStream(line);
        std::string prefix;
        lineStream >> prefix;
        if (prefix.empty()) {
            continue;
        }

        if (prefix == "v") {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            lineStream >> x >> y >> z;
            if (!lineStream) {
                ObjFailAndExit("Invalid vertex position line.");
            }
            positions.push_back(Vector3{x, y, z});
        } else if (prefix == "vt") {
            float u = 0.0f;
            float v = 0.0f;
            lineStream >> u >> v;
            if (!lineStream) {
                ObjFailAndExit("Invalid UV line.");
            }
            uvs.push_back(Vector2{u, 1.0f - v});
        } else if (prefix == "vn") {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            lineStream >> x >> y >> z;
            if (!lineStream) {
                ObjFailAndExit("Invalid normal line.");
            }
            normals.push_back(Vector3{x, y, z});
        } else if (prefix == "f") {
            std::vector<uint32_t> faceIndices;
            std::string token;
            while (lineStream >> token) {
                const ObjIndexTriplet triplet = ParseObjFaceVertexToken(token);
                const uint32_t index = GetOrCreateObjVertexIndex(
                    triplet,
                    positions,
                    uvs,
                    normals,
                    vertexMap,
                    model.vertices
                );
                faceIndices.push_back(index);
            }

            if (faceIndices.size() < 3) {
                ObjFailAndExit("Face has fewer than 3 vertices.");
            }

            for (size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                model.indices.push_back(faceIndices[i]);
                model.indices.push_back(faceIndices[0]);
                model.indices.push_back(faceIndices[i + 1]);
            }
        } else if (prefix == "mtllib" || prefix == "s" || prefix == "g") {
            continue;
        }
    }

    if (model.vertices.empty() || model.indices.empty()) {
        ObjFailAndExit("Parsed model is empty.");
    }
    if (uvs.empty()) {
        ObjFailAndExit("Model has no UV data.");
    }

    LoadTexture(state, texturePath, model.textureSRV);
    if (doCentering) {
        CenterModel(model);
    }
    return model;
}

struct VertData {
    Vector4 pos;
    Vector3 norm;
    Vector2 uv;
};

struct RawMesh {
    std::vector<VertData> vertexes;
    std::vector<uint32_t> indicies;
};

MeshHandle FromRawMesh(RenderingResourceManager& resourceManager, const RawMesh& rawMesh) {
    auto vertexBuffer = resourceManager.CreateVertexBuffer<VertData>(rawMesh.vertexes);
    auto indexBuffer = resourceManager.CreateIndexBuffer<uint32_t>(rawMesh.indicies);
    VertexLayout layout;
    layout.Add("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    layout.Add("NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 16);
    layout.Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 + 12);

    return resourceManager.CreateMesh(vertexBuffer, indexBuffer, std::move(layout), static_cast<uint32_t>(rawMesh.indicies.size()));
}


inline RawMesh CreateSphere(float size) {
    RawMesh mesh;

    constexpr uint32_t slices = 32;
    constexpr uint32_t stacks = 16;
    constexpr float PI = 3.14159265358979323846f;

    // Duplicate the seam vertex so U can cleanly go from 0 -> 1.
    mesh.vertexes.reserve((stacks + 1) * (slices + 1));
    mesh.indicies.reserve(stacks * slices * 6);

    for (uint32_t y = 0; y <= stacks; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(stacks);
        const float theta = v * PI;

        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for (uint32_t x = 0; x <= slices; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(slices);
            const float phi = u * 2.0f * PI;

            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);

            Vector3 normal{
                sinTheta * cosPhi,
                cosTheta,
                sinTheta * sinPhi
            };

            VertData vert;
            vert.pos = Vector4{
                normal.x * size,
                normal.y * size,
                normal.z * size,
                1.0f
            };

            vert.norm = normal;

            // Flip V here if your texture coordinate convention requires it.
            vert.uv = Vector2{
                u,
                v
            };

            mesh.vertexes.push_back(vert);
        }
    }

    const uint32_t stride = slices + 1;

    for (uint32_t y = 0; y < stacks; ++y) {
        for (uint32_t x = 0; x < slices; ++x) {
            const uint32_t a = y * stride + x;
            const uint32_t b = (y + 1) * stride + x;
            const uint32_t c = a + 1;
            const uint32_t d = b + 1;

            // Avoid degenerate triangles at the top pole.
            if (y != 0) {
                mesh.indicies.push_back(a);
                mesh.indicies.push_back(c);
                mesh.indicies.push_back(b);
            }

            // Avoid degenerate triangles at the bottom pole.
            if (y != stacks - 1) {
                mesh.indicies.push_back(c);
                mesh.indicies.push_back(d);
                mesh.indicies.push_back(b);
            }
        }
    }

    return mesh;
}

inline RawMesh CreateCube(float sizeX, float sizeY, float sizeZ) {
    RawMesh mesh;

    const float hx = sizeX * 0.5f;
    const float hy = sizeY * 0.5f;
    const float hz = sizeZ * 0.5f;

    // A cube needs separate vertices per face because each face has a
    // different normal and its own 0..1 UV rectangle.
    mesh.vertexes.reserve(24);
    mesh.indicies.reserve(36);

    auto AddFace = [&mesh](
        const Vector3& p0,
        const Vector3& p1,
        const Vector3& p2,
        const Vector3& p3,
        const Vector3& normal){
        const uint32_t base =
            static_cast<uint32_t>(mesh.vertexes.size());

        mesh.vertexes.push_back({
            Vector4{p0.x, p0.y, p0.z, 1.0f},
            normal,
            Vector2{0.0f, 0.0f}
        });

        mesh.vertexes.push_back({
            Vector4{p1.x, p1.y, p1.z, 1.0f},
            normal,
            Vector2{1.0f, 0.0f}
        });

        mesh.vertexes.push_back({
            Vector4{p2.x, p2.y, p2.z, 1.0f},
            normal,
            Vector2{1.0f, 1.0f}
        });

        mesh.vertexes.push_back({
            Vector4{p3.x, p3.y, p3.z, 1.0f},
            normal,
            Vector2{0.0f, 1.0f}
        });

        mesh.indicies.push_back(base + 0);
        mesh.indicies.push_back(base + 1);
        mesh.indicies.push_back(base + 2);

        mesh.indicies.push_back(base + 0);
        mesh.indicies.push_back(base + 2);
        mesh.indicies.push_back(base + 3);
    };

    // +Z front
    AddFace(
        Vector3{-hx, -hy, hz},
        Vector3{hx, -hy, hz},
        Vector3{hx, hy, hz},
        Vector3{-hx, hy, hz},
        Vector3{0.0f, 0.0f, 1.0f}
    );

    // -Z back
    AddFace(
        Vector3{hx, -hy, -hz},
        Vector3{-hx, -hy, -hz},
        Vector3{-hx, hy, -hz},
        Vector3{hx, hy, -hz},
        Vector3{0.0f, 0.0f, -1.0f}
    );

    // +X right
    AddFace(
        Vector3{hx, -hy, hz},
        Vector3{hx, -hy, -hz},
        Vector3{hx, hy, -hz},
        Vector3{hx, hy, hz},
        Vector3{1.0f, 0.0f, 0.0f}
    );

    // -X left
    AddFace(
        Vector3{-hx, -hy, -hz},
        Vector3{-hx, -hy, hz},
        Vector3{-hx, hy, hz},
        Vector3{-hx, hy, -hz},
        Vector3{-1.0f, 0.0f, 0.0f}
    );

    // +Y top
    AddFace(
        Vector3{-hx, hy, hz},
        Vector3{hx, hy, hz},
        Vector3{hx, hy, -hz},
        Vector3{-hx, hy, -hz},
        Vector3{0.0f, 1.0f, 0.0f}
    );

    // -Y bottom
    AddFace(
        Vector3{-hx, -hy, -hz},
        Vector3{hx, -hy, -hz},
        Vector3{hx, -hy, hz},
        Vector3{-hx, -hy, hz},
        Vector3{0.0f, -1.0f, 0.0f}
    );

    return mesh;
}
}
}
