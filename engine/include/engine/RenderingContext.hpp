#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <span>
#include <optional>
#include <engine/engine_export.hpp>
#include <spdlog/spdlog.h>
#include <wrl/client.h>

#include "BindSlot.hpp"
#include "Buffer.hpp"
#include "GraphicalState.hpp"
#include "RenderedMesh.h"
#include "RenderingResourceManager.hpp"
#include "Texture.hpp"
#include "window.hpp"

namespace engine {
using Microsoft::WRL::ComPtr;

/**
 * Simple thin wrapper around DX11 Context, helps to set shaders and resources via Handles
 * To simplify architecture we are keeping most resources bound during render pass
 * Each resource has reserved slot so that's no problem.
 * If pass requires to reuse some out-resources as in-resources, it must inbind all out-resources explicitly or wise-versa
 */
struct RenderingContext {
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> deviceContext_;
    RenderingResourceManager resourceManager_;
    RenderStates renderingStates_;
    ShaderLayoutHandle vsInputLayout_{};

    RenderingContext() = default;

    ENGINE_API void Initialize(const uint32_t screenWidth = 1000, const uint32_t screenHeight = 600) {
        const auto hwnd = makeWindow(screenWidth, screenHeight);

        const DXGI_SWAP_CHAIN_DESC swapChainDesc{
            .BufferDesc = DXGI_MODE_DESC{
                .Width = screenWidth,
                .Height = screenHeight,
                .RefreshRate = {.Numerator = 60, .Denominator = 1}, //todo FPS
                .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                .ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
                .Scaling = DXGI_MODE_SCALING_UNSPECIFIED
            },
            .SampleDesc = {.Count = 1, .Quality = 0},
            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = 2,
            .OutputWindow = hwnd,
            .Windowed = true,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH,
        };

        constexpr D3D_FEATURE_LEVEL featureLevel[] = {D3D_FEATURE_LEVEL_11_1};
        const auto r = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_DEBUG, //todo
            featureLevel,
            1,
            D3D11_SDK_VERSION,
            &swapChainDesc,
            swapChain_.GetAddressOf(),
            device_.GetAddressOf(),
            nullptr,
            deviceContext_.GetAddressOf());
        checkResult(r, "D3D11CreateDeviceAndSwapChain");
        resourceManager_.Initialize(device_.Get());
        renderingStates_.Initialize(device_.Get());
    }

    void SetShader(const ShaderHandle h) {
        switch (auto& shader = resourceManager_.shaders_.Get(h); shader.type) {
        case ShaderType::Vertex:
            deviceContext_->VSSetShader(shader.shader.vertexShader.Get(), nullptr, 0);
            vsInputLayout_ = shader.layoutHandle;
            break;
        case ShaderType::Pixel:
            deviceContext_->PSSetShader(shader.shader.pixelShader.Get(), nullptr, 0);
            break;
        case ShaderType::Geometry:
            deviceContext_->GSSetShader(shader.shader.geometryShader.Get(), nullptr, 0);
            break;
        }
    }

    void SetViewport(const uint32_t width, const uint32_t height) const {
        const D3D11_VIEWPORT viewport = {
            .TopLeftX = 0.0f,
            .TopLeftY = 0.0f,
            .Width = static_cast<float>(width),
            .Height = static_cast<float>(height),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f,
        };
        deviceContext_->RSSetViewports(1, &viewport);
    }

    void BindMesh(const MeshHandle h) {
        assert(vsInputLayout_); //Vertex shader should be set before mesh bindings as we need to bind input layout
        const auto& mesh = resourceManager_.meshes_.Get(h);
        const auto inputLayout = resourceManager_.inputLayoutCache_.GetInputLayout(
            device_.Get(), mesh.meshLayout, vsInputLayout_);

        deviceContext_->IASetInputLayout(inputLayout);
        deviceContext_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto& indexBuffer = resourceManager_.buffers_.Get(mesh.indexBuffer);
        deviceContext_->IASetIndexBuffer(indexBuffer.buffer.Get(), DXGI_FORMAT_R32_UINT, 0);

        //If we'll even want multiple vertex buffers, we'll need some enhancements here VVV
        const auto& vertexBuffer = resourceManager_.buffers_.Get(mesh.vertexBuffer);
        const UINT strides[] = {vertexBuffer.stride};
        constexpr UINT offsets[] = {0};
        deviceContext_->IASetVertexBuffers(0, 1, vertexBuffer.buffer.GetAddressOf(), strides, offsets);
    }
    
