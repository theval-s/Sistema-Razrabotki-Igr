#pragma once
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <initializer_list>
#include <optional>
#include <engine/engine_export.hpp>
#include <wrl/client.h>

#include "BindSlot.hpp"
#include "Buffer.hpp"
#include "GraphicalState.hpp"
#include "RenderedMesh.h"
#include "RenderingResourceManager.hpp"
#include "Texture.hpp"

namespace engine {
using Microsoft::WRL::ComPtr;

/**
 * Simple thin wrapper around DX11 Context, helps to set shaders and resources via Handles
 * To simplify architecture we are keeping most resources bound during render pass
 * Each resource has reserved slot so that's no problem.
 * If pass requires to reuse some out-resources as in-resources, it must inbind all out-resources explicitly or wise-versa
 */
struct ENGINE_API RenderingContext {
    RenderingContext();
    ~RenderingContext();

    void Initialize(uint32_t screenWidth = 1000, uint32_t screenHeight = 600);

    [[nodiscard]] RenderingResourceManager& GetResourceManager();
    [[nodiscard]] ID3D11Device* GetDevice() const;
    [[nodiscard]] ID3D11DeviceContext* GetDeviceContext() const;
    [[nodiscard]] IDXGISwapChain* GetSwapChain() const;

    void SetShader(ShaderHandle h);
    void SetViewport(uint32_t width, uint32_t height) const;
    void BindMesh(MeshHandle h);
    void RawDrawIndexed(uint32_t indexCount, uint32_t baseIndex = 0, uint32_t baseVertex = 0) const;
    void RawDraw(uint32_t vertexCount, uint32_t baseVertex = 0) const;
    void DrawMesh(MeshHandle h);
    void BindCBuffer(BindSlot slot, BufferHandle h);
    void SetRenderTargets(std::initializer_list<TextureHandle> targets, TextureHandle depth,
                          bool depthReadOnly = false);
    void SetShaderResources(BindSlot slot, TextureHandle h);
    void BindSamplers();
    void SetBlendState(BlendState state, const float* blendFactor = nullptr,
                       UINT sampleMask = 0xffffffff) const;
    void SetDepthState(DepthState state, UINT stencilRef = 0) const;
    void SetRasterizerState(RasterizerState state) const;
    void SetGraphicalState(GraphicalState state) const;
    void ClearState();
    void ClearHazardousState() const;
    void ClearDepthView(TextureHandle h, std::optional<float> depth = 1.f,
                        std::optional<UINT> stencil = std::nullopt);
    void ClearRenderTargetView(TextureHandle h,
                               std::array<float, 4> color = {0.f, 0.f, 0.f, 0.f});
    void UpdateBuffer(const GpuBuffer& buffer, const void* data, uint32_t size) const;

    template<typename T>
    void UpdateCBuffer(const BufferHandle handle, const T& data) {
        const auto& buffer = resourceManager_.buffers_.Get(handle);
        UpdateBuffer(buffer, reinterpret_cast<const void*>(&data), sizeof(T));
    }

    void Present() const;

private:
    static void checkResult(HRESULT result, const char* message);

    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> deviceContext_;
    RenderingResourceManager resourceManager_;
    RenderStates renderingStates_;
    ShaderLayoutHandle vsInputLayout_{};
};

}
