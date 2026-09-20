#pragma once
#include "RenderPass.hpp"

namespace engine {
struct GBufferPass : RenderPass {

    ShaderHandle vertexShader, pixelShader;
    
    
    void Initialize(RenderingResourceManager& resourceManager, RenderingResources& resources) override {
        vertexShader = resourceManager.CompileShader(L"GBufferPass.hlsl", ShaderType::Vertex);
        pixelShader = resourceManager.CompileShader(L"GBufferPass.hlsl", ShaderType::Pixel);
        
        const auto textureDesc = [&](const DXGI_FORMAT format){
            return TextureDesc{
                .width = resources.screenWith, .height = resources.screenHeight,
                .format = format,
                .usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
                .srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                .dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT,
            }; 
        };
        auto & gBuffer = resources.gBuffer;
        gBuffer.albedoTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM));
        gBuffer.normalTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT));
        gBuffer.materialTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM));
        gBuffer.resultTexture = resourceManager.CreateTexture(textureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT));
        auto desc = textureDesc(DXGI_FORMAT_R24G8_TYPELESS);
        desc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        desc.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        gBuffer.depthTexture = resourceManager.CreateTexture(desc);
    }
    
    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override {
        auto & gBuffer = resources.gBuffer;
        context.SetViewport(resources.screenWith, resources.screenHeight);
        context.SetRasterizerState(RasterizerState::CullBack);
        context.ClearRenderTargetView(gBuffer.albedoTexture);
        context.ClearRenderTargetView(gBuffer.normalTexture);
        context.ClearRenderTargetView(gBuffer.materialTexture);
        context.ClearDepthView(gBuffer.depthTexture);
        
        context.SetRenderTargets({gBuffer.albedoTexture, gBuffer.normalTexture, gBuffer.materialTexture}, gBuffer.depthTexture);
        context.SetShader(vertexShader);
        context.SetShader(pixelShader);
        
        context.SetDepthState(DepthState::ReadWrite);
        world.BindCameraView(context, resources);
        
        //todo: generalize with other passes
        for (const auto & ri : world.renderItems) {
            resources.materialCBuffer.Update(context, [&](MaterialBufferData & data){
                data.specularColor = ri.material.specularColor;
                data.shininess = ri.material.shininess;
                data.specialType = ri.material.specialType;
            });
            
            resources.objectCBuffer.Update(context, [&](ObjectBufferData & data){
                data.worldMatrix = ri.worldMatrix;
                data.normalMatrix = ri.worldMatrix.Invert().Transpose();
            });
            
            context.SetShaderResources(BindSlot::Texture::Albedo, ri.texture);
            
            context.DrawMesh(ri.mesh);
        }
    }
    
    
    
};

}
