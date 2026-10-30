#include <engine/rendering/UglyUtils.hpp>

#include <cmath>
#include <utility>

namespace engine {
namespace ugly_utils {

MeshHandle FromRawMesh(RenderingResourceManager& resourceManager, const RawMesh& rawMesh) {
    auto vertexBuffer = resourceManager.CreateVertexBuffer<VertData>(rawMesh.vertexes);
    auto indexBuffer = resourceManager.CreateIndexBuffer<uint32_t>(rawMesh.indicies);
    VertexLayout layout;
    layout.Add("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    layout.Add("NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 16);
    layout.Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 + 12);

    return resourceManager.CreateMesh(vertexBuffer, indexBuffer, std::move(layout),
                                      static_cast<uint32_t>(rawMesh.indicies.size()));
}

RawMesh CreateSphere(const float size) {
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
                mesh.indicies.push_back(b);
                mesh.indicies.push_back(c);
            }

            // Avoid degenerate triangles at the bottom pole.
            if (y != stacks - 1) {
                mesh.indicies.push_back(c);
                mesh.indicies.push_back(b);
                mesh.indicies.push_back(d);
            }
        }
    }

    return mesh;
}

RawMesh CreateCube(const float sizeX, const float sizeY, const float sizeZ) {
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
        mesh.indicies.push_back(base + 2);
        mesh.indicies.push_back(base + 1);

        mesh.indicies.push_back(base + 0);
        mesh.indicies.push_back(base + 3);
        mesh.indicies.push_back(base + 2);
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
