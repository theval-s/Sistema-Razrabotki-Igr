#pragma once
#include <SimpleMath.h>

#include <engine/engine_export.hpp>

#include "FPSCamera.hpp"
#include "RenderingContext.hpp"
#include "engine/RenderingResources.h"

namespace engine {
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Matrix;

struct LightInfo {
    Vector3 position;
    Vector3 direction;
    Vector3 color;
    float intensity;
    float range;

    enum LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 } type;

    union {
        struct {
            float innerAngle;
            float outerAngle;
            float range;
            MeshHandle boundingBox;
        } spotLight;
    } lightParameters;
};

struct MaterialParameters {
    Vector3 specularColor;
    float shininess;
    uint32_t specialType;
};

struct RenderItem {
    Matrix worldMatrix;
    MeshHandle mesh;
    MaterialParameters material;
    TextureHandle texture;
};

struct RenderWorld {
    FPSCamera camera;
    std::vector<RenderItem> renderItems;
    std::vector<LightInfo> lights;
    LightInfo directionalLight; //as for now only one dir light allowed


    ENGINE_API void BindDirectionalLightView(RenderingContext& context, RenderingResources& resources);

    ENGINE_API void BindCameraView(RenderingContext& context, RenderingResources& resources);
};

}
