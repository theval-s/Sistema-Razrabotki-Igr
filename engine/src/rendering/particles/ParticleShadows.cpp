#include "ParticleShadows.hpp"
#include <engine/rendering/RenderingResources.hpp>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
namespace engine {
using Microsoft::WRL::ComPtr;
struct ParticleShadows::Impl {
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv[4];
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> sampler;
    UINT capacity{};
};
ParticleShadows::ParticleShadows()=default;
ParticleShadows::~ParticleShadows()=default;
long ParticleShadows::Initialize(ID3D11Device* d, Programs p, UINT capacity) {
    if(!d||!p.vertex||!p.pixel||!capacity||capacity>65535*64||d->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0) return E_INVALIDARG;
    ComPtr<ID3D11Device> owner; p.vertex->GetDevice(&owner);
    if(owner.Get()!=d) return E_INVALIDARG;
    owner.Reset(); p.pixel->GetDevice(&owner);
    if(owner.Get()!=d) return E_INVALIDARG;
    auto a=std::make_unique<Impl>(); d->GetImmediateContext(&a->ctx); a->vs=p.vertex; a->ps=p.pixel; a->capacity=capacity;
    D3D11_TEXTURE2D_DESC t{}; t.Width=t.Height=ParticleShadowMapSize; t.ArraySize=4;
    t.MipLevels=t.SampleDesc.Count=1; t.Format=DXGI_FORMAT_R16_FLOAT; t.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture; HRESULT hr;
    if(FAILED(hr=d->CreateTexture2D(&t,nullptr,&texture)) || FAILED(hr=d->CreateShaderResourceView(texture.Get(),nullptr,&a->srv))) return hr;
    for(UINT i=0;i<4;++i) {
        D3D11_RENDER_TARGET_VIEW_DESC r{}; r.Format=t.Format; r.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        r.Texture2DArray.FirstArraySlice=i; r.Texture2DArray.ArraySize=1;
        if(FAILED(hr=d->CreateRenderTargetView(texture.Get(),&r,&a->rtv[i]))) return hr;
    }
    D3D11_BUFFER_DESC b{}; b.ByteWidth=80; b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(hr=d->CreateBuffer(&b,nullptr,&a->cb))) return hr;
    D3D11_BLEND_DESC blend{}; auto& rt=blend.RenderTarget[0]; rt.BlendEnable=TRUE;
    rt.SrcBlend=D3D11_BLEND_ZERO; rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp=D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha=D3D11_BLEND_ZERO; rt.DestBlendAlpha=D3D11_BLEND_ONE; rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    if(FAILED(hr=d->CreateBlendState(&blend,&a->blend))) return hr;
    D3D11_RASTERIZER_DESC r{}; r.FillMode=D3D11_FILL_SOLID; r.CullMode=D3D11_CULL_NONE; r.DepthClipEnable=TRUE;
    if(FAILED(hr=d->CreateRasterizerState(&r,&a->raster))) return hr;
    D3D11_DEPTH_STENCIL_DESC z{};
    if(FAILED(hr=d->CreateDepthStencilState(&z,&a->depth))) return hr;
    D3D11_SAMPLER_DESC s{}; s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(hr=d->CreateSamplerState(&s,&a->sampler))) return hr;
    const float one[4]{1,1,1,1}; for(auto& target:a->rtv) a->ctx->ClearRenderTargetView(target.Get(),one);
    impl_=std::move(a); return S_OK;
}
long ParticleShadows::Render(ParticleSimulation::Views v, ID3D11ShaderResourceView* textures,
                            const CascadeInfo& cascades, ID3D11ShaderResourceView* opaque) {
    if(!impl_) return E_UNEXPECTED; auto& a=*impl_; auto* c=a.ctx.Get();
    const float one[4]{1,1,1,1}; for(auto& target:a.rtv) c->ClearRenderTargetView(target.Get(),one);
    if(!v.particles||!v.activeCount||!v.activeIndices||!opaque||!v.upperAlive) return S_OK;
    ID3D11ShaderResourceView* srvs[10]{v.particles,v.activeIndices,v.activeCount,opaque,nullptr,nullptr,nullptr,nullptr,v.settings,textures};
    c->VSSetShaderResources(0,10,srvs); c->PSSetShaderResources(0,10,srvs);
    c->VSSetShader(a.vs.Get(),nullptr,0); c->PSSetShader(a.ps.Get(),nullptr,0); c->GSSetShader(nullptr,nullptr,0);
    c->HSSetShader(nullptr,nullptr,0); c->DSSetShader(nullptr,nullptr,0);
    c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->OMSetBlendState(a.blend.Get(),nullptr,UINT32_MAX); c->OMSetDepthStencilState(a.depth.Get(),0); c->RSSetState(a.raster.Get());
    D3D11_VIEWPORT viewport{0,0,float(ParticleShadowMapSize),float(ParticleShadowMapSize),0,1}; c->RSSetViewports(1,&viewport);
    c->VSSetConstantBuffers(0,1,a.cb.GetAddressOf()); c->PSSetConstantBuffers(0,1,a.cb.GetAddressOf());
    c->PSSetSamplers(0,1,a.sampler.GetAddressOf());
    for(UINT i=0;i<4;++i) {
        struct Frame { Matrix matrix; UINT slice,targetSize,pad[2]; };
        static_assert(sizeof(Frame)==80);
        const Frame f{cascades.bufferData.viewProjections[i],i,ParticleShadowMapSize,{}};
        c->UpdateSubresource(a.cb.Get(),0,nullptr,&f,0,0); c->OMSetRenderTargets(1,a.rtv[i].GetAddressOf(),nullptr);
        c->DrawInstanced(6,(std::min)(v.upperAlive,a.capacity),0,0);
    }
    c->OMSetRenderTargets(0,nullptr,nullptr);
    ID3D11ShaderResourceView* nulls[10]{}; c->VSSetShaderResources(0,10,nulls); c->PSSetShaderResources(0,10,nulls);
    ID3D11Buffer* b{}; c->VSSetConstantBuffers(0,1,&b); c->PSSetConstantBuffers(0,1,&b);
    ID3D11SamplerState* s{}; c->PSSetSamplers(0,1,&s); c->VSSetShader(nullptr,nullptr,0); c->PSSetShader(nullptr,nullptr,0);
    return S_OK;
}
ID3D11ShaderResourceView* ParticleShadows::View() const { return impl_ ? impl_->srv.Get() : nullptr; }
}
