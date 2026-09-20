#pragma once
#include <SimpleMath.h>

#include "FPSCamera.hpp"
#include "RenderingContext.hpp"
#include "RenderingResources.h"

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


    void BindDirectionalLightView(RenderingContext& context, RenderingResources& resources) {
        resources.viewCBuffer.Update(context, [&](ViewBufferData& data){
            float sceneRadius = 10;
            constexpr Vector3 sceneCenter{0, 1, 0};
            const auto lightPos = sceneCenter - directionalLight.direction * sceneRadius;
            data.viewMatrix = Matrix::CreateLookAt(lightPos, sceneCenter, Vector3::Up);
            sceneRadius *= 1.2f;
            data.projectionMatrix = Matrix::CreateOrthographic(sceneRadius * 2, sceneRadius * 2, 0.1f, sceneRadius * 2);
            data.invViewProj = (data.viewMatrix * data.projectionMatrix).Invert();
        });
    }

    void BindCameraView(RenderingContext& context, RenderingResources& resources) {
        resources.viewCBuffer.Update(context, [&](ViewBufferData& data){
            data.viewMatrix = camera.GetViewMatrix();
            data.projectionMatrix = camera.GetProjectionMatrix();
            data.invViewProj = (data.viewMatrix * data.projectionMatrix).Invert();
            data.cameraPos = camera.position;
            data.viewportSize = Vector2{
                static_cast<float>(resources.screenWith), static_cast<float>(resources.screenHeight)
            };
        });
    }
};

}
