#pragma once

#include <optional>
#include <vector>
#include <SimpleMath.h>
#include <entt/entity/entity.hpp>

namespace engine {

/// World-space ray segment. Query functions normalize direction; maxDistance is in metres.
struct Ray {
    DirectX::SimpleMath::Vector3 origin{};
    DirectX::SimpleMath::Vector3 direction{0, 0, -1};
    float maxDistance = 100.0f;
};

/// A snapshot of one entity's nearest intersection.
struct RayHit {
    entt::entity entity = entt::null;
    float distance = 0;
    DirectX::SimpleMath::Vector3 point{};
    /// No surface normal is defined for an immediate (distance == 0) hit.
    std::optional<DirectX::SimpleMath::Vector3> normal;
    bool isTrigger = false;
};

struct QueryFilter {
    std::vector<entt::entity> ignore;
    bool ignoreDescendants = false;
};

} // namespace engine
