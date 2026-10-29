#pragma once

#include <string_view>

#include <entt/entity/entity.hpp>

#include <engine/core/Subsystem.hpp>

namespace engine {
class InputSubsystem;
}

namespace editor {

/// Viewport navigation: flies the scene's first engine::Camera entity.
///   hold RMB    mouse-look, WASD move, Q/E down/up, Shift faster, wheel = speed
///   wheel       (without RMB) dolly forward/back
/// Uses real time, so it keeps working while the simulation is paused.
/// Register after InputSubsystem.
class EditorCameraSubsystem final : public engine::Subsystem {
public:
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Update(const engine::FrameContext& frame) override;

private:
    engine::InputSubsystem* input_ = nullptr;
    /// The camera yaw/pitch were taken from; re-read when it changes.
    entt::entity camera_ = entt::null;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    /// Units per second.
    float speed_ = 5.0f;
};

}  // namespace editor
