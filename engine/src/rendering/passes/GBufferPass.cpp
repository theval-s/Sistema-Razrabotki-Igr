#include <engine/rendering/passes/GBufferPass.hpp>

#include <engine/rendering/UglyUtils.hpp>
#include <engine/rendering/MaterialDrawList.hpp>

namespace engine {

namespace {
void CreateGBuffer(RenderingResourceManager& resourceManager, RenderingResources& resources) {
    const auto textureDesc = [&](const DXGI_FORMAT format){
        return TextureDesc{
            .width = resources.screenWith, .height = resources.screenHeight,
            .format = format,
            .usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
            .srvFormat = format,
            .dsvFormat = format,
        };
    };
    auto& gBuffer = resources.gBuffer;
    gBuffer.albedoTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM));
    gBuffer.normalTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT));
    gBuffer.materialTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM));
    gBuffer.resultTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT));
    auto desc = textureDesc(DXGI_FORMAT_R32_TYPELESS);
    desc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    desc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    desc.srvFormat = DXGI_FORMAT_R32_FLOAT;
    gBuffer.depthTexture = resourceManager.CreateTexture(desc);
}

void DestroyGBuffer(RenderingResourceManager& resourceManager, RenderingResources& resources) {
    auto& gBuffer = resources.gBuffer;
    resourceManager.DestroyTexture(gBuffer.albedoTexture);
    resourceManager.DestroyTexture(gBuffer.normalTexture);
    resourceManager.DestroyTexture(gBuffer.materialTexture);
    resourceManager.DestroyTexture(gBuffer.resultTexture);
    resourceManager.DestroyTexture(gBuffer.depthTexture);
    gBuffer = {};
}
}

void GBufferPass::Initialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                              RenderingResources& resources) try {
    vertexShader_ = resourceManager.CompileShader(EngineShader::GBufferVS);
    skinnedVertexShader_ = resourceManager.CompileShader(EngineShader::SkinnedGBufferVS);
    pixelShader_ = resourceManager.CompileShader(EngineShader::GBufferPS);

    CreateGBuffer(resourceManager, resources);

    lightSphere_ = ugly_utils::FromRawMesh(resourceManager, ugly_utils::CreateSphere(1.0f));
} catch (...) {
    Destroy(resourceManager, context, resources);
    throw;
}

void GBufferPass::Destroy(RenderingResourceManager& resourceManager, RenderingContext& context,
    RenderingResources& resources) {
    resourceManager.DestroyShader(vertexShader_);
    resourceManager.DestroyShader(skinnedVertexShader_);
    resourceManager.DestroyShader(pixelShader_);
    DestroyGBuffer(resourceManager, resources);
    resourceManager.DestroyMesh(lightSphere_);
    vertexShader_ = {};
    skinnedVertexShader_ = {};
    pixelShader_ = {};
    lightSphere_ = {};
}

void GBufferPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    auto& gBuffer = resources.gBuffer;
    context.SetViewport(resources.screenWith, resources.screenHeight);
    context.SetRasterizerState(RasterizerState::CullBack);
    context.ClearRenderTargetView(gBuffer.albedoTexture);
    context.ClearRenderTargetView(gBuffer.normalTexture);
    context.ClearRenderTargetView(gBuffer.materialTexture);
    context.ClearRenderTargetView(gBuffer.resultTexture, {0, 0, 0, 1});
    context.ClearDepthView(gBuffer.depthTexture);

    context.SetRenderTargets({gBuffer.albedoTexture, gBuffer.normalTexture, gBuffer.materialTexture,
                              gBuffer.resultTexture}, gBuffer.depthTexture);
    context.SetBlendState(BlendState::Opaque);
    context.SetDepthState(DepthState::ReadWrite);
    world.BindCameraView(context, resources);

    const auto& manager = context.GetResourceManager();
    const auto draws = BuildMaterialDrawList(world, manager, false, vertexShader_, pixelShader_, skinnedVertexShader_);
    ShaderHandle boundVS{}, boundPS{};
    MaterialInstanceHandle boundInstance{};
    for (const auto& draw : draws) {
        if (draw.vertexShader != boundVS) {
            context.SetShader(draw.vertexShader);
            boundVS = draw.vertexShader;
        }
        if (draw.pixelShader != boundPS) {
            context.SetShader(draw.pixelShader);
            boundPS = draw.pixelShader;
        }
        if (draw.item->materialInstance != boundInstance) {
            const auto params = ResolveMaterialParameters(*draw.material, *draw.instance);
            resources.materialCBuffer.Update(context, [&](MaterialBufferData& data){
                data.specularColor = params.parameters.specularColor;
                data.shininess = params.parameters.shininess;
                data.emission = params.parameters.emission;
                data.lightingWeight = params.parameters.lightingWeight;
            });
            if (params.customBuffer) context.BindCBuffer(BindSlots::CBuffer::MaterialCustomBuffer, params.customBuffer);
            else context.UnbindCBuffer(BindSlots::CBuffer::MaterialCustomBuffer);
            context.SetShaderResources(BindSlots::Texture::Albedo,
                params.albedoTexture ? params.albedoTexture : resources.defaultAlbedoTexture_);
            context.SetShaderResources(BindSlots::Texture::Emission,
                params.emissionTexture ? params.emissionTexture : resources.defaultAlbedoTexture_);
            boundInstance = draw.item->materialInstance;
        }
        const auto& ri = *draw.item;
        resources.BindSkinning(context, ri.skinning.get());
        resources.objectCBuffer.Update(context, [&](ObjectBufferData& data){
            data.worldMatrix = ri.worldMatrix;
            data.normalMatrix = ri.worldMatrix.Invert().Transpose();
        });
        context.DrawMesh(ri.mesh);
    }
}

bool GBufferPass::Reinitialize(RenderingResourceManager& resourceManager, RenderingContext&, RenderingResources& resources) {
    DestroyGBuffer(resourceManager, resources);
    try { CreateGBuffer(resourceManager, resources); }
    catch (...) { DestroyGBuffer(resourceManager, resources); throw; }
    return true;
}

}


