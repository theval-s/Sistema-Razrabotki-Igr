#include <engine/scene/SceneQueries.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/animation/AnimationComponents.hpp>
#include <engine/physics/PhysicsComponents.hpp>
#include "RayQueryUtils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
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
void ForBounds(const Scene& scene, AssetSystem& assets, Fn&& fn,
               const std::unordered_set<entt::entity>* ignored = nullptr) {
    auto visit = [&](auto&& self, entt::entity entity) -> void {
        const Aabb* local = nullptr;
        if (!ignored || !ignored->contains(entity)) {
            if (const auto* renderer = scene.TryGetComponent<MeshRenderer>(entity))
                if (const auto mesh = assets.Peek(renderer->mesh)) local = &mesh->bounds;
            if (const auto* renderer = scene.TryGetComponent<SkinnedMeshRenderer>(entity)) {
                if (renderer->posedBounds) local = &*renderer->posedBounds;
                else if (const auto mesh = assets.Peek(renderer->mesh)) local = &mesh->bounds;
            }
            if (!local) if (const auto* bounds = scene.TryGetComponent<Bounds>(entity)) local = &bounds->local;
            if (local) fn(entity, WorldBounds(*local, scene.GetWorldMatrix(entity)));
        }
        for (const auto child : scene.GetChildren(entity)) self(self, child);
    };
    for (const auto root : scene.GetRootEntities()) visit(visit, root);
}

bool IntersectBounds(const Scene& scene, entt::entity entity, const DirectX::BoundingBox& box,
                     const Ray& ray, RayHit& hit) {
    const float origins[] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float directions[] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float centers[] = {box.Center.x, box.Center.y, box.Center.z};
    const float extents[] = {box.Extents.x, box.Extents.y, box.Extents.z};
    double entry = 0, exit = ray.maxDistance;
    Vector3 normal{};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(centers[i]) || !std::isfinite(extents[i]) || extents[i] < 0) return false;
        const double low = double(centers[i]) - extents[i], high = double(centers[i]) + extents[i];
        if (directions[i] == 0) {
            if (origins[i] < low || origins[i] > high) return false;
            continue;
        }
        double entryDistance = (low - origins[i]) / directions[i];
        double exitDistance = (high - origins[i]) / directions[i];
        float sign = -1;
        if (entryDistance > exitDistance) { std::swap(entryDistance, exitDistance); sign = 1; }
        if (entryDistance > entry) {
            entry = entryDistance;
            normal = {};
            if (i == 0) normal.x = sign;
            else if (i == 1) normal.y = sign;
            else normal.z = sign;
        }
        exit = std::min(exit, exitDistance);
        if (entry > exit) return false;
    }
    hit.entity = entity;
    hit.distance = static_cast<float>(entry);
    hit.point = {static_cast<float>(double(ray.origin.x) + entry * ray.direction.x),
                 static_cast<float>(double(ray.origin.y) + entry * ray.direction.y),
                 static_cast<float>(double(ray.origin.z) + entry * ray.direction.z)};
    hit.normal = hit.distance > 0 ? std::optional<Vector3>(normal) : std::nullopt;
    const auto* collider = scene.TryGetComponent<Collider>(entity);
    hit.isTrigger = collider && collider->isTrigger;
    return true;
}

bool HitLess(const RayHit& a, const RayHit& b) {
    return a.distance < b.distance ||
        (a.distance == b.distance && entt::to_integral(a.entity) < entt::to_integral(b.entity));
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
    if (!std::isfinite(maxDistance) || maxDistance < 0) return hits;
    Ray ray;
    std::string error;
    // Legacy queries accept zero length and do not reject segments with an
    // unrepresentable endpoint; use a unit length only for normalization.
    if (!detail::NormalizeQueryRay({origin, direction, 1.0f}, ray, error))
        return hits;
    ray.maxDistance = maxDistance;
    ForBounds(scene, assets, [&](entt::entity entity, const DirectX::BoundingBox& box) {
        RayHit hit;
        if (IntersectBounds(scene, entity, box, ray, hit)) hits.push_back(hit);
    });
    std::stable_sort(hits.begin(), hits.end(), [](const RayHit& a, const RayHit& b) { return a.distance < b.distance; });
    return hits;
}

bool RaycastBounds(const Scene& scene, AssetSystem& assets, const Ray& input,
                   const QueryFilter& filter, std::optional<RayHit>& hit, std::string& error) {
    hit.reset();
    Ray ray;
    std::unordered_set<entt::entity> ignored;
    if (!detail::NormalizeQueryRay(input, ray, error) ||
        !detail::QueryIgnoredEntities(scene, filter, ignored, error)) return false;
    ForBounds(scene, assets, [&](entt::entity entity, const DirectX::BoundingBox& box) {
        RayHit candidate;
        if (IntersectBounds(scene, entity, box, ray, candidate) && (!hit || HitLess(candidate, *hit)))
            hit = candidate;
    }, &ignored);
    return true;
}

