#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <entt/entity/entity.hpp>
#include <engine/core/Subsystem.hpp>
#include <engine/physics/PhysicsComponents.hpp>

namespace engine {
class DebugDraw;

struct PhysicsConfig {
    DirectX::SimpleMath::Vector3 gravity{0, -9.81f, 0};
    std::uint32_t maxBodies = 4096;
    std::uint32_t maxBodyPairs = 16384;
    std::uint32_t maxContactConstraints = 8192;
    std::uint32_t tempAllocatorBytes = 16 * 1024 * 1024;
    std::uint32_t workerThreads = 1; // bounded independently of enkiTS
};

/// Register after gameplay subsystems which apply forces during FixedUpdate.
/// All public operations are main-thread only. No ECS access from Jolt workers.
class ENGINE_API PhysicsSubsystem final : public Subsystem {
public:
    explicit PhysicsSubsystem(PhysicsConfig config = {});
    ~PhysicsSubsystem() override;
    std::string_view GetName() const override { return "PhysicsSubsystem"; }
    bool Initialize() override;
    void Shutdown() override;
    void FixedUpdate(const FixedUpdateContext& step) override;
    void ResetSimulation() override;

    [[nodiscard]] bool SetLinearVelocity(entt::entity entity, DirectX::SimpleMath::Vector3 velocity, std::string& error);
    [[nodiscard]] bool AddImpulse(entt::entity entity, DirectX::SimpleMath::Vector3 impulse, std::string& error);
    [[nodiscard]] bool MoveKinematic(entt::entity entity, DirectX::SimpleMath::Vector3 position,
                                    DirectX::SimpleMath::Quaternion rotation, std::string& error);
    [[nodiscard]] bool Teleport(entt::entity entity, DirectX::SimpleMath::Vector3 position,
                               DirectX::SimpleMath::Quaternion rotation, std::string& error);
    /// Draws the authored collider's wireframe, including in Edit mode.
    /// Uses the same Jolt shape builder as simulation; does not create a body or step physics.
    /// One preview is cached. Pass entt::null to release it when the inspector has no selection.
    [[nodiscard]] bool DrawCollider(entt::entity entity, DebugDraw& draw,
        const DirectX::SimpleMath::Color& color, std::string& error);
    [[nodiscard]] std::size_t GetBodyCount() const;
private:
    struct Impl;
    PhysicsConfig config_;
    std::unique_ptr<Impl> impl_;
};
} // namespace engine
