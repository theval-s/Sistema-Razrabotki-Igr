#include "../engine/src/rendering/particles/ParticleSimulation.hpp"
#include <engine/rendering/resources/ShaderContext.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace {
using Sim = engine::ParticleSimulation;
using Microsoft::WRL::ComPtr;

class ParticleSimulationWarp : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<ComPtr<ID3D11ComputeShader>, 3> shaders;
    Sim sim;

    void SetUp() override {
        ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        const auto path = std::filesystem::path(SRI_SOURCE_ROOT) / "engine/shaders/ParticleSim.hlsl";
        constexpr std::array defines{"PARTICLE_SIM_UPDATE", "PARTICLE_SIM_ADMIT", "PARTICLE_SIM_SPAWN"};
        for (size_t i = 0; i < defines.size(); ++i) {
            const auto cs = engine::ShaderContext::CompileCpu(path, engine::ShaderType::Compute,
                {.defines = {{defines[i], "1"}}});
            ASSERT_HRESULT_SUCCEEDED(device->CreateComputeShader(cs.blob->GetBufferPointer(),
                cs.blob->GetBufferSize(), nullptr, &shaders[i]));
        }
    }

    HRESULT Init(UINT capacity, UINT slots) {
        return sim.Initialize(device.Get(), {shaders[0].Get(), shaders[1].Get(), shaders[2].Get()}, capacity, slots);
    }

    // Wait only in the harness. Callers request frames known to fit in the three-entry ring.
    void Await(uint64_t frame, Sim::Snapshot& out) {
        context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            if (const auto* s = sim.Latest(); s && s->frame >= frame) {
                ASSERT_EQ(s->frame, frame);
                out = *s;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL() << "Timed out waiting for particle snapshot " << frame;
    }

    template<class T>
    void Read(ID3D11Buffer* buffer, std::vector<T>& values) {
        ASSERT_NE(buffer, nullptr);
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        ASSERT_EQ(desc.ByteWidth % sizeof(T), 0u);
        values.resize(desc.ByteWidth / sizeof(T));
        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = desc.ByteWidth;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> copy;
        ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&staging, nullptr, &copy));
        context->CopyResource(copy.Get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_HRESULT_SUCCEEDED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::memcpy(values.data(), mapped.pData, desc.ByteWidth);
        context->Unmap(copy.Get(), 0);
    }

    template<class T>
    void Read(ID3D11ShaderResourceView* view, std::vector<T>& values) {
        ASSERT_NE(view, nullptr);
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Buffer> buffer;
        ASSERT_HRESULT_SUCCEEDED(resource.As(&buffer));
        Read(buffer.Get(), values);
    }

    void Rendered(UINT expected, std::vector<Sim::Particle>& visible) {
        const auto views = sim.RenderViews();
        std::vector<UINT> counts, args, indices;
        std::vector<Sim::Particle> pool;
        ASSERT_NO_FATAL_FAILURE(Read(views.activeCount, counts));
        ASSERT_FALSE(counts.empty());
        ASSERT_EQ(counts[0], expected);
        EXPECT_GE(views.upperAlive, counts[0]);
        ASSERT_NO_FATAL_FAILURE(Read(views.dispatchArgs, args));
        ASSERT_GE(args.size(), 3u);
        EXPECT_EQ(args[0], (expected + 63) / 64);
        EXPECT_EQ(args[1], 1u);
        EXPECT_EQ(args[2], 1u);
        ASSERT_NO_FATAL_FAILURE(Read(views.activeIndices, indices));
        ASSERT_NO_FATAL_FAILURE(Read(views.particles, pool));
        ASSERT_GE(indices.size(), expected);
        std::vector<bool> seen(pool.size());
        visible.clear();
        for (UINT i = 0; i < expected; ++i) {
            const auto index = indices[i];
            ASSERT_LT(index, pool.size());
            ASSERT_FALSE(seen[index]);
            seen[index] = true;
            visible.push_back(pool[index]);
        }
    }
};

