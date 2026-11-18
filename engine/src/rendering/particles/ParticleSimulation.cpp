#include "ParticleSimulation.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace engine {
using Microsoft::WRL::ComPtr;
namespace {
struct Buffer {
    ComPtr<ID3D11Buffer> data;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
HRESULT MakeBuffer(ID3D11Device* device, Buffer& out, UINT n, UINT stride, UINT binds,
                   UINT misc, const void* initial = nullptr) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = n * stride;
    d.BindFlags = binds;
    d.MiscFlags = misc;
    if (misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init{initial, 0, 0};
    HRESULT hr = device->CreateBuffer(&d, initial ? &init : nullptr, &out.data);
    if (FAILED(hr)) return hr;
    if (binds & D3D11_BIND_SHADER_RESOURCE) {
        hr = device->CreateShaderResourceView(out.data.Get(), nullptr, &out.srv);
        if (FAILED(hr)) return hr;
    }
    if (binds & D3D11_BIND_UNORDERED_ACCESS) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Format = d.StructureByteStride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
        u.Buffer.NumElements = n;
        hr = device->CreateUnorderedAccessView(out.data.Get(), &u, &out.uav);
    }
    return hr;
}
struct GpuEmitter { float posLife[4], dirSpeed[4], tintRadius[4], jitter[4]; UINT request[4]; };
static_assert(sizeof(GpuEmitter) == 80);
struct Frame { float dt; UINT capacity, slots, frame; };
struct Slot {
    std::optional<std::uint64_t> owner;
    std::uint64_t lastBirthRequest{}, knownEmpty{};
};
struct Ticket {
    ComPtr<ID3D11Buffer> staging;
    ComPtr<ID3D11Query> done;
    std::uint64_t frame{};
    UINT birthsAfter{}; // conservative bound on births submitted since this copy
    std::vector<std::optional<std::uint64_t>> owners;
    std::vector<ParticleSimulation::EmitterStats> overflow;
};
bool Valid(const ParticleSimulation::Emitter& e) {
    auto finite = [](float x) { return std::isfinite(x); };
    return std::all_of(e.position,e.position+3,finite) && std::all_of(e.tint,e.tint+3,finite) &&
        std::all_of(e.direction,e.direction+3,finite) && finite(e.lifetime) && e.lifetime>0 &&
        finite(e.speed) && finite(e.spread) && e.spread>=0 && finite(e.radius) && e.radius>=0;
}
}

struct ParticleSimulation::Impl {
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> update, admit, spawn;
    Buffer pool[2], count[2], indices, args, births, inputs, stats, cb;
    UINT capacity{}, current{}, read{}, pending{}, upperAlive{};
    std::uint64_t frame{};
    std::vector<Slot> slots;
    std::vector<GpuEmitter> upload;
    std::array<Ticket,3> ring;
    Snapshot latest;

