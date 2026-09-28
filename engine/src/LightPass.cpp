#include "engine/LightPass.hpp"

#include "engine/UglyUtils.hpp"

namespace engine {
namespace {

void UpdateLight(RenderingContext& context, RenderingResources& resources,
                 const std::vector<LightInfo>::value_type& light) {
    resources.lightCBuffer.Update(context, [&](LightBufferData & data){
        data.lightColor = light.color;
        data.intensity = light.intensity;
        data.lightType = light.type;
        data.position = light.position;
        data.direction = light.direction;
        data.range = light.range;
        if (light.type == LightInfo::Spot) {
            data.spotInnerAngle = light.lightParameters.spotLight.innerAngle;
            data.spotOuterAngle = light.lightParameters.spotLight.outerAngle;
        }
    });
}

}

void LightPass::Initialize(RenderingResourceManager& resourceManager, RenderingContext&, RenderingResources&) {
    vertexShader_ = resourceManager.CompileShader(L"LightPass.hlsl", ShaderType::Vertex);
    pixelShader_ = resourceManager.CompileShader(L"LightPass.hlsl", ShaderType::Pixel);
    dirLightVertexShader_ = resourceManager.CompileShader(L"DirectionalLightVS.hlsl", ShaderType::Vertex);
    lightSphere_ = ugly_utils::FromRawMesh(resourceManager, ugly_utils::CreateSphere(1.0f));
}

void LightPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    auto& gBuffer = resources.gBuffer;
    context.SetViewport(resources.screenWith, resources.screenHeight);
    context.SetRasterizerState(RasterizerState::CullFront);
    context.SetBlendState(BlendState::Additive);
    context.SetDepthState(DepthState::ReadOnly);
    context.ClearRenderTargetView(gBuffer.resultTexture, {0, 0, 0, 1});

    context.SetRenderTargets({gBuffer.resultTexture}, gBuffer.depthTexture, true);

    context.SetShader(vertexShader_);
    context.SetShader(pixelShader_);

    context.SetShaderResources(BindSlots::Texture::Albedo, gBuffer.albedoTexture);
    context.SetShaderResources(BindSlots::Texture::Normal, gBuffer.normalTexture);
    context.SetShaderResources(BindSlots::Texture::Material, gBuffer.materialTexture);
    context.SetShaderResources(BindSlots::Texture::Depth, gBuffer.depthTexture);
    context.SetShaderResources(BindSlots::Texture::Shadow, resources.shadowTexture);

    for (const auto & light : world.lights) {
        UpdateLight(context, resources, light);

        Matrix lightWorldMatrix;
        if (light.type == LightInfo::Spot) {
            Vector3 direction = light.direction;
            direction.Normalize();
            const Vector3 up = std::abs(direction.Dot(Vector3::Up)) > 0.99f
                                   ? Vector3::Right
                                   : Vector3::Up;
            const Vector3 boxCenter = light.position + direction * (light.range * 0.5f);
            lightWorldMatrix = Matrix::CreateWorld(boxCenter, direction, up);
        } else {
            lightWorldMatrix = Matrix::CreateScale(light.range) * Matrix::CreateTranslation(light.position);
        }
        resources.objectCBuffer.Update(context, [&](ObjectBufferData & data){
            data.worldMatrix = lightWorldMatrix;
            data.normalMatrix = lightWorldMatrix.Invert().Transpose();
        });

        if (light.type == LightInfo::Point) {
            context.DrawMesh(lightSphere_);
        } else if (light.type == LightInfo::Spot) {
            const auto bb = light.lightParameters.spotLight.boundingBox;
            context.DrawMesh(bb);
        }
    }
    // context.SetRenderTargets({gBuffer.resultTexture}, TextureHandle{});
    context.SetRasterizerState(RasterizerState::CullNone);
    UpdateLight(context, resources, world.directionalLight);
    context.SetShader(dirLightVertexShader_);
    context.RawDraw(6, 0);
}

}
