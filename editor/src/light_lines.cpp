#include <editor/light_lines.hpp>

#include <algorithm>
#include <cmath>

#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>

namespace editor {

namespace {

using DirectX::SimpleMath::Vector3;
using Lines = std::vector<Vector3>;

void line(Lines& lines, const Vector3& a, const Vector3& b) {
    lines.push_back(a);
    lines.push_back(b);
}

// The light range is measured in world units, independent of entity scale.
void circle(Lines& lines, const Vector3& center, const Vector3& axisA,
            const Vector3& axisB, float radius) {
    constexpr int segments = 32;
    constexpr float tau = 6.28318530718f;
    for (int i = 0; i < segments; ++i) {
        const float a = tau * static_cast<float>(i) / segments;
        const float b = tau * static_cast<float>(i + 1) / segments;
        line(lines, center + radius * (std::cos(a) * axisA + std::sin(a) * axisB),
             center + radius * (std::cos(b) * axisA + std::sin(b) * axisB));
    }
}

}  // namespace

void lightLines(Lines& lines, const engine::Scene& scene, entt::entity entity) {
    const auto matrix = scene.GetWorldMatrix(entity);
    const Vector3 origin = matrix.Translation();
    if (const auto* point = scene.TryGetComponent<engine::PointLight>(entity);
        point && std::isfinite(point->range) && point->range > 0.0f) {
        circle(lines, origin, Vector3::Right, Vector3::Up, point->range);
        circle(lines, origin, Vector3::Right, Vector3::Forward, point->range);
        circle(lines, origin, Vector3::Up, Vector3::Forward, point->range);
    }
    Vector3 forward = Vector3::TransformNormal(Vector3::Forward, matrix);
    Vector3 right = Vector3::TransformNormal(Vector3::Right, matrix);
    if (forward.LengthSquared() <= 0.0f) forward = Vector3::Forward;
    forward.Normalize();
    right -= forward * right.Dot(forward);
    if (right.LengthSquared() <= 0.0f) right = forward.Cross(Vector3::Up);
    if (right.LengthSquared() <= 0.0f) right = forward.Cross(Vector3::Right);
    right.Normalize();
    Vector3 up = right.Cross(forward);
    up.Normalize();
    if (const auto* spot = scene.TryGetComponent<engine::SpotLight>(entity);
        spot && std::isfinite(spot->range) && spot->range > 0.0f && std::isfinite(spot->outerHalfAngle)) {
        // Cone clipped by the light's spherical range, as in the lighting shader.
        const float angle = std::clamp(spot->outerHalfAngle, 0.0f, 1.5707963f);
        const float depth = spot->range * std::cos(angle);
        const float radius = spot->range * std::sin(angle);
        const Vector3 end = origin + forward * depth;
        circle(lines, end, right, up, radius);
        for (const Vector3 rim : {right, -right, up, -up}) line(lines, origin, end + rim * radius);
        line(lines, origin, end);
    }
    if (scene.HasComponent<engine::DirectionalLight>(entity)) {
        line(lines, origin - forward * 0.5f, origin + forward * 1.5f);
        const Vector3 tip = origin + forward * 1.5f;
        for (const Vector3 side : {right, -right, up, -up})
            line(lines, tip, tip - forward * 0.35f + side * 0.25f);
    }
}

}  // namespace editor
