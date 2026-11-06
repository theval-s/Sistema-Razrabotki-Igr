#include <editor/viewport_view.hpp>

#include <algorithm>

#include <engine/rendering/FPSCamera.hpp>

namespace editor {

namespace {

using DirectX::SimpleMath::Vector4;

/// Below this w a point is at or behind the camera.
constexpr float MinW = 1e-5f;

}  // namespace

ViewportView ViewportView::fromCamera(const engine::FPSCamera& camera, const Vector2 size) {
    return {camera.GetViewMatrix(), camera.GetProjectionMatrix(), size};
}

ViewportView::Vector3 ViewportView::cameraPosition() const {
    return view.Invert().Translation();
}

ViewportView::Ray ViewportView::ray(const Vector2 point) const {
    const auto inverse = (view * projection).Invert();
    const float x = 2.0f * point.x / size.x - 1.0f;
    const float y = 1.0f - 2.0f * point.y / size.y;
    // Reverse-Z: near is 1, far is 0.
    const Vector3 near_point = Vector3::Transform({x, y, 1.0f}, inverse);
    const Vector3 far_point = Vector3::Transform({x, y, 0.0f}, inverse);
    Vector3 direction = far_point - near_point;
    direction.Normalize();
    return {near_point, direction};
}

std::optional<ViewportView::Vector2> ViewportView::project(const Vector3& point) const {
    const Vector4 clip = Vector4::Transform(Vector4{point.x, point.y, point.z, 1.0f}, view * projection);
    if (clip.w < MinW) return std::nullopt;
    return Vector2{(clip.x / clip.w + 1.0f) * 0.5f * size.x, (1.0f - clip.y / clip.w) * 0.5f * size.y};
}

float ViewportView::pixelSize(const Vector3& point) const {
    const Vector4 clip = Vector4::Transform(Vector4{point.x, point.y, point.z, 1.0f}, view * projection);
    // w is the distance along the view, where the screen's height spans 2 * w / _22.
    return 2.0f * std::max(clip.w, MinW) / (projection._22 * size.y);
}

}  // namespace editor