void ExpectRow(const Sim::Snapshot& s, uint64_t owner, UINT alive, UINT rejected) {
    SCOPED_TRACE(::testing::Message() << "frame=" << s.frame << " owner=" << owner);
    auto row = std::find_if(s.emitters.begin(), s.emitters.end(), [owner](auto& e) { return e.owner == owner; });
    ASSERT_NE(row, s.emitters.end());
    EXPECT_EQ(row->alive, alive);
    EXPECT_EQ(row->rejected, rejected);
}

TEST_F(ParticleSimulationWarp, RemovedOwnerRetainsAccelerationDragAndSymmetricBirthVariation) {
    ASSERT_HRESULT_SUCCEEDED(Init(256,1));
    Sim::Emitter e{.owner=42,.requested=256,.maxAlive=256,.lifetime=10,.speed=4};
    e.effect.acceleration={2,0,0}; e.effect.drag=1;
    e.effect.lifetimeVariation=.25f; e.effect.speedVariation=.5f;
    ASSERT_HRESULT_SUCCEEDED(sim.Step(0,std::span(&e,1)));
    std::vector<Sim::Particle> born, moved;
    ASSERT_NO_FATAL_FAILURE(Rendered(256,born));
    bool shortLife=false,longLife=false,slow=false,fast=false;
    for(const auto& p:born) {
        EXPECT_GE(p.velocityLifetime[3],7.5f); EXPECT_LE(p.velocityLifetime[3],12.5f);
        EXPECT_GE(p.velocityLifetime[1],2); EXPECT_LE(p.velocityLifetime[1],6);
        shortLife|=p.velocityLifetime[3]<10; longLife|=p.velocityLifetime[3]>10;
        slow|=p.velocityLifetime[1]<4; fast|=p.velocityLifetime[1]>4;
    }
    EXPECT_TRUE(shortLife&&longLife&&slow&&fast);
    ASSERT_HRESULT_SUCCEEDED(sim.Step(.5f,{}));
    ASSERT_NO_FATAL_FAILURE(Rendered(256,moved));
    for(const auto& p:moved) {
        auto previous=std::find_if(born.begin(),born.end(),[&](auto& b){return b.meta[2]==p.meta[2];});
        ASSERT_NE(previous,born.end());
        EXPECT_NEAR(p.velocityLifetime[0],std::exp(-.5f),1e-5f);
        EXPECT_NEAR(p.velocityLifetime[1],previous->velocityLifetime[1]*std::exp(-.5f),1e-5f);
        EXPECT_NEAR(p.positionAge[0],.5f*std::exp(-.5f),1e-5f);
        EXPECT_EQ(p.meta[3],0u);
    }
}

