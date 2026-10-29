#include <engine/scene/SceneQueries.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {
namespace {
using Vector3 = DirectX::SimpleMath::Vector3;

Aabb WorldBounds(const Aabb& local, const DirectX::SimpleMath::Matrix& matrix) {
    Aabb result{{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
                {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}};
    for (int i = 0; i < 8; ++i) {
        const Vector3 point = Vector3::Transform({i & 1 ? local.max.x : local.min.x,
            i & 2 ? local.max.y : local.min.y, i & 4 ? local.max.z : local.min.z}, matrix);
        result.min = Vector3::Min(result.min, point);
        result.max = Vector3::Max(result.max, point);
    }
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
    ForBounds(scene, assets, [&](entt::entity entity, const Aabb& b) {
        Vector3 closest;
        center.Clamp(b.min, b.max, closest);
        if ((closest - center).LengthSquared() <= radius * radius) hits.push_back(entity);
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
    ForBounds(scene, assets, [&](entt::entity entity, const Aabb& b) {
        float enter = 0, exit = maxDistance;
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        const float lo[3] = {b.min.x, b.min.y, b.min.z};
        const float hi[3] = {b.max.x, b.max.y, b.max.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(d[axis]) < 1e-8f) {
                if (o[axis] < lo[axis] || o[axis] > hi[axis]) return;
            } else {
                float a = (lo[axis] - o[axis]) / d[axis];
                float c = (hi[axis] - o[axis]) / d[axis];
                if (a > c) std::swap(a, c);
                enter = std::max(enter, a);
                exit = std::min(exit, c);
                if (enter > exit) return;
            }
        }
        hits.push_back({entity, enter});
    });
    std::stable_sort(hits.begin(), hits.end(), [](const RayHit& a, const RayHit& b) { return a.distance < b.distance; });
    return hits;
}
} // namespace engine
