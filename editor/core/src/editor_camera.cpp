#include <editor/editor_camera.hpp>

#include <algorithm>
#include <cmath>

#include <editor/editor_log.hpp>

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

/// How far ahead new entities go when the view doesn't hit the ground nearby.
constexpr float SpawnDistance = 5.0f;
/// Farthest point on the ground (y = 0) that new entities are put on.
constexpr float MaxGroundDistance = 20.0f;

}  // namespace

std::string_view EditorCameraSubsystem::GetName() const {
    return "EditorCameraSubsystem";
}

bool EditorCameraSubsystem::Initialize() {
    input_ = GetEngine().GetSubsystem<engine::InputSubsystem>();
    rendering_ = GetEngine().GetSubsystem<engine::RenderingSubsystem>();
    if (input_ == nullptr || rendering_ == nullptr) {
        editorLog().error("EditorCameraSubsystem: needs InputSubsystem and RenderingSubsystem registered");
        return false;
    }
    auto& bindings = input_->DefineContext("EditorViewport");
    using engine::PhysicalKey;
    using Vector2 = DirectX::SimpleMath::Vector2;
    bindings.BindAxis2D("Move", PhysicalKey::W, Vector2{0, 1});
    bindings.BindAxis2D("Move", PhysicalKey::S, Vector2{0, -1});
    bindings.BindAxis2D("Move", PhysicalKey::D, Vector2{1, 0});
    bindings.BindAxis2D("Move", PhysicalKey::A, Vector2{-1, 0});
    bindings.BindAxis1D("Elevate", PhysicalKey::E, 1);
    bindings.BindAxis1D("Elevate", PhysicalKey::Q, -1);
    bindings.BindAction("Sprint", engine::InputSubsystem::Binding::Keyboard(PhysicalKey::LeftShift));
    bindings.BindAction("Sprint", engine::InputSubsystem::Binding::Keyboard(PhysicalKey::RightShift));
    bindings.BindAction("Look", engine::InputSubsystem::Binding::Mouse(engine::MouseButton::Right));
    return true;
}

void EditorCameraSubsystem::SetInputActive(bool active) {
    if (active && input_ && !controls_) {
        // Activation only on focus transitions (the caller does not repeat this each frame).
        controls_ = input_->Activate("EditorViewport", 10);
    } else if (!active) {
        controls_.Reset();
    }
}

void EditorCameraSubsystem::Shutdown() {
    controls_.Reset();
}

void EditorCameraSubsystem::ResetToSceneCamera() {
    reset_pending_ = true;
}

Vector3 EditorCameraSubsystem::GetPosition() const noexcept {
    return position_;
}

Vector3 EditorCameraSubsystem::GetForward() const noexcept {
    return Vector3::Transform(Vector3::Forward, Quaternion::CreateFromYawPitchRoll(yaw_, pitch_, 0.0f));
}

Vector3 EditorCameraSubsystem::GetRight() const noexcept {
    return Vector3::Transform(Vector3::Right, Quaternion::CreateFromYawPitchRoll(yaw_, 0.0f, 0.0f));
}

Vector3 EditorCameraSubsystem::GetSpawnPoint() const noexcept {
    return SpawnPoint(position_, GetForward());
}

Vector3 EditorCameraSubsystem::SpawnPoint(const Vector3& origin, const Vector3& direction) noexcept {
    if (direction.y < 0.0f) {
        const float distance = -origin.y / direction.y;
        if (distance > 0.0f && distance <= MaxGroundDistance) {
            return origin + direction * distance;
        }
    }
    return origin + direction * SpawnDistance;
}

void EditorCameraSubsystem::Focus(const Vector3& center, const float radius) {
    // A pending reset would undo the framing on the next Update().
    if (reset_pending_) {
        reset_pending_ = false;
        TakeSceneCamera();
    }
    // The narrower of the two fields of view decides.
    const float aspect = rendering_ != nullptr ? rendering_->GetWorld().camera.aspectRatio : 0.0f;
    float half_fov = lens_.fovY * 0.5f;
    if (aspect > 0.0f) half_fov = std::min(half_fov, std::atan(std::tan(half_fov) * aspect));
    const float distance = std::max(radius / std::sin(half_fov), radius + lens_.nearPlane);
    position_ = center - GetForward() * distance;
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
    if (!navigation_enabled_) return;
    if (reset_pending_) {
        reset_pending_ = false;
        TakeSceneCamera();
    }

    const Quaternion rotation = Quaternion::CreateFromYawPitchRoll(yaw_, pitch_, 0.0f);
    const float wheel = controls_.WheelDelta();
    if (!controls_.Down("Look")) {
        if (wheel != 0.0f) {
            position_ += Vector3::Transform(Vector3::Forward, rotation) * wheel * DollyStep;
        }
        return;
    }

    // On the press frame the delta still holds whatever the mouse did before the click.
    const auto look = controls_.Pressed("Look") ? DirectX::SimpleMath::Vector2{} : controls_.MouseDelta();
    yaw_ -= look.x * LookSensitivity;
    pitch_ = std::clamp(pitch_ - look.y * LookSensitivity, -MaxPitch, MaxPitch);
    speed_ = std::clamp(speed_ * std::pow(1.2f, wheel), MinSpeed, MaxSpeed);

    const Quaternion turned = Quaternion::CreateFromYawPitchRoll(yaw_, pitch_, 0.0f);
    const Vector3 forward = Vector3::Transform(Vector3::Forward, turned);
    const Vector3 right = Vector3::Transform(Vector3::Right, turned);

    Vector3 movement{};
    const auto move = controls_.Axis2D("Move");
    movement += forward * move.y + right * move.x;
    movement += Vector3::Up * controls_.Axis1D("Elevate");
    if (movement.LengthSquared() > 0.0f) {
        movement.Normalize();
        const float speed = controls_.Down("Sprint") ? speed_ * FastMultiplier : speed_;
        position_ += movement * speed * static_cast<float>(frame.realDeltaSeconds);
    }
}

void EditorCameraSubsystem::LateUpdate(const engine::FrameContext&) {
    if (!navigation_enabled_) return;
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
