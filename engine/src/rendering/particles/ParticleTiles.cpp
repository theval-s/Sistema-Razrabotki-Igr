#include "ParticleTiles.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>

namespace engine {
using Microsoft::WRL::ComPtr;
namespace {
struct Storage {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    HRESULT Create(ID3D11Device* d, UINT count, UINT stride) {
        D3D11_BUFFER_DESC b{};
        b.ByteWidth=count*stride; b.StructureByteStride=stride;
        b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        b.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr=d->CreateBuffer(&b,nullptr,&buffer);
        if (FAILED(hr)) return hr;
        if (FAILED(hr=d->CreateShaderResourceView(buffer.Get(),nullptr,&srv))) return hr;
        return d->CreateUnorderedAccessView(buffer.Get(),nullptr,&uav);
    }
};
struct Frame {
    ParticleTiles::Camera camera;
    UINT width,height,coarse,words,columns,level,step,pad;
};
static_assert(sizeof(Frame)==160);
struct Ticket {
    ComPtr<ID3D11Buffer> staging;
    ComPtr<ID3D11Query> ready;
    std::uint64_t frame{};
};
UINT DivUp(UINT n, UINT d) { return (n+d-1)/d; }
bool Texture(ID3D11ShaderResourceView* srv, UINT w, UINT h, DXGI_FORMAT format,
             ComPtr<ID3D11Texture2D>& tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    srv->GetDesc(&s);
    if (s.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || s.Format!=format) return false;
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    if (FAILED(resource.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC t{};
    tex->GetDesc(&t);
    return t.Width==w && t.Height==h && t.ArraySize==1 && t.MipLevels==1 && t.SampleDesc.Count==1;
}
}

struct ParticleTiles::Impl {
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> build,merge,bin,shade;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11UnorderedAccessView> imageUav;
    Storage keys,sprites,masks,stats;
    Frame f{};
    UINT padded{},read{},pending{};
    Ticket ring[3];
    Stats latest{};
    std::uint64_t frame{};

    HRESULT Poll() {
        while (pending) {
            auto& t=ring[read];
            HRESULT hr=ctx->GetData(t.ready.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr!=S_OK) return hr;
            D3D11_MAPPED_SUBRESOURCE m{};
            hr=ctx->Map(t.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
            if (hr==DXGI_ERROR_WAS_STILL_DRAWING) return S_FALSE;
            if (FAILED(hr)) return hr;
            auto data=static_cast<const UINT*>(m.pData);
            latest={t.frame,data[0],data[1]};
            ctx->Unmap(t.staging.Get(),0);
            read=(read+1)%3; --pending;
        }
        return S_OK;
    }
    void Unbind() {
        ID3D11ShaderResourceView* s[8]{};
        ID3D11UnorderedAccessView* u[3]{};
        ctx->CSSetShaderResources(0,8,s);
        ctx->CSSetUnorderedAccessViews(0,3,u,nullptr);
    }
};

ParticleTiles::ParticleTiles()=default;
ParticleTiles::~ParticleTiles()=default;
long ParticleTiles::Initialize(ID3D11Device* d, Programs p, UINT capacity, UINT w, UINT h) {
    if (!d || d->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0 || !capacity || capacity>65535*64 ||
        !w || !h || w>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || h>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return E_INVALIDARG;
    for (auto shader:{p.build,p.merge,p.bin,p.shade}) {
        if (!shader) return E_INVALIDARG;
        ComPtr<ID3D11Device> owner; shader->GetDevice(&owner);
        if (owner.Get()!=d) return E_INVALIDARG;
    }
    auto tmp=std::make_unique<Impl>(); auto& a=*tmp;
    d->GetImmediateContext(&a.ctx);
    a.build=p.build; a.merge=p.merge; a.bin=p.bin; a.shade=p.shade;
    a.padded=std::bit_ceil((std::max)(256u,capacity));
    auto& f=a.f;
    f.width=w; f.height=h; f.words=DivUp(capacity,32);
    UINT regions;
    for (f.coarse=64;;f.coarse*=2) {
        f.columns=DivUp(w,f.coarse); regions=f.columns*DivUp(h,f.coarse);
        if (regions<=(8u<<20)/f.words) break;
    }
    HRESULT hr;
    if (FAILED(hr=a.keys.Create(d,a.padded,16)) || FAILED(hr=a.sprites.Create(d,capacity,32)) ||
        FAILED(hr=a.masks.Create(d,regions*f.words,4)) || FAILED(hr=a.stats.Create(d,2,4))) return hr;
    D3D11_BUFFER_DESC b{}; b.ByteWidth=sizeof(Frame); b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(hr=d->CreateBuffer(&b,nullptr,&a.cb))) return hr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width=w; td.Height=h; td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.MipLevels=td.ArraySize=td.SampleDesc.Count=1; td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(hr=d->CreateTexture2D(&td,nullptr,&a.image)) ||
        FAILED(hr=d->CreateUnorderedAccessView(a.image.Get(),nullptr,&a.imageUav))) return hr;
    b={}; b.ByteWidth=8; b.Usage=D3D11_USAGE_STAGING; b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC event{D3D11_QUERY_EVENT,0};
    for (auto& t:a.ring) {
        if (FAILED(hr=d->CreateBuffer(&b,nullptr,&t.staging)) ||
            FAILED(hr=d->CreateQuery(&event,&t.ready))) return hr;
    }
    impl_=std::move(tmp); return S_OK;
}

long ParticleTiles::Render(ParticleSimulation::Views v, const Camera& camera,
                           ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* lit) {
    if (!impl_) return E_UNEXPECTED;
    if (!v.particles || !v.activeIndices || !v.activeCount || !v.dispatchArgs || !depth || !lit) return E_INVALIDARG;
    auto& a=*impl_; auto& f=a.f; auto* c=a.ctx.Get();
    ComPtr<ID3D11Texture2D> dst,z;
    if (!Texture(lit,f.width,f.height,DXGI_FORMAT_R16G16B16A16_FLOAT,dst) ||
        !Texture(depth,f.width,f.height,DXGI_FORMAT_R32_FLOAT,z)) return E_INVALIDARG;
    HRESULT hr=a.Poll(); if (FAILED(hr)) return hr;
    f.camera=camera;
    c->UpdateSubresource(a.cb.Get(),0,nullptr,&f,0,0);
    c->CSSetConstantBuffers(0,1,a.cb.GetAddressOf());
    ID3D11ShaderResourceView* s[]={v.particles,v.activeIndices,v.activeCount,a.keys.srv.Get(),
                                   a.sprites.srv.Get(),a.masks.srv.Get(),depth,lit};
    ID3D11UnorderedAccessView* buildOut[]={a.keys.uav.Get(),a.sprites.uav.Get()};
    c->CSSetShaderResources(0,3,s);
    c->CSSetUnorderedAccessViews(0,2,buildOut,nullptr);
    c->CSSetShader(a.build.Get(),nullptr,0);
    const UINT extent = std::bit_ceil((std::clamp)(v.upperAlive, 256u, a.padded));
    c->Dispatch(extent/256,1,1);
    c->CSSetShader(a.merge.Get(),nullptr,0);
    for (f.level=512;f.level<=extent;f.level*=2) {
        // One global dispatch per cross-block stride, then finish in group shared memory.
        for (f.step=f.level/2;f.step>=128;f.step/=2) {
            c->UpdateSubresource(a.cb.Get(),0,nullptr,&f,0,0);
            c->Dispatch(extent/256,1,1);
        }
    }
    a.Unbind();
    UINT zero[4]{};
    c->ClearUnorderedAccessViewUint(a.masks.uav.Get(),zero);
    c->ClearUnorderedAccessViewUint(a.stats.uav.Get(),zero);
    c->CSSetShader(a.bin.Get(),nullptr,0);
    c->CSSetShaderResources(2,3,s+2);
    c->CSSetUnorderedAccessViews(0,1,a.masks.uav.GetAddressOf(),nullptr);
    c->DispatchIndirect(v.dispatchArgs,0);
    a.Unbind();
    ID3D11UnorderedAccessView* shadeOut[]={a.imageUav.Get(),a.stats.uav.Get()};
    c->CSSetShader(a.shade.Get(),nullptr,0);
    c->CSSetShaderResources(0,8,s);
    c->CSSetUnorderedAccessViews(0,2,shadeOut,nullptr);
    c->Dispatch(DivUp(f.width,8),DivUp(f.height,8),1);
    a.Unbind();
    c->CSSetShader(nullptr,nullptr,0);
    ID3D11Buffer* nullCB{}; c->CSSetConstantBuffers(0,1,&nullCB);
    c->CopyResource(dst.Get(),a.image.Get());
    ++a.frame;
    if (a.pending<3) {
        auto& t=a.ring[(a.read+a.pending)%3]; t.frame=a.frame;
        c->CopyResource(t.staging.Get(),a.stats.buffer.Get()); c->End(t.ready.Get()); ++a.pending;
    }
    return S_OK;
}
UINT ParticleTiles::CoarsePixels() const { return impl_ ? impl_->f.coarse : 0; }
ParticleTiles::Stats ParticleTiles::Latest() {
    if (!impl_ || FAILED(impl_->Poll())) return {};
    return impl_->latest;
}
} // namespace engine
