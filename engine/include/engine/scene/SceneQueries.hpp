#pragma once
#include <vector>
#include <SimpleMath.h>
#include <entt/entity/entity.hpp>
#include <engine/assets/AssetTypes.hpp>

namespace engine {
struct Bounds { Aabb local; };
struct RayHit { entt::entity entity; float distance; };
class Scene;
class AssetSystem;

// CPU-only broad-phase queries. Hits are based on world AABBs, not mesh triangles.
[[nodiscard]] std::vector<entt::entity> QuerySphere(const Scene& scene, AssetSystem& assets,
    DirectX::SimpleMath::Vector3 center, float radius);
[[nodiscard]] std::vector<RayHit> RaycastAll(const Scene& scene, AssetSystem& assets,
    DirectX::SimpleMath::Vector3 origin, DirectX::SimpleMath::Vector3 direction, float maxDistance);
}
