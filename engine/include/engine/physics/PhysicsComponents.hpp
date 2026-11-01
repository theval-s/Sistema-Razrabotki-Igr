#pragma once
#include <SimpleMath.h>

namespace engine {
enum class MotionType { Static, Dynamic, Kinematic };

// Authoring data only. Runtime Jolt bodies belong to PhysicsSubsystem.
struct RigidBody {
    MotionType motionType = MotionType::Static;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    bool operator==(const RigidBody&) const = default;
};
struct BoxCollider {
    DirectX::SimpleMath::Vector3 center{};
    DirectX::SimpleMath::Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    bool operator==(const BoxCollider&) const = default;
};
} // namespace engine
