#include <engine/rendering/DebugDraw.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace engine {

namespace {

using DirectX::SimpleMath::Color;
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;

constexpr int CircleSegments = 32;
constexpr float Tau = 6.28318530718f;

/// Strokes of X, Y and Z in a box from -1 to 1, y down.
using Stroke = std::pair<Vector2, Vector2>;
const std::array<std::initializer_list<Stroke>, 3> Letters{{
    {{{-1, -1}, {1, 1}}, {{-1, 1}, {1, -1}}},
    {{{-1, -1}, {0, 0}}, {{1, -1}, {0, 0}}, {{0, 0}, {0, 1}}},
    {{{-1, -1}, {1, -1}}, {{1, -1}, {-1, 1}}, {{-1, 1}, {1, 1}}},
}};

}  // namespace

Color DebugDraw::AxisColor(const int axis) {
    switch (axis) {
        case 0: return {0.93f, 0.25f, 0.28f};
        case 1: return {0.45f, 0.82f, 0.2f};
        default: return {0.25f, 0.5f, 0.98f};
    }
}

void DebugDraw::Line(const Vector3& from, const Vector3& to, const Color& color) {
    lines_.push_back({from, color});
    lines_.push_back({to, color});
}

void DebugDraw::Arrow(const Vector3& from, const Vector3& to, const float headLength, const Color& color) {
    Line(from, to, color);
    Vector3 direction = to - from;
    if (direction.LengthSquared() <= 0.0f) return;
    direction.Normalize();
    // Any two directions across the shaft.
    Vector3 side = direction.Cross(std::abs(direction.y) < 0.9f ? Vector3::Up : Vector3::Right);
    side.Normalize();
    const Vector3 other = direction.Cross(side);
    const Vector3 base = to - direction * headLength;
    const float width = headLength * 0.7f;
    for (const Vector3& offset : {side, -side, other, -other}) Line(to, base + offset * width, color);
}

void DebugDraw::Circle(const Vector3& center, const Vector3& axisA, const Vector3& axisB, const float radius,
                       const Color& color) {
    for (int i = 0; i < CircleSegments; ++i) {
        const float a = Tau * static_cast<float>(i) / CircleSegments;
        const float b = Tau * static_cast<float>(i + 1) / CircleSegments;
        Line(center + radius * (std::cos(a) * axisA + std::sin(a) * axisB),
             center + radius * (std::cos(b) * axisA + std::sin(b) * axisB), color);
    }
}

void DebugDraw::Sphere(const Vector3& center, const float radius, const Color& color) {
    Circle(center, Vector3::UnitX, Vector3::UnitY, radius, color);
    Circle(center, Vector3::UnitX, Vector3::UnitZ, radius, color);
    Circle(center, Vector3::UnitY, Vector3::UnitZ, radius, color);
}

void DebugDraw::Box(const DirectX::SimpleMath::Matrix& transform, const Color& color) {
    // Corner i has x, y and z from its bits 0, 1 and 2.
    std::array<Vector3, 8> corners;
    for (int i = 0; i < 8; ++i) {
        corners[i] = Vector3::Transform({(i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f},
                                        transform);
    }
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0) Line(corners[i], corners[i | bit], color);
        }
    }
}

void DebugDraw::ScreenLine(const Vector2& from, const Vector2& to, const Color& color) {
    screenLines_.push_back({Vector3{from.x, from.y, 0.0f}, color});
    screenLines_.push_back({Vector3{to.x, to.y, 0.0f}, color});
}

void DebugDraw::ScreenAxes(const DirectX::SimpleMath::Matrix& view, const Vector2& center, const float length) {
    // In view space x goes right, y up and z toward the viewer.
    std::array<std::pair<int, Vector3>, 3> axes{{
        {0, Vector3::TransformNormal(Vector3::UnitX, view)},
        {1, Vector3::TransformNormal(Vector3::UnitY, view)},
        {2, Vector3::TransformNormal(Vector3::UnitZ, view)},
    }};
    // Farthest first, so the axes pointing at the viewer are drawn over the others.
    std::ranges::sort(axes, {}, [](const auto& axis) { return axis.second.z; });
    const Vector2 letterSize{length * 0.1f, length * 0.12f};  // half of it
    for (const auto& [index, direction] : axes) {
        Color color = AxisColor(index);
        if (direction.z < 0.0f) {
            color = Color::Lerp(color, Color{0.35f, 0.35f, 0.35f}, 0.45f);  // pointing away
        }
        const Vector2 screen{direction.x, -direction.y};
        ScreenLine(center, center + screen * length, color);
        const Vector2 letter = center + screen * length * 1.26f;
        for (const auto& [from, to] : Letters[index]) {
            ScreenLine(letter + from * letterSize, letter + to * letterSize, color);
        }
    }
}

void DebugDraw::Clear() {
    lines_.clear();
    screenLines_.clear();
}

}  // namespace engine
