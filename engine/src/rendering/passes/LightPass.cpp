#include <engine/rendering/passes/LightPass.hpp>

#include <engine/rendering/UglyUtils.hpp>

#include <string>

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
            data.spotInnerAngle = light.spotInnerAngle;
            data.spotOuterAngle = light.spotOuterAngle;
        }
    });
}

}

void LightPass::Initialize(RenderingResourceManager& resourceManager, RenderingContext&, RenderingResources&) {
    vertexShader_ = resourceManager.CompileShader(L"engine/LightPass.hlsl", ShaderType::Vertex);
    ShaderCompileOptions pixelOptions;
    pixelOptions.defines = {{"PCF_RADIUS", "1"}, {"SHADOW_MAP_SIZE", std::to_string(ShadowMapSize)}};
    pixelShader_ = resourceManager.CompileShader(L"engine/LightPass.hlsl", ShaderType::Pixel, pixelOptions);
    dirLightVertexShader_ = resourceManager.CompileShader(L"engine/DirectionalLightVS.hlsl", ShaderType::Vertex);
    lightSphere_ = ugly_utils::FromRawMesh(resourceManager, ugly_utils::CreateSphere(1.0f));
    // One shared bounding box; each spot light scales it to its cone bounds.
    spotLightBB_ = ugly_utils::FromRawMesh(resourceManager, ugly_utils::CreateCube(1.0f, 1.0f, 1.0f));
}

void LightPass::Destroy(RenderingResourceManager& resourceManager, RenderingContext& context,
    RenderingResources& resources) {
    resourceManager.DestroyShader(vertexShader_);
    resourceManager.DestroyShader(pixelShader_);
    resourceManager.DestroyShader(dirLightVertexShader_);
    resourceManager.DestroyMesh(lightSphere_);
    resourceManager.DestroyMesh(spotLightBB_);
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
            const float diameter = 2.0f * light.range * std::tan(std::acos(light.spotOuterAngle));
            lightWorldMatrix = Matrix::CreateScale(diameter, diameter, light.range) *
                Matrix::CreateWorld(boxCenter, direction, up);
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
            context.DrawMesh(spotLightBB_);
        }
    }
    // The directional light covers the entire screen; its shader reads scene depth
    // directly and must not be clipped by the light-volume depth test.
    context.SetRenderTargets({gBuffer.resultTexture}, TextureHandle{});
    context.SetDepthState(DepthState::Disabled);
    context.SetRasterizerState(RasterizerState::CullNone);
    UpdateLight(context, resources, world.directionalLight);
    context.SetShader(dirLightVertexShader_);
    context.RawDraw(6, 0);
}

}
