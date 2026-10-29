#include <editor/editor_camera.hpp>

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>

namespace editor {

namespace {

using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector3;

constexpr float LookSensitivity = 0.0035f;  // radians per pixel
constexpr float MaxPitch = 1.54f;           // just under 90 degrees
constexpr float MinSpeed = 0.5f;
constexpr float MaxSpeed = 100.0f;
constexpr float FastMultiplier = 3.0f;
constexpr float DollyStep = 1.0f;  // units per wheel notch

// Looking down at the origin, for scenes without a camera.
const Vector3 DefaultPosition{0.0f, 4.0f, 9.0f};
constexpr float DefaultPitch = -0.4f;

}  // namespace

std::string_view EditorCameraSubsystem::GetName() const {
    return "EditorCameraSubsystem";
}

bool EditorCameraSubsystem::Initialize() {
    input_ = GetEngine().GetSubsystem<engine::InputSubsystem>();
    rendering_ = GetEngine().GetSubsystem<engine::RenderingSubsystem>();
    if (input_ == nullptr || rendering_ == nullptr) {
        spdlog::error("EditorCameraSubsystem: needs InputSubsystem and RenderingSubsystem registered");
        return false;
    }
    return true;
}

void EditorCameraSubsystem::ResetToSceneCamera() {
    reset_pending_ = true;
}

void EditorCameraSubsystem::TakeSceneCamera() {
    const engine::Scene& scene = GetEngine().GetScene();
    const entt::entity camera = scene.GetActiveCamera();
    if (camera == entt::null) {
        position_ = DefaultPosition;
        yaw_ = 0.0f;
        pitch_ = DefaultPitch;
        lens_ = {};
        return;
    }
    // Same convention as RenderExtractionSubsystem: the camera looks along local -Z.
    const auto matrix = scene.GetWorldMatrix(camera);
    Vector3 forward = Vector3::TransformNormal(Vector3::Forward, matrix);
    if (forward.LengthSquared() > 0.0f) forward.Normalize();
    position_ = matrix.Translation();
    pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    yaw_ = std::atan2(-forward.x, -forward.z);
    lens_ = *scene.TryGetComponent<engine::Camera>(camera);
}

void EditorCameraSubsystem::Update(const engine::FrameContext& frame) {
    using engine::Key;

    if (reset_pending_) {
        reset_pending_ = false;
        TakeSceneCamera();
    }

    const Quaternion rotation = Quaternion::CreateFromYawPitchRoll(yaw_, pitch_, 0.0f);
    const float wheel = input_->GetWheelDelta();
    if (!input_->IsDown(engine::MouseButton::Right)) {
        if (wheel != 0.0f) {
            position_ += Vector3::Transform(Vector3::Forward, rotation) * wheel * DollyStep;
        }
        return;
    }

    // On the press frame the delta still holds whatever the mouse did before the click.
    const auto look = input_->WasPressed(engine::MouseButton::Right) ? DirectX::SimpleMath::Vector2{}
                                                                     : input_->GetMouseDelta();
    yaw_ -= look.x * LookSensitivity;
    pitch_ = std::clamp(pitch_ - look.y * LookSensitivity, -MaxPitch, MaxPitch);
    speed_ = std::clamp(speed_ * std::pow(1.2f, wheel), MinSpeed, MaxSpeed);

    const Quaternion turned = Quaternion::CreateFromYawPitchRoll(yaw_, pitch_, 0.0f);
    const Vector3 forward = Vector3::Transform(Vector3::Forward, turned);
    const Vector3 right = Vector3::Transform(Vector3::Right, turned);

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
        position_ += movement * speed * static_cast<float>(frame.realDeltaSeconds);
    }
}

void EditorCameraSubsystem::LateUpdate(const engine::FrameContext&) {
    // The aspect ratio was already set from the surface by RenderExtractionSubsystem.
    engine::FPSCamera& camera = rendering_->GetWorld().camera;
    camera.position = position_;
    camera.yaw = yaw_;
    camera.pitch = pitch_;
    camera.fov = lens_.fovY;
    camera.nearPlane = lens_.nearPlane;
    camera.farPlane = lens_.farPlane;
}

}  // namespace editor