TEST_F(ParticleSimulationWarp, ExactAdmissionAndCompactionAcrossPartialWorkgroups) {
    ASSERT_HRESULT_SUCCEEDED(Init(73, 2));
    Sim::Emitter a{.owner = 0, .requested = UINT32_MAX, .maxAlive = 60, .lifetime = 1};
    Sim::Emitter b{.owner = 1ull << 40, .requested = UINT32_MAX, .maxAlive = 60, .appearance = 7,
                   .position = {4, 5, 6}, .lifetime = 2, .speed = 3, .tint = {.5f, .25f, 1}, .radius = 2};
    std::array emitters{a, b};
    ASSERT_HRESULT_SUCCEEDED(sim.Step(0, emitters));
    Sim::Snapshot s;
    ASSERT_NO_FATAL_FAILURE(Await(1, s));
    EXPECT_EQ(s.alive, 73u);
    EXPECT_EQ(s.rejected, 2ull * UINT32_MAX - 73);
    ExpectRow(s, a.owner, 60, UINT32_MAX - 60);
    ExpectRow(s, b.owner, 13, UINT32_MAX - 13);
    std::vector<Sim::Particle> visible;
    ASSERT_NO_FATAL_FAILURE(Rendered(73, visible));
    std::vector<UINT> born;
    for (auto& p : visible) if (p.meta[1] == 7) born.push_back(p.meta[2]);
    ASSERT_EQ(born.size(), 13u);

    // The entire first cohort dies. The second starts near the end of group 0
    // and extends into group 1. Its seed multiset must survive atomic compaction.
    b.maxAlive = 5; // lowering the limit must neither cull survivors nor admit new ones
    ASSERT_HRESULT_SUCCEEDED(sim.Step(1, std::span(&b, 1)));
    ASSERT_NO_FATAL_FAILURE(Await(2, s));
    EXPECT_EQ(s.alive, 13u);
    EXPECT_EQ(s.rejected, UINT32_MAX);
    ExpectRow(s, a.owner, 0, 0);
    ExpectRow(s, b.owner, 13, UINT32_MAX);
    ASSERT_NO_FATAL_FAILURE(Rendered(13, visible));
    std::vector<UINT> survived;
    for (auto& p : visible) {
        EXPECT_EQ(p.meta[0], 1u);
        EXPECT_EQ(p.meta[1], 7u);
        survived.push_back(p.meta[2]);
        EXPECT_FLOAT_EQ(p.positionAge[1], 8.f);
        EXPECT_FLOAT_EQ(p.positionAge[3], 1.f);
        EXPECT_FLOAT_EQ(p.velocityLifetime[3], 2.f);
        EXPECT_FLOAT_EQ(p.colorSize[1], .25f);
        EXPECT_FLOAT_EQ(p.colorSize[3], 2.f);
    }
    std::sort(born.begin(), born.end());
    std::sort(survived.begin(), survived.end());
    EXPECT_EQ(survived, born);
    ASSERT_HRESULT_SUCCEEDED(sim.Step(1, {}));
    ASSERT_NO_FATAL_FAILURE(Await(3, s));
    EXPECT_EQ(s.alive, 0u);
    EXPECT_EQ(s.rejected, 0u);
    ASSERT_NO_FATAL_FAILURE(Rendered(0, visible));
}

TEST_F(ParticleSimulationWarp, DelayedSamplesCannotReleaseAnOwnerAfterANewerBirth) {
    ASSERT_HRESULT_SUCCEEDED(Init(8, 1));
    Sim::Emitter old{.owner = 0x100000001ull, .maxAlive = 8};
    Sim::Emitter next{.owner = 0x200000001ull, .requested = 4, .maxAlive = 8, .appearance = 9};
    ASSERT_EQ(sim.Latest(), nullptr);
    ASSERT_HRESULT_SUCCEEDED(sim.Step(0, std::span(&old, 1)));
    Sim::Snapshot s;
    ASSERT_NO_FATAL_FAILURE(Await(1, s));
    ExpectRow(s, old.owner, 0, 0);

    // Queue three steps before consuming telemetry; do not depend on WARP taking
    // a particular amount of time. The last confirmed zero predates frame 2.
    old.requested = 6;
    ASSERT_HRESULT_SUCCEEDED(sim.Step(0, std::span(&old, 1)));
    ASSERT_HRESULT_SUCCEEDED(sim.Step(0, std::span(&next, 1)));
    ASSERT_HRESULT_SUCCEEDED(sim.Step(1, std::span(&next, 1)));
    ASSERT_NO_FATAL_FAILURE(Await(4, s));
    ASSERT_EQ(s.emitters.size(), 2u);
    ExpectRow(s, old.owner, 0, 0);
    ExpectRow(s, next.owner, 0, 4);
    EXPECT_EQ(s.alive, 0u);
    EXPECT_EQ(s.rejected, 4u);

    // The drain has now been observed for the correct owner. Reuse the same
    // physical slot, despite the owners sharing their low 32 bits.
    ASSERT_HRESULT_SUCCEEDED(sim.Step(0, std::span(&next, 1)));
    ASSERT_NO_FATAL_FAILURE(Await(5, s));
    ASSERT_EQ(s.emitters.size(), 1u);
    ExpectRow(s, next.owner, 4, 0);
    std::vector<Sim::Particle> visible;
    ASSERT_NO_FATAL_FAILURE(Rendered(4, visible));
    for (auto& p : visible) {
        EXPECT_EQ(p.meta[0], 0u);
        EXPECT_EQ(p.meta[1], 9u);
        EXPECT_FLOAT_EQ(p.positionAge[3], 0.f);
    }
}
} // namespace
