#include "engine/RenderWorld.hpp"

namespace engine {

void RenderWorld::BindDirectionalLightView(RenderingContext& context, RenderingResources& resources) {
    resources.viewCBuffer.Update(context, [&](ViewBufferData& data){
        float sceneRadius = 10;
        constexpr Vector3 sceneCenter{0, 0, 0};
        const auto lightPos = sceneCenter - directionalLight.direction * sceneRadius;
        data.viewMatrix = Matrix::CreateLookAt(lightPos, sceneCenter, Vector3::Up);
        sceneRadius *= 1.2f;
        data.projectionMatrix = Matrix::CreateOrthographic(sceneRadius * 2, sceneRadius * 2, 0.1f, sceneRadius * 2);
        data.invViewProj = (data.viewMatrix * data.projectionMatrix).Invert();
    });
}

void RenderWorld::BindCameraView(RenderingContext& context, RenderingResources& resources) {
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

}
