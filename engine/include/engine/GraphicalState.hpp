#pragma once
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

#include <engine/engine_export.hpp>

namespace engine {
using Microsoft::WRL::ComPtr;

enum class BlendState : uint8_t {
    Opaque,
    AlphaBlend,
    Additive,

    Count
};

enum class DepthState : uint8_t {
    Disabled,
    ReadWrite, //.DepthFunc = D3D11_COMPARISON_LESS; //todo: think if we'll use Reverse-Z everywhere
    ReadOnly, //.DepthFunc = D3D11_COMPARISON_GREATER;

    Count
};

enum class RasterizerState : uint8_t {
    CullBack,
    CullBackWithBias,
    CullFront,
    CullNone,
    Wireframe,

    Count
};

struct GraphicalState {
    BlendState blendState;
    DepthState depthState;
    RasterizerState rasterizerState;
};

enum class SamplerState
{
    PointClamp,
    LinearClamp,
    LinearWrap,
    AnisotropicWrap,

    ShadowComparison,

    Count
};

struct RenderStates {
    ENGINE_API void Initialize(ID3D11Device* device);
    ENGINE_API ID3D11BlendState* GetBlendState(BlendState blendState) const;
    ENGINE_API ID3D11DepthStencilState* GetDepthState(DepthState depthState) const;
    ENGINE_API ID3D11RasterizerState* GetRasterizerState(RasterizerState rasterizerState) const;
    ENGINE_API ID3D11SamplerState* GetSamplerState(SamplerState samplerState) const;

private:
    void InitializeBlendStates(ID3D11Device* device);
    void InitializeDepthStates(ID3D11Device* device);
    void InitializeRasterizerStates(ID3D11Device* device);
    void InitializeSamplerStates(ID3D11Device* device);

    std::array<ComPtr<ID3D11BlendState>, static_cast<size_t>(BlendState::Count)> blendStates_;
    std::array<ComPtr<ID3D11DepthStencilState>, static_cast<size_t>(DepthState::Count)> depthStates_;
    std::array<ComPtr<ID3D11RasterizerState>, static_cast<size_t>(RasterizerState::Count)> rasterizerStates_;
    std::array<ComPtr<ID3D11SamplerState>, static_cast<size_t>(SamplerState::Count)> samplerStates_;
};

}
