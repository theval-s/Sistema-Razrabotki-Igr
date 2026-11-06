#pragma once
#include <SimpleMath.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <engine/engine_export.hpp>

#include <engine/rendering/RenderingResourceManager.hpp>

namespace engine {
namespace ugly_utils {
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Vector4;
using DirectX::SimpleMath::Matrix;

struct VertData {
    Vector4 pos;
    Vector3 norm;
    Vector2 uv;
};

struct RawMesh {
    std::vector<VertData> vertexes;
    std::vector<uint32_t> indicies;
};

ENGINE_API MeshHandle FromRawMesh(RenderingResourceManager& resourceManager, const RawMesh& rawMesh);
ENGINE_API RawMesh CreateSphere(float size);
ENGINE_API RawMesh CreateCube(float sizeX, float sizeY, float sizeZ);

}
}