    void RawDrawIndexed(const uint32_t indexCount, const uint32_t baseIndex = 0, const uint32_t baseVertex = 0) const {
        deviceContext_->DrawIndexed(indexCount, baseIndex, baseVertex);
    }
    
    void RawDraw(const uint32_t vertexCount, const uint32_t baseVertex = 0) const {
        deviceContext_->Draw(vertexCount, baseVertex);
    }
    
    void DrawMesh(const MeshHandle h) {
        BindMesh(h);
        const auto& mesh = resourceManager_.meshes_.Get(h);
        deviceContext_->DrawIndexed(mesh.indexCount, 0, 0);
    }

    void BindCBuffer(const BindSlot slot, const BufferHandle h) {
        const auto& buffer = resourceManager_.buffers_.Get(h);
        if (slot.HasStage(ShaderStage::Vertex)) {
            deviceContext_->VSSetConstantBuffers(slot.id_, 1, buffer.buffer.GetAddressOf());
        }
        if (slot.HasStage(ShaderStage::Pixel)) {
            deviceContext_->PSSetConstantBuffers(slot.id_, 1, buffer.buffer.GetAddressOf());
        }
        if (slot.HasStage(ShaderStage::Domain)) {
            deviceContext_->DSSetConstantBuffers(slot.id_, 1, buffer.buffer.GetAddressOf());
        }
        if (slot.HasStage(ShaderStage::Hull)) {
            deviceContext_->HSSetConstantBuffers(slot.id_, 1, buffer.buffer.GetAddressOf());
        }
        if (slot.HasStage(ShaderStage::Geometry)) {
            deviceContext_->GSSetConstantBuffers(slot.id_, 1, buffer.buffer.GetAddressOf());
        }
        if (slot.HasStage(ShaderStage::Compute)) {
            deviceContext_->CSSetConstantBuffers(slot.id_, 1, buffer.buffer.GetAddressOf());
        }
    }

    void SetRenderTargets(const std::initializer_list<TextureHandle> targets, const TextureHandle depth,
                          const bool depthReadOnly = false) {
        ID3D11DepthStencilView* dsv = nullptr;
        if (depth) {
            const auto& depthTexture = resourceManager_.textures_.Get(depth);
            dsv = depthReadOnly ? depthTexture.GetReadOnlyDSV() : depthTexture.GetDSV();
        }
        std::array<ID3D11RenderTargetView*, 16> renderTargetViews;
        for (int i = 0; i < targets.size(); i++) {
            if (const auto target = targets.begin()[i]) {
                renderTargetViews[i] = resourceManager_.textures_.Get(target).GetRTV();
            } else {
                renderTargetViews[i] = nullptr;
            }
        }
        deviceContext_->OMSetRenderTargets(static_cast<UINT>(targets.size()), renderTargetViews.data(), dsv);
    }

    //99% of calls would only set pixel shader resources...
    void SetShaderResources(const BindSlot slot, const TextureHandle h) {
        const auto& texture = resourceManager_.textures_.Get(h);
        const auto srv = texture.GetSRV();
        if (slot.HasStage(ShaderStage::Vertex)) {
            deviceContext_->VSSetShaderResources(slot.id_, 1, &srv);
        }
        if (slot.HasStage(ShaderStage::Pixel)) {
            deviceContext_->PSSetShaderResources(slot.id_, 1, &srv);
        }
        if (slot.HasStage(ShaderStage::Domain)) {
            deviceContext_->DSSetShaderResources(slot.id_, 1, &srv);
        }
        if (slot.HasStage(ShaderStage::Hull)) {
            deviceContext_->HSSetShaderResources(slot.id_, 1, &srv);
        }
        if (slot.HasStage(ShaderStage::Geometry)) {
            deviceContext_->GSSetShaderResources(slot.id_, 1, &srv);
        }
        if (slot.HasStage(ShaderStage::Compute)) {
            deviceContext_->CSSetShaderResources(slot.id_, 1, &srv);
        }
    }
    
    void BindSamplers() {
        constexpr uint32_t count = static_cast<uint32_t>(SamplerState::Count);
        std::array<ID3D11SamplerState*, count> samplers;
        for (int i = 0; i < count; i++) {
            samplers[i] = renderingStates_.GetSamplerState(static_cast<SamplerState>(i));
        }
        deviceContext_->PSSetSamplers(0, count, samplers.data());
    }

