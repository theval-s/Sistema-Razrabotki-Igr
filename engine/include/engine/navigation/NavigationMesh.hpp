#pragma once

#include <memory>
#include <span>
#include <vector>
#include <SimpleMath.h>
#include <engine/engine_export.hpp>

namespace engine {

struct NavigationSettings {
    float cellSize = 0.2f;
    float cellHeight = 0.1f;
    float agentHeight = 2.0f;
    float agentRadius = 0.4f;
    float agentClimb = 0.4f;
    float agentSlope = 45.0f;
    bool operator==(const NavigationSettings&) const = default;
};

struct NavigationRegion {
    DirectX::SimpleMath::Vector3 halfExtents{10, 3, 10};
    NavigationSettings settings;
    bool operator==(const NavigationRegion&) const = default;
};

struct NavigationSource {
    bool useCollider = true;
    bool operator==(const NavigationSource&) const = default;
};

class DebugDraw;

/// A single navigation mesh for a humanoid agent. Use from one thread at a time.
class ENGINE_API NavigationMesh {
public:
    NavigationMesh();
    ~NavigationMesh();

    /// World-space vertices in meters and triangle indices with upward-facing
    /// winding on walkable surfaces. A failed build preserves the previous mesh.
    [[nodiscard]] bool Build(std::span<const DirectX::SimpleMath::Vector3> vertices,
                             std::span<const int> indices, const NavigationSettings& settings = {});
    [[nodiscard]] bool BuildRegion(std::span<const DirectX::SimpleMath::Vector3> vertices,
        std::span<const int> indices, const NavigationSettings& settings,
        DirectX::SimpleMath::Vector3 minimum, DirectX::SimpleMath::Vector3 maximum);
    void Clear();
    void Draw(DebugDraw& draw) const;

    /// Projects both endpoints onto the mesh. Returns an empty path if either
    /// projection fails, the goal is unreachable, or the query exceeds capacity.
    [[nodiscard]] std::vector<DirectX::SimpleMath::Vector3> FindPath(
        DirectX::SimpleMath::Vector3 start, DirectX::SimpleMath::Vector3 goal) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}
