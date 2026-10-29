#include <engine/scene/SceneQueries.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <algorithm>
#include <cmath>
#include <DirectXCollision.h>

namespace engine {
namespace {
using Vector3 = DirectX::SimpleMath::Vector3;

DirectX::BoundingBox WorldBounds(const Aabb& local, const DirectX::SimpleMath::Matrix& matrix) {
    DirectX::BoundingBox box, result;
    DirectX::BoundingBox::CreateFromPoints(box, local.min, local.max);
    box.Transform(result, matrix);
    return result;
}

template <class Fn>
void ForBounds(const Scene& scene, AssetSystem& assets, Fn&& fn) {
    auto visit = [&](auto&& self, entt::entity entity) -> void {
        const Aabb* local = nullptr;
        if (const auto* renderer = scene.TryGetComponent<MeshRenderer>(entity))
            if (const auto mesh = assets.Peek(renderer->mesh)) local = &mesh->bounds;
        if (!local) if (const auto* bounds = scene.TryGetComponent<Bounds>(entity)) local = &bounds->local;
        if (local) fn(entity, WorldBounds(*local, scene.GetWorldMatrix(entity)));
        for (const auto child : scene.GetChildren(entity)) self(self, child);
    };
    for (const auto root : scene.GetRootEntities()) visit(visit, root);
}
}

std::vector<entt::entity> QuerySphere(const Scene& scene, AssetSystem& assets, Vector3 center, float radius) {
    std::vector<entt::entity> hits;
    if (!std::isfinite(radius) || radius < 0 || !std::isfinite(center.x) ||
        !std::isfinite(center.y) || !std::isfinite(center.z)) return hits;
    const DirectX::BoundingSphere sphere(center, radius);
    ForBounds(scene, assets, [&](entt::entity entity, const DirectX::BoundingBox& box) {
        if (box.Intersects(sphere)) hits.push_back(entity);
    });
    return hits;
}

std::vector<RayHit> RaycastAll(const Scene& scene, AssetSystem& assets,
                              Vector3 origin, Vector3 direction, float maxDistance) {
    std::vector<RayHit> hits;
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        direction.LengthSquared() <= 0 || !std::isfinite(maxDistance) || maxDistance < 0) return hits;
    direction.Normalize();
    ForBounds(scene, assets, [&](entt::entity entity, const DirectX::BoundingBox& box) {
        float distance;
        if (!box.Intersects(origin, direction, distance)) return;
        // DirectXCollision returns a negative entry distance for an origin inside
        // the box; scene queries report an immediate hit in that case.
        distance = std::max(0.0f, distance);
        if (distance <= maxDistance) hits.push_back({entity, distance});
    });
    std::stable_sort(hits.begin(), hits.end(), [](const RayHit& a, const RayHit& b) { return a.distance < b.distance; });
    return hits;
}
} // namespace engine