    //While I exposed blend factor, currently there are not BlendStates using it... so todo: think
    void SetBlendState(const BlendState state, const float* blendFactor = nullptr,
                       const UINT sampleMask = 0xffffffff) const {
        deviceContext_->OMSetBlendState(renderingStates_.GetBlendState(state), blendFactor, sampleMask);
    }

    void SetDepthState(const DepthState state, const UINT stencilRef = 0) const {
        deviceContext_->OMSetDepthStencilState(renderingStates_.GetDepthState(state), stencilRef);
    }

    void SetRasterizerState(const RasterizerState state) const {
        deviceContext_->RSSetState(renderingStates_.GetRasterizerState(state));
    }

    void SetGraphicalState(const GraphicalState state) const {
        SetBlendState(state.blendState);
        SetDepthState(state.depthState);
        SetRasterizerState(state.rasterizerState);
    }
    
    void ClearState() {
        deviceContext_->ClearState();
    }

    //Simple function that clear only state that can cause resource conflicts
    //Useful to call between render passes that use previous passes resources
    void ClearHazardousState() const {
        static constexpr ID3D11ShaderResourceView* const nullSRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        constexpr UINT count = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        deviceContext_->VSSetShaderResources(0, count, nullSRVs);
        deviceContext_->HSSetShaderResources(0, count, nullSRVs);
        deviceContext_->DSSetShaderResources(0, count, nullSRVs);
        deviceContext_->GSSetShaderResources(0, count, nullSRVs);
        deviceContext_->PSSetShaderResources(0, count, nullSRVs);
        deviceContext_->CSSetShaderResources(0, count, nullSRVs);

        deviceContext_->OMSetRenderTargets(0, nullptr, nullptr);
        deviceContext_->SOSetTargets(0, nullptr, nullptr);

        static constexpr ID3D11UnorderedAccessView* const nullUAVs[8] = {};
        deviceContext_->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
        
        deviceContext_->VSSetShader(nullptr, nullptr, 0);
        deviceContext_->HSSetShader(nullptr, nullptr, 0);
        deviceContext_->DSSetShader(nullptr, nullptr, 0);
        deviceContext_->GSSetShader(nullptr, nullptr, 0);
        deviceContext_->PSSetShader(nullptr, nullptr, 0);
        deviceContext_->CSSetShader(nullptr, nullptr, 0);
    }

    void ClearDepthView(const TextureHandle h, const std::optional<float> depth = 1.f, const std::optional<UINT> stencil = std::nullopt) {
        UINT flags = depth ? D3D11_CLEAR_DEPTH : 0;
        flags |= stencil ? D3D11_CLEAR_STENCIL : 0;
        const auto& texture = resourceManager_.textures_.Get(h);
        deviceContext_->ClearDepthStencilView(
            texture.GetDSV(), flags, depth.value_or(0.f), static_cast<UINT8>(stencil.value_or(0)));
    }
    
    void ClearRenderTargetView(const TextureHandle h, const std::array<float, 4> color = {0.f, 0.f, 0.f, 0.f}) {
        const auto& texture = resourceManager_.textures_.Get(h);
        deviceContext_->ClearRenderTargetView(texture.GetRTV(), color.data());
    }
    
    void UpdateBuffer(const GpuBuffer& buffer, const void* data, const uint32_t size) const {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = deviceContext_->Map(buffer.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        assert(SUCCEEDED(hr)); //Idk what to do if not...
        memcpy(mapped.pData, data, size);
        deviceContext_->Unmap(buffer.buffer.Get(), 0);
    }
    
    template<typename T>
    void UpdateCBuffer(const BufferHandle handle, const T& data) {
        const auto& buffer = resourceManager_.buffers_.Get(handle);
        UpdateBuffer(buffer, reinterpret_cast<const void*>(&data), sizeof(T));
    }
    
    void Present() const {
        const auto res = swapChain_->Present(1, 0);
        assert(SUCCEEDED(res));
    }


    ~RenderingContext() {
        if (deviceContext_) {
            deviceContext_->Flush();
            deviceContext_->ClearState();
        }
    }

private:


    static void checkResult(const HRESULT result, const char* message) {
        if (FAILED(result)) {
            spdlog::critical(message);
            std::exit(1);
        }
    }
};


}
