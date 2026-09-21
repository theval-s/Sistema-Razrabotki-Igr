#pragma once
#include <stdexcept>
#include <wrl/client.h>

#include "BindSlot.hpp"
#include "GraphicalState.hpp"
#include "RenderingContext.hpp"
#include "RenderingResourceManager.hpp"
#include "RenderingResources.h"
#include "RenderPass.hpp"
#include "RenderWorld.hpp"
#include "ShaderContext.hpp"
#include "Texture.hpp"

namespace engine {
struct PresentPass : RenderPass {
    ShaderHandle vertexShader, pixelShader;
    TextureHandle backBufferView_;
    
    void Initialize(RenderingResourceManager& resourceManager, RenderingContext & context, RenderingResources&) override {
        vertexShader = resourceManager.CompileShader(L"PresentPass.hlsl", ShaderType::Vertex);
        pixelShader = resourceManager.CompileShader(L"PresentPass.hlsl", ShaderType::Pixel);
        
        //todo: this generally doesnt have to be here
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = context.swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(result)) {
            spdlog::error("Failed to get the back-buffer render target");
            std::exit(-1); //todo error handling
        }
        ComPtr<ID3D11RenderTargetView> backBufferView;
        result = context.device_->CreateRenderTargetView(backBuffer.Get(), nullptr, backBufferView.GetAddressOf());
        if (FAILED(result)) {
            spdlog::error("Failed to create the back-buffer render target");
            std::exit(-1); //todo error handling
        }
        Texture backBufferTexture(std::move(backBufferView));
        backBufferView_ = resourceManager.RegisterTexture(std::move(backBufferTexture));
    }

    void Render(RenderWorld&, RenderingContext& context, RenderingResources& resources) override {
        context.SetRenderTargets({backBufferView_}, TextureHandle{});
        context.SetViewport(resources.screenWith, resources.screenHeight);
        context.SetBlendState(BlendState::Opaque);
        context.SetDepthState(DepthState::Disabled);
        context.SetRasterizerState(RasterizerState::CullNone);
        context.SetShader(vertexShader);
        context.SetShader(pixelShader);
        context.SetShaderResources(BindSlots::Texture::SceneResult, resources.gBuffer.resultTexture);
        context.RawDraw(6, 0);
        context.Present();
    }

};

}
