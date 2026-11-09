#pragma once

#include <optional>

#include <SimpleMath.h>

#include <editor/editor_core_export.hpp>

namespace engine {
struct FPSCamera;
}

namespace editor {

/// De facto interface between engine and Qt
struct EDITOR_CORE_API ViewportView {
    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;

    struct Ray {
        Vector3 origin;
        /// Unit length.
        Vector3 direction;
        /// From the origin to the far plane.
        float length = 0.0f;
    };

    DirectX::SimpleMath::Matrix view;
    /// Reverse-Z, as the engine renders.
    DirectX::SimpleMath::Matrix projection;
    /// In logical pixels.
    Vector2 size;

    [[nodiscard]] static ViewportView fromCamera(const engine::FPSCamera& camera, Vector2 size);

    [[nodiscard]] Vector3 cameraPosition() const;
    /// From the near plane through `point` on screen, as far as the far plane.
    [[nodiscard]] Ray ray(Vector2 point) const;
    /// Where `point` shows on screen; nothing when it is behind the camera.
    [[nodiscard]] std::optional<Vector2> project(const Vector3& point) const;
    /// The world size of one pixel at the distance of `point`.
    [[nodiscard]] float pixelSize(const Vector3& point) const;
};

}  // namespace editor
