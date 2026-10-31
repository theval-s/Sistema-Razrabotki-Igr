#include <engine/rendering/passes/SelectionMaskPass.hpp>

#include <engine/rendering/MaterialDrawList.hpp>

namespace engine {
namespace {
void CreateMask(RenderingResourceManager& manager, RenderingResources& resources) {
    resources.selectionMask = manager.CreateTexture({
        .width = resources.screenWith, .height = resources.screenHeight,
        .format = DXGI_FORMAT_R8G8_UNORM,
        .usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
    });
}
}

void SelectionMaskPass::Initialize(RenderingResourceManager& manager, RenderingContext&, RenderingResources& resources) {
    defaultVS_ = manager.CompileShader(L"engine/MaterialTemplate.hlsl", ShaderType::Vertex,
                                       {.defines = {{"MATERIAL_GBUFFER_VS"}}});
    visiblePS_ = manager.CompileShader(L"engine/SelectionMask.hlsl", ShaderType::Pixel);
    occludedPS_ = manager.CompileShader(L"engine/SelectionMask.hlsl", ShaderType::Pixel,
                                        {.defines = {{"OCCLUDED"}}});
    CreateMask(manager, resources);
}

void SelectionMaskPass::Destroy(RenderingResourceManager& manager, RenderingContext&, RenderingResources& resources) {
    manager.DestroyTexture(resources.selectionMask);
    resources.selectionMask = {};
    manager.DestroyShader(defaultVS_);
    manager.DestroyShader(visiblePS_);
    manager.DestroyShader(occludedPS_);
}

bool SelectionMaskPass::Reinitialize(RenderingResourceManager& manager, RenderingContext&, RenderingResources& resources) {
    manager.DestroyTexture(resources.selectionMask);
    CreateMask(manager, resources);
    return static_cast<bool>(resources.selectionMask);
}

void SelectionMaskPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    context.ClearRenderTargetView(resources.selectionMask);
    if (world.selectedSceneItems.empty()) return;

    RenderWorld selected;
    selected.sceneItems = world.selectedSceneItems;
    const auto draws = BuildMaterialDrawList(selected, context.GetResourceManager(), false, defaultVS_);
    context.SetViewport(resources.screenWith, resources.screenHeight);
    context.SetRenderTargets({resources.selectionMask}, resources.gBuffer.depthTexture, true);
    // Accumulate channels separately when several selected objects overlap.
    context.SetBlendState(BlendState::Additive);
    context.SetRasterizerState(RasterizerState::CullBack);
    world.BindCameraView(context, resources);

    for (const bool occluded : {false, true}) {
        context.SetDepthState(occluded ? DepthState::ReadOnly : DepthState::ReadOnlyReverseZ);
        context.SetShader(occluded ? occludedPS_ : visiblePS_);
        ShaderHandle boundVS{};
        for (const auto& draw : draws) {
            if (draw.vertexShader != boundVS) {
                context.SetShader(draw.vertexShader);
                boundVS = draw.vertexShader;
            }
            const auto& params = draw.instance->parameters;
            if (params.customBuffer) context.BindCBuffer(BindSlots::CBuffer::MaterialCustomBuffer, params.customBuffer);
            else context.UnbindCBuffer(BindSlots::CBuffer::MaterialCustomBuffer);
            context.SetShaderResources(BindSlots::Texture::Albedo,
                params.albedoTexture ? params.albedoTexture : resources.defaultAlbedoTexture_);
            resources.objectCBuffer.Update(context, [&](ObjectBufferData& data) {
                data.worldMatrix = draw.item->worldMatrix;
                data.normalMatrix = draw.item->worldMatrix.Invert().Transpose();
            });
            context.DrawMesh(draw.item->mesh);
        }
    }
}
} // namespace engine
