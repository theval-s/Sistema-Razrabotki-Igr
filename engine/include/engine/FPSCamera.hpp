#pragma once
#include <SimpleMath.h>

#include <engine/engine_export.hpp>

namespace engine {

using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Matrix;
       
struct FPSCamera {

    float fov, aspectRatio, nearPlane, farPlane;

    float yaw = 0.0f, pitch = 0.0f;
    Vector3 position{0, 5, 0};
    
    ENGINE_API Matrix GetProjectionMatrix() const;

    ENGINE_API Matrix GetViewMatrix() const;
};   
    
}

