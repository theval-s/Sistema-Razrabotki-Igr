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
enum class ColliderShape { Cube, Box, Sphere, Capsule, ConvexMesh, TriangleMesh };

struct Collider {
    DirectX::SimpleMath::Vector3 center{};
    DirectX::SimpleMath::Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    ColliderShape shape = ColliderShape::Box;
    float radius = 0.5f; // sphere/capsule
    float halfHeight = 0.5f; // capsule cylinder, along local Y (without caps)
    bool showDebug = true; // editor wireframe; does not affect simulation
    bool isTrigger = false;
    bool operator==(const Collider&) const = default;
};
} // namespace engine
