#include <engine/rendering/passes/PresentPass.hpp>

#include <utility>

#include <wrl/client.h>

#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/rendering/resources/Texture.hpp>

namespace engine {

namespace {
[[noreturn]] void BackbufferFailure(RenderingContext& context, const char* operation,
                                    const char* detail, HRESULT result) {
    RenderingFailure failure;
    failure.operation = operation;
    failure.pass = "PresentPass";
    failure.summary = "Failed to create the presentation back-buffer target";
    failure.detail = detail;
    failure.nativeError = static_cast<std::int64_t>(result);
    if (auto* device = context.GetDevice()) {
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (FAILED(reason)) failure.deviceRemovalReason = static_cast<std::int64_t>(reason);
    }
    throw RenderingException(std::move(failure));
}

TextureHandle CreateBackbufferTexture(RenderingResourceManager& resourceManager, RenderingContext& context) {
    if (!context.GetSwapChain() || !context.GetDevice()) {
        BackbufferFailure(context, "ValidateBackbuffer", "A live device and swap chain are required.", E_POINTER);
    }
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = context.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(result)) {
        BackbufferFailure(context, "IDXGISwapChain::GetBuffer", "Could not acquire swap-chain buffer 0.", result);
    }
    ComPtr<ID3D11RenderTargetView> backBufferView;
    result = context.GetDevice()->CreateRenderTargetView(backBuffer.Get(), nullptr, backBufferView.GetAddressOf());
    if (FAILED(result)) {
        BackbufferFailure(context, "ID3D11Device::CreateRenderTargetView",
                          "Could not create the render-target view for swap-chain buffer 0.", result);
    }
    Texture backBufferTexture(std::move(backBufferView));
    return resourceManager.RegisterTexture(std::move(backBufferTexture));
}

}

void PresentPass::Initialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                             RenderingResources& resources) {
    Destroy(resourceManager, context, resources);
    try {
        backBufferView_ = CreateBackbufferTexture(resourceManager, context);
        presentBuffer_ = resourceManager.CreateConstantBuffer<PresentBufferData>();
        vertexShader_ = resourceManager.CompileShader(L"engine/PresentPass.hlsl", ShaderType::Vertex);
        pixelShader_ = resourceManager.CompileShader(L"engine/PresentPass.hlsl", ShaderType::Pixel);
        outlinedPixelShader_ = resourceManager.CompileShader(L"engine/PresentPass.hlsl", ShaderType::Pixel,
                                                            {.defines = {{"SELECTION_OUTLINE"}}});
        linesVertexShader_ = resourceManager.CompileShader(L"engine/DebugDraw.hlsl", ShaderType::Vertex);
        screenLinesVertexShader_ = resourceManager.CompileShader(L"engine/DebugDraw.hlsl", ShaderType::Vertex,
                                                               {.defines = {{"SCREEN_SPACE"}}});
        linesGeometryShader_ = resourceManager.CompileShader(L"engine/DebugDraw.hlsl", ShaderType::Geometry);
        linesPixelShader_ = resourceManager.CompileShader(L"engine/DebugDraw.hlsl", ShaderType::Pixel);
    } catch (...) {
        Destroy(resourceManager, context, resources);
        throw;
    }
}

void PresentPass::Destroy(RenderingResourceManager& resourceManager, RenderingContext& context,
    RenderingResources&) {
    for (auto* shader : {&vertexShader_, &pixelShader_, &outlinedPixelShader_, &linesVertexShader_,
                         &screenLinesVertexShader_, &linesGeometryShader_, &linesPixelShader_}) {
        if (*shader) resourceManager.DestroyShader(*shader);
        *shader = {};
    }
    if (presentBuffer_) {
        if (context.GetDeviceContext()) context.UnbindCBuffer(BindSlots::CBuffer::Pass0);
        resourceManager.DestroyBuffer(presentBuffer_);
        presentBuffer_ = {};
    }
    if (backBufferView_) {
        if (context.GetDeviceContext()) context.GetDeviceContext()->OMSetRenderTargets(0, nullptr, nullptr);
        resourceManager.DestroyTexture(backBufferView_);
        backBufferView_ = {};
    }
}

void PresentPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    // In particular detach the scene DSV before binding its SRV, even if the
    // preceding selection pass left a read-only DSV attached.
    context.SetRenderTargets({backBufferView_}, TextureHandle{});
    context.SetViewport(resources.screenWith, resources.screenHeight);
    context.SetBlendState(BlendState::Opaque);
    context.SetDepthState(DepthState::Disabled);
    context.SetRasterizerState(RasterizerState::CullNone);
    context.SetShader(vertexShader_);
    const auto mode = resources.debugView;
    const bool outlined = mode == RenderDebugView::Final && !world.selectedSceneItems.empty();
    context.SetShader(outlined ? outlinedPixelShader_ : pixelShader_);
    TextureHandle source = resources.gBuffer.resultTexture;
    switch (mode) {
    case RenderDebugView::Albedo: source = resources.gBuffer.albedoTexture; break;
    case RenderDebugView::Normals: source = resources.gBuffer.normalTexture; break;
    case RenderDebugView::Depth: source = resources.gBuffer.depthTexture; break;
    default: break;
    }
    const PresentBufferData parameters{static_cast<uint32_t>(mode), world.camera.nearPlane, world.camera.farPlane};
    context.UpdateCBuffer(presentBuffer_, parameters);
    context.BindCBuffer(BindSlots::CBuffer::Pass0.WithStages(ShaderStage::Pixel), presentBuffer_);
    context.SetShaderResources(BindSlots::Texture::SceneResult, source);
    ID3D11ShaderResourceView* empty = nullptr;
    if (outlined) context.SetShaderResources(BindSlots::Texture::Pass0, resources.selectionMask);
    else context.GetDeviceContext()->PSSetShaderResources(16, 1, &empty);
    context.GetDeviceContext()->GSSetShader(nullptr, nullptr, 0);
    context.GetDeviceContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.RawDraw(6, 0);
    // Do not leave a depth SRV in the generic source slot for the next frame's
    // writable DSV, or a stale selection mask when switching away from Final.
    context.GetDeviceContext()->PSSetShaderResources(5, 1, &empty);
    context.GetDeviceContext()->PSSetShaderResources(16, 1, &empty);
    if (!world.debugDraw.GetLines().empty() || !world.debugDraw.GetScreenLines().empty()) {
        world.BindCameraView(context, resources);
        context.SetShader(linesGeometryShader_);
        context.SetShader(linesPixelShader_);
        context.SetShader(linesVertexShader_);
        context.DrawLines(world.debugDraw.GetLines());
        context.SetShader(screenLinesVertexShader_);
        context.DrawLines(world.debugDraw.GetScreenLines());
        context.GetDeviceContext()->GSSetShader(nullptr, nullptr, 0);
    }
    // RenderingSystem composites the registered overlay and presents this target once.
}

bool PresentPass::PreReinitialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                                   RenderingResources&) {
    if (backBufferView_) {
        if (context.GetDeviceContext()) context.GetDeviceContext()->OMSetRenderTargets(0, nullptr, nullptr);
        resourceManager.DestroyTexture(backBufferView_);
        backBufferView_ = {};
    }
    return true;
}

bool PresentPass::Reinitialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                               RenderingResources& resources) {
    PreReinitialize(resourceManager, context, resources);
    backBufferView_ = CreateBackbufferTexture(resourceManager, context);
    return true;
}

}
