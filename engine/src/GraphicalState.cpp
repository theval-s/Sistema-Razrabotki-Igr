#include "engine/GraphicalState.hpp"

#include <cassert>
#include <stdexcept>
#include <fmt/base.h>

namespace engine {

void RenderStates::Initialize(ID3D11Device* device) {
    InitializeBlendStates(device);
    InitializeDepthStates(device);
    InitializeRasterizerStates(device);
    InitializeSamplerStates(device);
}

void RenderStates::InitializeBlendStates(ID3D11Device* device) {
    auto init = [&](const D3D11_BLEND_DESC& desc, BlendState type){
        const HRESULT hr = device->CreateBlendState(
            &desc, blendStates_[static_cast<size_t>(type)].GetAddressOf()
        );
        assert(SUCCEEDED(hr));
    };
    // Opaque
    {
        D3D11_BLEND_DESC desc{};

        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;

        auto& rt = desc.RenderTarget[0];
        rt.BlendEnable = FALSE;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        init(desc, BlendState::Opaque);
    }

    // Standard alpha blending:
    //
    // out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)
    //
    {
        D3D11_BLEND_DESC desc{};
        auto& rt = desc.RenderTarget[0];
        rt.BlendEnable = TRUE;

        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;

        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        init(desc, BlendState::AlphaBlend);
    }

    // Additive:
    //
    // out.rgb = src.rgb + dst.rgb
    //
    {
        D3D11_BLEND_DESC desc{};

        auto& rt = desc.RenderTarget[0];
        rt.BlendEnable = TRUE;

        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ONE;
        rt.BlendOp = D3D11_BLEND_OP_ADD;

        rt.SrcBlendAlpha = D3D11_BLEND_ZERO; //Additive blending doesn't touch alpha, handy for deferred lighting
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        init(desc, BlendState::Additive);
    }
}

void RenderStates::InitializeDepthStates(ID3D11Device* device) {
    auto init = [&](const D3D11_DEPTH_STENCIL_DESC& desc, DepthState type){
        const HRESULT hr = device->CreateDepthStencilState(
            &desc, depthStates_[static_cast<size_t>(type)].GetAddressOf()
        );
        assert(SUCCEEDED(hr));
    };

    // Disabled
    {
        D3D11_DEPTH_STENCIL_DESC desc{};

        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_ALWAYS;

        desc.StencilEnable = FALSE;

        init(desc, DepthState::Disabled);
    }

    // ReadWrite
    {
        D3D11_DEPTH_STENCIL_DESC desc{};

        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_LESS;

        desc.StencilEnable = FALSE;

        init(desc, DepthState::ReadWrite);
    }

    // ReadOnly
    {
        D3D11_DEPTH_STENCIL_DESC desc{};

        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_GREATER;

        desc.StencilEnable = FALSE;

        init(desc, DepthState::ReadOnly);
    }
}

void RenderStates::InitializeRasterizerStates(ID3D11Device* device) {
    auto init = [&](const D3D11_RASTERIZER_DESC& desc, RasterizerState type){
        const HRESULT hr = device->CreateRasterizerState(
            &desc, rasterizerStates_[static_cast<size_t>(type)].GetAddressOf()
        );
        assert(SUCCEEDED(hr));
    };

    // CullBack
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_BACK;

        desc.FrontCounterClockwise = FALSE;

        desc.DepthBias = 0;
        desc.DepthBiasClamp = 0.0f;
        desc.SlopeScaledDepthBias = 0.0f;

        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = FALSE;
        desc.MultisampleEnable = FALSE;
        desc.AntialiasedLineEnable = FALSE;

        init(desc, RasterizerState::CullBack);
    }
    
    // CullBackWithBias
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_BACK;

        desc.FrontCounterClockwise = FALSE;

        desc.DepthBias = 10000;
        desc.DepthBiasClamp = 0.0f;
        desc.SlopeScaledDepthBias = 2.0f;

        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = FALSE;
        desc.MultisampleEnable = FALSE;
        desc.AntialiasedLineEnable = FALSE;

        init(desc, RasterizerState::CullBackWithBias);
    }

    // CullFront
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_FRONT;

        desc.FrontCounterClockwise = FALSE;
        desc.DepthClipEnable = TRUE;

        init(desc, RasterizerState::CullFront);
    }

    // CullNone
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;

        desc.FrontCounterClockwise = FALSE;
        desc.DepthClipEnable = TRUE;

        init(desc, RasterizerState::CullNone);
    }

    // Wireframe
    {
        D3D11_RASTERIZER_DESC desc{};

        desc.FillMode = D3D11_FILL_WIREFRAME;
        desc.CullMode = D3D11_CULL_NONE;

        desc.FrontCounterClockwise = FALSE;
        desc.DepthClipEnable = TRUE;

        init(desc, RasterizerState::Wireframe);
    }
}

void RenderStates::InitializeSamplerStates(ID3D11Device* device) {
    auto init = [&](const D3D11_SAMPLER_DESC& desc, SamplerState type){
        const HRESULT hr = device->CreateSamplerState(
            &desc, samplerStates_[static_cast<size_t>(type)].GetAddressOf()
        );
        assert(SUCCEEDED(hr));
    };

    // PointClamp
    {
        D3D11_SAMPLER_DESC desc{};

        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        
        init(desc, SamplerState::PointClamp);
    }

    // LinearClamp
    {
        D3D11_SAMPLER_DESC desc{};

        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;

        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        
        init(desc, SamplerState::LinearClamp);
    }

    // LinearWrap
    {
        D3D11_SAMPLER_DESC desc{};

        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;

        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        
        init(desc, SamplerState::LinearWrap);
    }

    // AnisotropicWrap
    {
        D3D11_SAMPLER_DESC desc{};

        desc.Filter = D3D11_FILTER_ANISOTROPIC;

        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 16;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        
        init(desc, SamplerState::AnisotropicWrap);
    }

    // ShadowComparison
    {
        D3D11_SAMPLER_DESC desc{};

        desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;

        desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;

        desc.BorderColor[0] = 1.0f;
        desc.BorderColor[1] = 1.0f;
        desc.BorderColor[2] = 1.0f;
        desc.BorderColor[3] = 1.0f;
        
        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 1;

        desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        
        init(desc, SamplerState::ShadowComparison);
    }
}

}
