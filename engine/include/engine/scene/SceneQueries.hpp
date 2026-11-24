#pragma once
#include <vector>
#include <SimpleMath.h>
#include <entt/entity/entity.hpp>
#include <engine/assets/AssetTypes.hpp>
#include <engine/engine_export.hpp>
#include <engine/scene/RayQuery.hpp>
#include <string>

namespace engine {
struct Bounds {
    Aabb local;
    bool operator==(const Bounds&) const = default;
};
class Scene;
class AssetSystem;

// CPU-only broad-phase queries. Hits are based on world AABBs, not mesh triangles.
[[nodiscard]] ENGINE_API std::vector<entt::entity> QuerySphere(const Scene& scene, AssetSystem& assets,
    DirectX::SimpleMath::Vector3 center, float radius);
[[nodiscard]] ENGINE_API std::vector<RayHit> RaycastAll(const Scene& scene, AssetSystem& assets,
    DirectX::SimpleMath::Vector3 origin, DirectX::SimpleMath::Vector3 direction, float maxDistance);

/// World-AABB picking, including nonphysical entities. Success with no hit is not an error.
/// maxDistance must be positive. All hits contains one entry per entity, sorted by distance.
[[nodiscard]] ENGINE_API bool RaycastBounds(const Scene& scene, AssetSystem& assets,
    const Ray& ray, const QueryFilter& filter, std::optional<RayHit>& hit, std::string& error);
[[nodiscard]] ENGINE_API bool RaycastBoundsAll(const Scene& scene, AssetSystem& assets,
    const Ray& ray, const QueryFilter& filter, std::vector<RayHit>& hits, std::string& error);

/// Viewport-local pixels, top-left origin, +Y down. Uses the active camera when camera is null.
/// The returned ray starts on the near plane and ends on the far plane.
[[nodiscard]] ENGINE_API bool ScreenPointToRay(const Scene& scene,
    DirectX::SimpleMath::Vector2 point, DirectX::SimpleMath::Vector2 viewportSize,
    entt::entity camera, Ray& ray, std::string& error);
}
