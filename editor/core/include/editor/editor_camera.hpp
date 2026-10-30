#pragma once

#include <string_view>

#include <SimpleMath.h>

#include <engine/core/Subsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>

#include <editor/editor_core_export.hpp>

namespace engine {
class InputSubsystem;
class RenderingSubsystem;
}  // namespace engine

namespace editor {

/// Viewport navigation with a viewpoint of its own, so the scene is never changed by it:
///   hold RMB    mouse-look, WASD move, Q/E down/up, Shift faster, wheel = speed
///   wheel       (without RMB) dolly forward/back
/// Uses real time, so it keeps working while the simulation is paused.
/// Register after InputSubsystem and after RenderExtractionSubsystem: its LateUpdate
/// replaces the camera that extraction copied from the scene.
class EDITOR_CORE_API EditorCameraSubsystem final : public engine::Subsystem {
public:
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Update(const engine::FrameContext& frame) override;
    void LateUpdate(const engine::FrameContext& frame) override;

    /// On the next frame, starts from the scene's active camera, or from the default
    /// viewpoint when the scene has none. Call after a scene was opened.
    void ResetToSceneCamera();

    /// The viewpoint, in world space.
    [[nodiscard]] DirectX::SimpleMath::Vector3 GetPosition() const noexcept;
    /// Unit vector the view looks along.
    [[nodiscard]] DirectX::SimpleMath::Vector3 GetForward() const noexcept;
    /// Unit vector to the view's right, kept horizontal.
    [[nodiscard]] DirectX::SimpleMath::Vector3 GetRight() const noexcept;

private:
    void TakeSceneCamera();

    engine::InputSubsystem* input_ = nullptr;
    engine::RenderingSubsystem* rendering_ = nullptr;
    DirectX::SimpleMath::Vector3 position_;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    /// Field of view and clip planes, from the scene camera when there is one.
    engine::Camera lens_{};
    /// Units per second.
    float speed_ = 5.0f;
    bool reset_pending_ = true;
};

}  // namespace editor
