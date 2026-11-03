#pragma once

#include <SimpleMath.h>
#include <entt/entity/entity.hpp>
#include <cstdint>

namespace engine {
enum class PhysicsContactKind { CollisionEnter, CollisionExit, TriggerEnter, TriggerExit };

/// One notification per entity pair and fixed step. The normal points from first to second.
struct PhysicsContactEvent {
    PhysicsContactKind kind;
    entt::entity first, second;
    DirectX::SimpleMath::Vector3 point, normal;
    std::uint64_t stepIndex;
};
} // namespace engine