    HRESULT Poll() {
        while (pending) {
            auto& t = ring[read];
            HRESULT hr = ctx->GetData(t.done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr != S_OK) return hr;
            Snapshot s;
            s.frame = t.frame;
            s.emitters.reserve(slots.size()+t.overflow.size());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = ctx->Map(t.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return S_FALSE;
            if (FAILED(hr)) return hr;
            const auto* words = static_cast<const UINT*>(mapped.pData);
            for (size_t i=0; i<slots.size(); ++i) {
                if (!t.owners[i]) continue;
                UINT alive=words[4*i], rejected=words[4*i+1];
                s.emitters.push_back({*t.owners[i],alive,rejected});
                s.alive += alive;
                s.rejected += rejected;
                if (!alive && slots[i].owner == t.owners[i]) slots[i].knownEmpty = t.frame;
            }
            ctx->Unmap(t.staging.Get(),0);
            for (auto& row : t.overflow) { s.rejected += row.rejected; s.emitters.push_back(row); }
            upperAlive = (std::min)(capacity, s.alive + t.birthsAfter);
            latest = std::move(s);
            read = (read+1)%3;
            --pending;
        }
        return S_OK;
    }
    void Unbind() {
        ID3D11ShaderResourceView* srvs[3]{};
        ID3D11UnorderedAccessView* uavs[5]{};
        ctx->CSSetShaderResources(0,3,srvs);
        ctx->CSSetUnorderedAccessViews(0,5,uavs,nullptr);
    }
};

ParticleSimulation::ParticleSimulation() = default;
ParticleSimulation::~ParticleSimulation() = default;
long ParticleSimulation::Initialize(ID3D11Device* d, Programs p, UINT cap, UINT n) {
    if (!d || d->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0 || !p.update || !p.admit || !p.spawn ||
        !cap || cap>65535*64 || !n || n>4096) return E_INVALIDARG;
    for (auto* shader : {p.update,p.admit,p.spawn}) {
        ComPtr<ID3D11Device> owner; shader->GetDevice(&owner);
        if (owner.Get()!=d) return E_INVALIDARG;
    }
    auto impl = std::make_unique<Impl>();
    auto& a = *impl;
    d->GetImmediateContext(&a.ctx);
    a.capacity=cap; a.update=p.update; a.admit=p.admit; a.spawn=p.spawn;
    a.slots.resize(n); a.upload.resize(n);
    constexpr UINT structured=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, srv=D3D11_BIND_SHADER_RESOURCE,
        uav=D3D11_BIND_UNORDERED_ACCESS, indirect=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    HRESULT hr;
    const UINT empty[]{0,1,1};
    for (unsigned i=0;i<2;++i) {
        if (FAILED(hr=MakeBuffer(d,a.pool[i],cap,sizeof(Particle),srv|uav,structured))) return hr;
        if (FAILED(hr=MakeBuffer(d,a.count[i],1,4,srv|uav,structured,empty))) return hr;
    }
    std::vector<UINT> identity(cap);
    std::iota(identity.begin(),identity.end(),0u);
    if (FAILED(hr=MakeBuffer(d,a.indices,cap,4,srv,structured,identity.data()))) return hr;
    if (FAILED(hr=MakeBuffer(d,a.args,3,4,uav,indirect,empty))) return hr;
    if (FAILED(hr=MakeBuffer(d,a.births,3,4,uav,indirect,empty))) return hr;
    if (FAILED(hr=MakeBuffer(d,a.inputs,n,sizeof(GpuEmitter),srv,structured))) return hr;
    if (FAILED(hr=MakeBuffer(d,a.stats,n,16,srv|uav,structured))) return hr;
    if (FAILED(hr=MakeBuffer(d,a.cb,1,sizeof(Frame),D3D11_BIND_CONSTANT_BUFFER,0))) return hr;
    D3D11_BUFFER_DESC staging{};
    staging.ByteWidth=n*16; staging.Usage=D3D11_USAGE_STAGING; staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC event{D3D11_QUERY_EVENT,0};
    for (auto& ticket:a.ring) {
        ticket.owners.resize(n);
        if (FAILED(hr=d->CreateBuffer(&staging,nullptr,&ticket.staging))) return hr;
        if (FAILED(hr=d->CreateQuery(&event,&ticket.done))) return hr;
    }
    impl_=std::move(impl);
    return S_OK;
}
long ParticleSimulation::Step(float dt, std::span<const Emitter> emitters) {
    if (!impl_) return E_UNEXPECTED;
    if (!std::isfinite(dt) || dt<0) return E_INVALIDARG;
    std::unordered_set<std::uint64_t> present;
    for (const auto& e:emitters) if (!Valid(e) || !present.insert(e.owner).second) return E_INVALIDARG;
    auto& a=*impl_;
    HRESULT hr=a.Poll();
    if (FAILED(hr)) return hr;
    ++a.frame;
    Ticket* ticket = a.pending<3 ? &a.ring[(a.read+a.pending)%3] : nullptr;
    if (ticket) ticket->overflow.clear();
    std::unordered_map<std::uint64_t,UINT> assigned;
    std::vector<UINT> free;
    const UINT n=static_cast<UINT>(a.slots.size());
    for (UINT j=n;j-- > 0;) {
        auto& s=a.slots[j];
        if (s.owner && !present.contains(*s.owner) && s.knownEmpty>=s.lastBirthRequest) s={};
        if (s.owner) assigned.emplace(*s.owner,j); else free.push_back(j);
    }
    std::fill(a.upload.begin(),a.upload.end(),GpuEmitter{});
    UINT possibleBirths = 0;
    for (auto& e:emitters) {
        auto it=assigned.find(e.owner);
        UINT j;
        if (it!=assigned.end()) j=it->second;
        else {
            if (free.empty()) { if (ticket) ticket->overflow.push_back({e.owner,0,e.requested}); continue; }
            j=free.back(); free.pop_back(); a.slots[j].owner=e.owner;
        }
        if (e.requested) a.slots[j].lastBirthRequest=a.frame;
        possibleBirths += (std::min)({a.capacity - possibleBirths, e.requested, e.maxAlive});
        a.upload[j]={{e.position[0],e.position[1],e.position[2],e.lifetime},
                     {e.direction[0],e.direction[1],e.direction[2],e.speed},
                     {e.tint[0],e.tint[1],e.tint[2],e.radius},{e.spread,0,0,0},
                     {e.requested,e.maxAlive,e.appearance,e.seed}};
    }
    auto* c=a.ctx.Get();
    Frame f{dt,a.capacity,n,static_cast<UINT>(a.frame)};
    c->UpdateSubresource(a.cb.data.Get(),0,nullptr,&f,0,0);
    c->UpdateSubresource(a.inputs.data.Get(),0,nullptr,a.upload.data(),0,0);
    c->CSSetConstantBuffers(0,1,a.cb.data.GetAddressOf());
    UINT prev=a.current, next=prev^1, zero[4]{};
    c->ClearUnorderedAccessViewUint(a.stats.uav.Get(),zero);
    c->ClearUnorderedAccessViewUint(a.count[next].uav.Get(),zero);
    ID3D11ShaderResourceView* previous[]={a.pool[prev].srv.Get(),a.count[prev].srv.Get()};
    ID3D11UnorderedAccessView* survivors[]={a.pool[next].uav.Get(),a.count[next].uav.Get(),a.stats.uav.Get()};
    c->CSSetShader(a.update.Get(),nullptr,0);
    c->CSSetShaderResources(0,2,previous);
    c->CSSetUnorderedAccessViews(0,3,survivors,nullptr);
    c->DispatchIndirect(a.args.data.Get(),0);
    a.Unbind();

    ID3D11UnorderedAccessView* reservations[]={a.count[next].uav.Get(),a.stats.uav.Get(),a.args.uav.Get(),a.births.uav.Get()};
    c->CSSetShader(a.admit.Get(),nullptr,0);
    c->CSSetShaderResources(0,1,a.inputs.srv.GetAddressOf());
    c->CSSetUnorderedAccessViews(1,4,reservations,nullptr);
    c->Dispatch(1,1,1);
    a.Unbind();

    ID3D11ShaderResourceView* requests[]={a.inputs.srv.Get(),a.stats.srv.Get(),a.count[next].srv.Get()};
    c->CSSetShader(a.spawn.Get(),nullptr,0);
    c->CSSetShaderResources(0,3,requests);
    c->CSSetUnorderedAccessViews(0,1,a.pool[next].uav.GetAddressOf(),nullptr);
    c->DispatchIndirect(a.births.data.Get(),0);
    a.Unbind();
    ID3D11Buffer* nullCB=nullptr;
    c->CSSetConstantBuffers(0,1,&nullCB);
    c->CSSetShader(nullptr,nullptr,0);
    a.current=next;
    // A delayed count alone is unsafe for dispatch sizing: account for every intervening
    // birth request, including frames where the staging ring had no free ticket.
    a.upperAlive = (std::min)(a.capacity, a.upperAlive + possibleBirths);
    for (UINT i = 0; i < a.pending; ++i) {
        auto& t = a.ring[(a.read + i) % 3];
        t.birthsAfter = (std::min)(a.capacity, t.birthsAfter + possibleBirths);
    }
    if (ticket) {
        ticket->frame=a.frame;
        ticket->birthsAfter=0;
        for (UINT i=0;i<n;++i) ticket->owners[i]=a.slots[i].owner;
        c->CopyResource(ticket->staging.Get(),a.stats.data.Get());
        c->End(ticket->done.Get());
        ++a.pending;
    }
    return S_OK;
}
ParticleSimulation::Views ParticleSimulation::RenderViews() const {
    if (!impl_) return {};
    auto& a=*impl_;
    return {a.pool[a.current].srv.Get(),a.indices.srv.Get(),a.count[a.current].srv.Get(),a.args.data.Get(),a.upperAlive};
}
const ParticleSimulation::Snapshot* ParticleSimulation::Latest() {
    if (!impl_ || FAILED(impl_->Poll()) || !impl_->latest.frame) return nullptr;
    return &impl_->latest;
}
} // namespace engine
