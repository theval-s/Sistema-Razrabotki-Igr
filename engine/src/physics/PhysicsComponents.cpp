#include "PhysicsValidation.hpp"
#include <cmath>

namespace engine::detail {
bool Finite(DirectX::SimpleMath::Vector3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool ValidatePhysics(const RigidBody& body, const Collider& collider, std::string& error) {
    const auto extents = collider.halfExtents;
    if ((body.motionType != MotionType::Static && body.motionType != MotionType::Dynamic && body.motionType != MotionType::Kinematic) ||
        !std::isfinite(body.mass) || body.mass <= 0 || !std::isfinite(body.friction) || body.friction < 0 ||
        !std::isfinite(body.restitution) || body.restitution < 0 || body.restitution > 1 ||
        !Finite(collider.center) || !Finite(extents) || extents.x <= 0 || extents.y <= 0 || extents.z <= 0) {
        error = "invalid RigidBody/Collider: require positive mass/extents, nonnegative friction and restitution in [0,1]";
        return false;
    }
    if (!std::isfinite(collider.radius) || collider.radius <= 0 ||
        !std::isfinite(collider.halfHeight) || collider.halfHeight < 0 ||
        collider.shape < ColliderShape::Cube || collider.shape > ColliderShape::TriangleMesh) {
        error = "invalid collider shape, radius or capsule half height";
        return false;
    }
    if (collider.shape == ColliderShape::TriangleMesh && body.motionType != MotionType::Static) {
        error = "triangle mesh colliders require Static; use ConvexMesh for moving bodies";
        return false;
    }
    error.clear();
    return true;
}
bool GetPhysicsPose(const DirectX::SimpleMath::Matrix& matrix, PhysicsPose& pose, std::string& error) {
    auto decomposed = matrix;
    if (!decomposed.Decompose(pose.scale, pose.rotation, pose.position) ||
        !Finite(pose.scale) || !Finite(pose.position) ||
        pose.scale.x <= 0 || pose.scale.y <= 0 || pose.scale.z <= 0 ||
        !std::isfinite(pose.rotation.LengthSquared()) || pose.rotation.LengthSquared() < 1e-8f) {
        error = "physics requires a finite transform with positive scale and valid rotation";
        return false;
    }
    pose.rotation.Normalize();
    const auto rebuilt = DirectX::SimpleMath::Matrix::CreateScale(pose.scale) *
        DirectX::SimpleMath::Matrix::CreateFromQuaternion(pose.rotation) *
        DirectX::SimpleMath::Matrix::CreateTranslation(pose.position);
    const float* a = &matrix._11;
    const float* b = &rebuilt._11;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(a[i]) || std::abs(a[i] - b[i]) > 1e-4f * (1.0f + std::abs(a[i]))) {
            error = "physics does not support sheared transforms";
            return false;
        }
    }
    error.clear();
    return true;
}
} // namespace engine::detail
