#include "ParticleTextures.hpp"
#include <engine/rendering/Particles.hpp>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
namespace engine {
using Microsoft::WRL::ComPtr;
struct ParticleTextures::Impl {
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cb;
    std::vector<ComPtr<ID3D11ShaderResourceView>> sources;
    void Upload(ID3D11ShaderResourceView* source, UINT layer) {
        UINT data[4]{layer,source?1u:0u,0,0};
        ctx->UpdateSubresource(cb.Get(),0,nullptr,data,0,0);
        ctx->CSSetShader(shader.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(0,1,&source); ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
        ctx->Dispatch(ParticleTextureSize/8,ParticleTextureSize/8,1);
        ID3D11ShaderResourceView* s{}; ID3D11UnorderedAccessView* u{}; ID3D11Buffer* b{}; ID3D11SamplerState* sm{};
        ctx->CSSetShaderResources(0,1,&s); ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        ctx->CSSetConstantBuffers(0,1,&b); ctx->CSSetSamplers(0,1,&sm); ctx->CSSetShader(nullptr,nullptr,0);
    }
};
ParticleTextures::ParticleTextures()=default;
ParticleTextures::~ParticleTextures()=default;
long ParticleTextures::Initialize(ID3D11Device* d, ID3D11ComputeShader* shader) {
    if(!d||!shader||d->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0) return E_INVALIDARG;
    ComPtr<ID3D11Device> owner; shader->GetDevice(&owner);
    if(owner.Get()!=d) return E_INVALIDARG;
    auto a=std::make_unique<Impl>(); d->GetImmediateContext(&a->ctx); a->shader=shader;
    D3D11_TEXTURE2D_DESC t{}; t.Width=t.Height=ParticleTextureSize; t.ArraySize=ParticleTextureLayers;
    t.MipLevels=t.SampleDesc.Count=1; t.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    t.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture; HRESULT hr;
    if(FAILED(hr=d->CreateTexture2D(&t,nullptr,&texture)) || FAILED(hr=d->CreateShaderResourceView(texture.Get(),nullptr,&a->srv)) ||
       FAILED(hr=d->CreateUnorderedAccessView(texture.Get(),nullptr,&a->uav))) return hr;
    D3D11_BUFFER_DESC b{}; b.ByteWidth=16; b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(hr=d->CreateBuffer(&b,nullptr,&a->cb))) return hr;
    D3D11_SAMPLER_DESC s{}; s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(hr=d->CreateSamplerState(&s,&a->sampler))) return hr;
    a->sources.emplace_back(); a->Upload(nullptr,0); impl_=std::move(a); return S_OK;
}
long ParticleTextures::Resolve(ID3D11ShaderResourceView* source, std::uint32_t& layer) {
    layer=UINT32_MAX; if(!impl_) return E_UNEXPECTED;
    auto& a=*impl_;
    for(UINT i=0;i<a.sources.size();++i) if(a.sources[i].Get()==source) { layer=i; return S_OK; }
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; source->GetDesc(&desc);
    if(desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D) return E_INVALIDARG;
    ComPtr<ID3D11Device> owner,device; source->GetDevice(&owner); a.ctx->GetDevice(&device);
    if(owner.Get()!=device.Get()) return E_INVALIDARG;
    if(a.sources.size()==ParticleTextureLayers) return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS); // caller logs
    layer=static_cast<UINT>(a.sources.size()); a.sources.emplace_back(source); a.Upload(source,layer); return S_OK;
}
ID3D11ShaderResourceView* ParticleTextures::View() const { return impl_ ? impl_->srv.Get() : nullptr; }
}
