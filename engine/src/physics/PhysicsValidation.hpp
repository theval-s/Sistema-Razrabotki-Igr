#pragma once
#include <string>
#include <engine/physics/PhysicsComponents.hpp>
#include <engine/scene/Transform.hpp>

namespace engine::detail {
struct PhysicsPose {
    DirectX::SimpleMath::Vector3 scale;
    DirectX::SimpleMath::Quaternion rotation;
    DirectX::SimpleMath::Vector3 position;
};
bool Finite(DirectX::SimpleMath::Vector3 value);
bool ValidatePhysics(const RigidBody& body, const BoxCollider& collider, std::string& error);
bool GetPhysicsPose(const DirectX::SimpleMath::Matrix& matrix, PhysicsPose& pose, std::string& error);
} // namespace engine::detail
