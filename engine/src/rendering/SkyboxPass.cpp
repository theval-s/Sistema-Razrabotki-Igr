#include "SkyboxPass.hpp"
#include "LifecycleFailure.hpp"

#include <cstring>

namespace engine {

void SkyboxPass::Initialize(RenderingResourceManager& manager, RenderingContext& context,
                            RenderingResources& resources) try {
    vertexShader_ = manager.CompileShader(EngineShader::SkyboxVS);
    pixelShader_ = manager.CompileShader(EngineShader::SkyboxPS);
    auto* device = context.GetDevice();
    const D3D11_BUFFER_DESC buffer{
        .ByteWidth = sizeof(SkyboxConstants), .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE};
    rendering_detail::Check(device->CreateBuffer(&buffer, nullptr, &constants_), "CreateBuffer", device, "Skybox constants");
    const D3D11_DEPTH_STENCIL_DESC depth{
        .DepthEnable = TRUE, .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc = D3D11_COMPARISON_GREATER_EQUAL};
    rendering_detail::Check(device->CreateDepthStencilState(&depth, &depthState_), "CreateDepthStencilState", device);
    const D3D11_RASTERIZER_DESC rasterizer{
        .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE, .DepthClipEnable = TRUE};
    rendering_detail::Check(device->CreateRasterizerState(&rasterizer, &rasterizerState_), "CreateRasterizerState", device);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    rendering_detail::Check(device->CreateBlendState(&blend, &blendState_), "CreateBlendState", device);
    const D3D11_SAMPLER_DESC sampler{
        .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP, .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP, .MaxAnisotropy = 1,
        .ComparisonFunc = D3D11_COMPARISON_NEVER, .MinLOD = 0, .MaxLOD = D3D11_FLOAT32_MAX};
    rendering_detail::Check(device->CreateSamplerState(&sampler, &sampler_), "CreateSamplerState", device);
} catch (...) {
    Destroy(manager, context, resources);
    throw;
}

void SkyboxPass::Destroy(RenderingResourceManager& manager, RenderingContext& context, RenderingResources&) {
    // Also detach any reference before a scene asset scope can evict its cubemap.
    if (auto* dc = context.GetDeviceContext()) {
        ID3D11ShaderResourceView* empty = nullptr;
        dc->PSSetShaderResources(0, 1, &empty);
    }
    manager.DestroyShader(vertexShader_);
    manager.DestroyShader(pixelShader_);
    vertexShader_ = {};
    pixelShader_ = {};
    constants_.Reset();
    depthState_.Reset();
    rasterizerState_.Reset();
    blendState_.Reset();
    sampler_.Reset();
}

void SkyboxPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    if (resources.debugView != RenderDebugView::Final) return;

    SkyboxConstants data;
    data.inverseProjection = world.camera.GetProjectionMatrix().Invert();
    auto viewRotation = world.camera.GetViewMatrix();
    // Remove translation BEFORE inversion: large camera positions must not enter
    // the inverse, nor affect the ray through a homogeneous divide.
    viewRotation._41 = viewRotation._42 = viewRotation._43 = 0;
    viewRotation._14 = viewRotation._24 = viewRotation._34 = 0;
    viewRotation._44 = 1;
    data.viewToCubemap = viewRotation.Invert() * Matrix::CreateRotationY(-world.environment.settings.yawRadians);
    data.tint = world.environment.settings.tint;
    data.brightness = world.environment.settings.brightness;
    data.fallbackColor = world.environment.settings.fallbackColor;
    ID3D11ShaderResourceView* srv = nullptr;
    if (const auto* texture = context.GetResourceManager().TryGetTexture(world.environment.cubemap)) {
        if (auto* candidate = texture->GetSRV()) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            candidate->GetDesc(&desc);
            if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE) srv = candidate;
        }
    }
    data.useCubemap = srv ? 1u : 0u;
    auto* dc = context.GetDeviceContext();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    rendering_detail::Check(dc->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                            "Map", context.GetDevice(), "Skybox constants");
    std::memcpy(mapped.pData, &data, sizeof(data));
    dc->Unmap(constants_.Get(), 0);

    context.ClearState();
    context.SetViewport(resources.screenWith, resources.screenHeight);
    context.SetRenderTargets({resources.gBuffer.resultTexture}, resources.gBuffer.depthTexture, true);
    dc->OMSetDepthStencilState(depthState_.Get(), 0);
    dc->OMSetBlendState(blendState_.Get(), nullptr, ~0u);
    dc->RSSetState(rasterizerState_.Get());
    context.SetShader(vertexShader_);
    context.SetShader(pixelShader_);
    dc->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    dc->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    dc->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    dc->PSSetShaderResources(0, 1, &srv);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.RawDraw(3, 0);

    ID3D11ShaderResourceView* empty = nullptr;
    dc->PSSetShaderResources(0, 1, &empty);
    context.ClearState();
    // b0 and s0 were private to this draw. Restore shared bindings, keeping the
    // camera view data produced by GBufferPass intact for particles and selection.
    resources.BindCBuffers(context);
    context.BindSamplers();
}
} // namespace engine
