#pragma once
#include <SimpleMath.h>

namespace engine {

using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Matrix;
       
struct FPSCamera {

    float fov, aspectRatio, nearPlane, farPlane;

    float yaw = 0.0f, pitch = 0.0f;
    Vector3 position{0, 5, 0};
    
    Matrix GetProjectionMatrix() const {
        return Matrix::CreatePerspectiveFieldOfView(fov, aspectRatio, nearPlane, farPlane);
    }

    Matrix GetViewMatrix() const {
        const auto rotationMatrix = Matrix::CreateFromYawPitchRoll(yaw, pitch, 0.0f);
        const auto forward = Vector3::Transform(Vector3::Forward, rotationMatrix);
        const auto focusPosition = position + forward;

        return Matrix::CreateLookAt(position, focusPosition, Vector3::Up);
    }
};   
    
}

