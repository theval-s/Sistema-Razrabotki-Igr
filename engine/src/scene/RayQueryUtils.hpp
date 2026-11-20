#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <engine/scene/RayQuery.hpp>
#include <engine/scene/Scene.hpp>

namespace engine::detail {

inline bool FiniteQueryVector(DirectX::SimpleMath::Vector3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool NormalizeQueryRay(const Ray& input, Ray& output, std::string& error) {
    if (!FiniteQueryVector(input.origin) || !FiniteQueryVector(input.direction) ||
        !std::isfinite(input.maxDistance) || input.maxDistance <= 0) {
        error = "ray requires finite origin/direction and a positive finite maxDistance";
        return false;
    }
    const double x = input.direction.x, y = input.direction.y, z = input.direction.z;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length == 0) {
        error = "ray direction must be nonzero";
        return false;
    }
    output = input;
    output.direction = {static_cast<float>(x / length), static_cast<float>(y / length),
        static_cast<float>(z / length)};
    const double limit = std::numeric_limits<float>::max();
    for (const double coordinate : {
        double(input.origin.x) + double(output.direction.x) * input.maxDistance,
        double(input.origin.y) + double(output.direction.y) * input.maxDistance,
        double(input.origin.z) + double(output.direction.z) * input.maxDistance}) {
        if (std::abs(coordinate) > limit) {
            error = "ray endpoint exceeds the supported coordinate range";
            return false;
        }
    }
    error.clear();
    return true;
}

inline bool QuerySegment(DirectX::SimpleMath::Vector3 start, DirectX::SimpleMath::Vector3 end,
                         Ray& ray, std::string& error) {
    if (!FiniteQueryVector(start) || !FiniteQueryVector(end)) {
        error = "linecast endpoints must be finite";
        return false;
    }
    const double x = double(end.x) - start.x, y = double(end.y) - start.y, z = double(end.z) - start.z;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length > std::numeric_limits<float>::max()) {
        error = "linecast length exceeds the supported range";
        return false;
    }
    ray = {start, {}, static_cast<float>(length)};
    if (length > 0)
        ray.direction = {static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length)};
    error.clear();
    return true;
}

inline bool QueryIgnoredEntities(const Scene& scene, const QueryFilter& filter,
                                std::unordered_set<entt::entity>& ignored, std::string& error) {
    ignored.clear();
    auto visit = [&](auto&& self, entt::entity entity) -> void {
        if (!ignored.insert(entity).second) return;
        if (filter.ignoreDescendants)
            for (const auto child : scene.GetChildren(entity)) self(self, child);
    };
    for (const auto entity : filter.ignore) {
        if (!scene.IsValid(entity)) {
            error = "ignore entries must be valid entities in the query scene";
            return false;
        }
        visit(visit, entity);
    }
    error.clear();
    return true;
}

} // namespace engine::detail
