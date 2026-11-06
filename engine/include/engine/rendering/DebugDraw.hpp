#pragma once

#include <span>
#include <vector>

#include <SimpleMath.h>

#include <engine/engine_export.hpp>
#include <engine/rendering/RenderingContext.hpp>

namespace engine {

/// Lines drawn over the final image
class ENGINE_API DebugDraw {
public:
    using Color = DirectX::SimpleMath::Color;
    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;

    /// X (0) red, Y (1) green, Z (2) blue.
    [[nodiscard]] static Color AxisColor(int axis);

    // --- In world space ---
    void Line(const Vector3& from, const Vector3& to, const Color& color);
    /// A line with an arrowhead `headLength` long at `to`.
    void Arrow(const Vector3& from, const Vector3& to, float headLength, const Color& color);
    /// A circle in the plane of `axisA` and `axisB`, both unit length and perpendicular.
    void Circle(const Vector3& center, const Vector3& axisA, const Vector3& axisB, float radius,
                const Color& color);
    /// A circle around each world axis.
    void Sphere(const Vector3& center, float radius, const Color& color);

    // --- On screen, in pixels from the top-left corner ---
    void ScreenLine(const Vector2& from, const Vector2& to, const Color& color);
    /// The world axes as seen through `view`, labeled X, Y and Z: an axis `length` pixels
    /// long seen side on, starting at `center`. The labels go about 0.4 * length further.
    void ScreenAxes(const DirectX::SimpleMath::Matrix& view, const Vector2& center, float length);

    [[nodiscard]] std::span<const LineVertex> GetLines() const noexcept { return lines_; }
    [[nodiscard]] std::span<const LineVertex> GetScreenLines() const noexcept { return screenLines_; }
    /// The renderer does it after each frame.
    void Clear();

private:
    std::vector<LineVertex> lines_;
    std::vector<LineVertex> screenLines_;
};

}  // namespace engine
