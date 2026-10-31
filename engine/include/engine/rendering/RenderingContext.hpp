#pragma once
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <initializer_list>
#include <optional>
#include <span>
#include <windows.h>

#include <engine/core/Surface.hpp>
#include <engine/engine_export.hpp>
#include <wrl/client.h>

#include <engine/rendering/resources/BindSlot.hpp>
#include <engine/rendering/resources/Buffer.hpp>
#include <engine/rendering/GraphicalState.hpp>
#include <engine/rendering/resources/RenderedMesh.hpp>
#include <engine/rendering/RenderingResourceManager.hpp>
#include <engine/rendering/resources/Texture.hpp>
#include <SimpleMath.h>

namespace engine {
using Microsoft::WRL::ComPtr;

/**
 * Simple thin wrapper around DX11 Context, helps to set shaders and resources via Handles
 * To simplify architecture we are keeping most resources bound during render pass
 * Each resource has reserved slot so that's no problem.
 * If pass requires to reuse some out-resources as in-resources, it must unbind all out-resources explicitly and wise-versa
 */
struct ENGINE_API RenderingContext {
    RenderingContext();
    ~RenderingContext();
    
    /// Device and swap chain onto a host-owned window; false when there is nothing to render into.
    bool Initialize(const NativeSurface& surface);
    /// Reinit everything for new differently sized surface
    bool Reinitialize(const NativeSurface& surface) const;

    [[nodiscard]] RenderingResourceManager& GetResourceManager();
    [[nodiscard]] ID3D11Device* GetDevice() const;
    [[nodiscard]] ID3D11DeviceContext* GetDeviceContext() const;
    [[nodiscard]] IDXGISwapChain* GetSwapChain() const;

    void SetShader(ShaderHandle h);
    void SetViewport(uint32_t width, uint32_t height) const;
    void BindMesh(MeshHandle h);
    void RawDrawIndexed(uint32_t indexCount, uint32_t baseIndex = 0, uint32_t baseVertex = 0) const;
    void RawDraw(uint32_t vertexCount, uint32_t baseVertex = 0) const;
    /// Draw world-space line pairs with the currently bound vertex shader (POSITION input).
    void DrawLines(std::span<const DirectX::SimpleMath::Vector3> endpoints);
    void DrawMesh(MeshHandle h);
    void BindCBuffer(BindSlot slot, BufferHandle h);
    void UnbindCBuffer(BindSlot slot);
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
    void ClearDepthView(TextureHandle h, std::optional<float> depth = 0.f, // ReverseZ, so 0 depth is farthest
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
    bool CreateDeviceAndSwapChain(HWND outputWindow, uint32_t width, uint32_t height);

    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> deviceContext_;
    RenderingResourceManager resourceManager_;
    RenderStates renderingStates_;
    ShaderLayoutHandle vsInputLayout_{};
    ComPtr<ID3D11Buffer> lineBuffer_;
    uint32_t lineBufferCapacity_ = 0;
    MeshLayoutHandle lineLayout_{};
};

}
