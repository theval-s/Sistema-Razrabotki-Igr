#pragma once
#include "RenderPass.hpp"
#include "UglyUtils.hpp"

namespace engine {
struct LightPass : RenderPass {
    ShaderHandle vertexShader, pixelShader, dirLightVertexShader;
    MeshHandle lightSphere;

    void Initialize(RenderingResourceManager& resourceManager, RenderingResources& resources) override {
        vertexShader = resourceManager.CompileShader(L"LightPass.hlsl", ShaderType::Vertex);
        pixelShader = resourceManager.CompileShader(L"LightPass.hlsl", ShaderType::Pixel);
        dirLightVertexShader = resourceManager.CompileShader(L"DirectionalLightVS.hlsl", ShaderType::Vertex);
        auto rawMesh = ugly_utils::CreateSphere(1.f);
        
    }

    static void UpdateLight(RenderingContext& context, RenderingResources& resources, const std::vector<LightInfo>::value_type& light) {
        resources.lightCBuffer.Update(context, [&](LightBufferData & data){
            data.lightColor = light.color;
            data.intensity = light.intensity;
            data.lightType = light.type;
            data.position = light.position;
            data.direction = light.direction;
            if (light.type == LightInfo::Spot) {
                data.spotInnerAngle = light.lightParameters.spotLight.innerAngle;
                data.spotOuterAngle = light.lightParameters.spotLight.outerAngle;
            }
        });
    }

    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override {
        auto& gBuffer = resources.gBuffer;
        context.SetViewport(resources.screenWith, resources.screenHeight);
        context.SetRasterizerState(RasterizerState::CullFront);
        context.SetBlendState(BlendState::Additive);
        context.ClearRenderTargetView(gBuffer.resultTexture, {0, 0, 0, 1});
        context.SetRenderTargets({gBuffer.resultTexture}, gBuffer.depthTexture, true);

        context.SetShader(vertexShader);
        context.SetShader(pixelShader);

        context.SetShaderResources(BindSlot::Texture::Albedo, gBuffer.albedoTexture);
        context.SetShaderResources(BindSlot::Texture::Normal, gBuffer.normalTexture);
        context.SetShaderResources(BindSlot::Texture::Material, gBuffer.materialTexture);
        context.SetShaderResources(BindSlot::Texture::Depth, gBuffer.depthTexture);
        context.SetShaderResources(BindSlot::Texture::Shadow, resources.shadowTexture);
        
        for (const auto & light : world.lights) {
            UpdateLight(context, resources, light);
            
            const auto scaleMatrix = light.type == LightInfo::Point
                                 ? Matrix::CreateScale(light.range, light.range, light.range)
                                 : Matrix::CreateScale(1, 1, 1);
            const auto rotationMatrix = Matrix::CreateFromYawPitchRoll(0, 0, 0);
            const auto positionMatrix = Matrix::CreateTranslation(light.position.x,
                                                                  light.position.y,
                                                                  light.position.z);
            auto lightWorldMatrix = scaleMatrix * rotationMatrix * positionMatrix;
            resources.objectCBuffer.Update(context, [&](ObjectBufferData & data){
                data.worldMatrix = lightWorldMatrix;
                data.normalMatrix = lightWorldMatrix.Invert().Transpose();
            });
            
            if (light.type == LightInfo::Point) {
                context.DrawMesh(lightSphere);
            } else if (light.type == LightInfo::Spot) {
                const auto bb = light.lightParameters.spotLight.boundingBox;
                context.DrawMesh(bb);
            }
        }
        
        UpdateLight(context, resources, world.directionalLight);
        context.RawDraw(6, 0);
    }
};

}
