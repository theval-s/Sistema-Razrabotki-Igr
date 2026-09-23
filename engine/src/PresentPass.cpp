#include "engine/PresentPass.hpp"

#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>
#include <wrl/client.h>

#include "engine/RenderingContext.hpp"
#include "engine/Texture.hpp"

namespace engine {

void PresentPass::Initialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                             RenderingResources&) {
    vertexShader_ = resourceManager.CompileShader(L"PresentPass.hlsl", ShaderType::Vertex);
    pixelShader_ = resourceManager.CompileShader(L"PresentPass.hlsl", ShaderType::Pixel);

    //todo: this generally doesnt have to be here
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = context.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(result)) {
        spdlog::error("Failed to get the back-buffer render target");
        std::exit(-1); //todo error handling
    }
    ComPtr<ID3D11RenderTargetView> backBufferView;
    result = context.GetDevice()->CreateRenderTargetView(backBuffer.Get(), nullptr, backBufferView.GetAddressOf());
    if (FAILED(result)) {
        spdlog::error("Failed to create the back-buffer render target");
        std::exit(-1); //todo error handling
    }
    Texture backBufferTexture(std::move(backBufferView));
    backBufferView_ = resourceManager.RegisterTexture(std::move(backBufferTexture));
}

void PresentPass::Render(RenderWorld&, RenderingContext& context, RenderingResources& resources) {
    context.SetRenderTargets({backBufferView_}, TextureHandle{});
    context.SetViewport(resources.screenWith, resources.screenHeight);
    context.SetBlendState(BlendState::Opaque);
    context.SetDepthState(DepthState::Disabled);
    context.SetRasterizerState(RasterizerState::CullNone);
    context.SetShader(vertexShader_);
    context.SetShader(pixelShader_);
    context.SetShaderResources(BindSlots::Texture::SceneResult, resources.gBuffer.resultTexture);
    context.RawDraw(6, 0);
    context.Present();
}

}
