#include <editor/editor_camera.hpp>

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>

namespace editor {

namespace {

constexpr float LookSensitivity = 0.0035f;  // radians per pixel
constexpr float MaxPitch = 1.54f;           // just under 90 degrees
constexpr float MinSpeed = 0.5f;
constexpr float MaxSpeed = 100.0f;
constexpr float FastMultiplier = 3.0f;
constexpr float DollyStep = 1.0f;  // units per wheel notch

}  // namespace

std::string_view EditorCameraSubsystem::GetName() const {
    return "EditorCameraSubsystem";
}

bool EditorCameraSubsystem::Initialize() {
    input_ = GetEngine().GetSubsystem<engine::InputSubsystem>();
    if (input_ == nullptr) {
        spdlog::error("EditorCameraSubsystem: needs InputSubsystem registered");
        return false;
    }
    return true;
}

void EditorCameraSubsystem::Update(const engine::FrameContext& frame) {
    using DirectX::SimpleMath::Quaternion;
    using DirectX::SimpleMath::Vector3;
    using engine::Key;

    engine::Scene& scene = GetEngine().GetScene();
    const entt::entity camera = scene.GetActiveCamera();
    if (camera == entt::null) {
        camera_ = entt::null;
        return;
    }
    engine::Transform& transform = scene.GetTransform(camera);

    // Same convention as RenderExtractionSubsystem: the camera looks along local -Z. Re-read on
    // every RMB press too, in case something else turned the camera meanwhile.
    if (camera != camera_ || input_->WasPressed(engine::MouseButton::Right)) {
        camera_ = camera;
        const Vector3 forward = Vector3::Transform(Vector3::Forward, transform.rotation);
        pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        yaw_ = std::atan2(-forward.x, -forward.z);
    }

    const float wheel = input_->GetWheelDelta();
    // Only touch the transform while navigating, so whatever else sets it keeps working.
    if (!input_->IsDown(engine::MouseButton::Right)) {
        if (wheel != 0.0f) {
            transform.position += Vector3::Transform(Vector3::Forward, transform.rotation) * wheel * DollyStep;
        }
        return;
    }

    // On the press frame the delta still holds whatever the mouse did before the click.
    const auto look = input_->WasPressed(engine::MouseButton::Right) ? DirectX::SimpleMath::Vector2{}
                                                                     : input_->GetMouseDelta();
    yaw_ -= look.x * LookSensitivity;
    pitch_ = std::clamp(pitch_ - look.y * LookSensitivity, -MaxPitch, MaxPitch);
    speed_ = std::clamp(speed_ * std::pow(1.2f, wheel), MinSpeed, MaxSpeed);
    transform.rotation = Quaternion::CreateFromYawPitchRoll(yaw_, pitch_, 0.0f);

    const Vector3 forward = Vector3::Transform(Vector3::Forward, transform.rotation);
    const Vector3 right = Vector3::Transform(Vector3::Right, transform.rotation);

    Vector3 movement{};
    if (input_->IsDown(Key::W)) movement += forward;
    if (input_->IsDown(Key::S)) movement -= forward;
    if (input_->IsDown(Key::D)) movement += right;
    if (input_->IsDown(Key::A)) movement -= right;
    if (input_->IsDown(Key::E)) movement += Vector3::Up;
    if (input_->IsDown(Key::Q)) movement -= Vector3::Up;
    if (movement.LengthSquared() > 0.0f) {
        movement.Normalize();
        const float speed = input_->IsDown(Key::Shift) ? speed_ * FastMultiplier : speed_;
        transform.position += movement * speed * static_cast<float>(frame.realDeltaSeconds);
    }
}

}  // namespace editor
