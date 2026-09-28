#pragma once

#include <SimpleMath.h>

namespace engine {

/// Local transform of an entity, relative to its parent (or the world for roots).
struct Transform {
    DirectX::SimpleMath::Vector3 position{0.0f, 0.0f, 0.0f};
    DirectX::SimpleMath::Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::SimpleMath::Vector3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] DirectX::SimpleMath::Matrix ToMatrix() const {
        using DirectX::SimpleMath::Matrix;
        return Matrix::CreateScale(scale) * Matrix::CreateFromQuaternion(rotation) *
               Matrix::CreateTranslation(position);
    }
};

}  // namespace engine