bool RaycastBoundsAll(const Scene& scene, AssetSystem& assets, const Ray& input,
                      const QueryFilter& filter, std::vector<RayHit>& hits, std::string& error) {
    hits.clear();
    Ray ray;
    std::unordered_set<entt::entity> ignored;
    if (!detail::NormalizeQueryRay(input, ray, error) ||
        !detail::QueryIgnoredEntities(scene, filter, ignored, error)) return false;
    ForBounds(scene, assets, [&](entt::entity entity, const DirectX::BoundingBox& box) {
        RayHit hit;
        if (IntersectBounds(scene, entity, box, ray, hit)) hits.push_back(hit);
    }, &ignored);
    std::sort(hits.begin(), hits.end(), HitLess);
    return true;
}

bool ScreenPointToRay(const Scene& scene, DirectX::SimpleMath::Vector2 point,
                      DirectX::SimpleMath::Vector2 size, entt::entity cameraEntity,
                      Ray& ray, std::string& error) {
    ray = {};
    auto fail = [&](const char* message) { error = message; return false; };
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0 || size.y <= 0 ||
        point.x < 0 || point.y < 0 || point.x >= size.x || point.y >= size.y)
        return fail("point must lie within a finite positive viewport");
    if (cameraEntity == entt::null) cameraEntity = scene.GetActiveCamera();
    if (!scene.IsValid(cameraEntity)) return fail("camera entity is invalid or missing");
    const auto* camera = scene.TryGetComponent<Camera>(cameraEntity);
    if (!camera) return fail("camera entity has no Camera component");
    if (!std::isfinite(camera->fovY) || camera->fovY <= 0 || camera->fovY >= DirectX::XM_PI ||
        !std::isfinite(camera->nearPlane) || !std::isfinite(camera->farPlane) ||
        camera->nearPlane <= 0 || camera->farPlane <= camera->nearPlane)
        return fail("camera projection is invalid");
    const auto matrix = scene.GetWorldMatrix(cameraEntity);
    const Vector3 position = matrix.Translation();
    Vector3 forward = Vector3::TransformNormal(Vector3::Forward, matrix);
    if (!detail::FiniteQueryVector(position) || !detail::FiniteQueryVector(forward))
        return fail("camera pose is invalid");
    const double length = std::sqrt(double(forward.x)*forward.x + double(forward.y)*forward.y + double(forward.z)*forward.z);
    if (!(length > 0) || !std::isfinite(length)) return fail("camera pose is invalid");
    forward = {float(forward.x / length), float(forward.y / length), float(forward.z / length)};
    const float pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    const float yaw = std::atan2(-forward.x, -forward.z);
    const auto rotation = DirectX::SimpleMath::Matrix::CreateFromYawPitchRoll(yaw, pitch, 0);
    forward = Vector3::TransformNormal(Vector3::Forward, rotation);
    Vector3 right = forward.Cross(Vector3::Up);
    const float rightLength = right.Length();
    if (!std::isfinite(rightLength) || rightLength < 1e-6f) return fail("camera pose has a singular view");
    right /= rightLength;
    const Vector3 up = right.Cross(forward);
    const double halfHeight = std::tan(double(camera->fovY) * 0.5);
    const double x = (2.0 * point.x / size.x - 1.0) * (double(size.x) / size.y) * halfHeight;
    const double y = (1.0 - 2.0 * point.y / size.y) * halfHeight;
    const double dx = forward.x + x * right.x + y * up.x;
    const double dy = forward.y + x * right.y + y * up.y;
    const double dz = forward.z + x * right.z + y * up.z;
    const double scale = std::sqrt(dx*dx + dy*dy + dz*dz);
    const double distance = (double(camera->farPlane) - camera->nearPlane) * scale;
    const double ox = position.x + camera->nearPlane * dx;
    const double oy = position.y + camera->nearPlane * dy;
    const double oz = position.z + camera->nearPlane * dz;
    const double limit = std::numeric_limits<float>::max();
    if (!std::isfinite(scale) || !std::isfinite(distance) || distance > limit ||
        std::abs(ox) > limit || std::abs(oy) > limit || std::abs(oz) > limit)
        return fail("camera ray exceeds the supported range");
    Ray result{{float(ox), float(oy), float(oz)},
               {float(dx / scale), float(dy / scale), float(dz / scale)}, float(distance)};
    return detail::NormalizeQueryRay(result, ray, error);
}
} // namespace engine
