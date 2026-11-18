#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <engine/rendering/Particles.hpp>

struct ID3D11Device;
struct ID3D11ComputeShader;
struct ID3D11ShaderResourceView;
struct ID3D11Buffer;

namespace engine {
class ParticleSimulation {
public:
    struct Particle {
        float positionAge[4], velocityLifetime[4], colorSize[4];
        std::uint32_t meta[4];
    };
    static_assert(sizeof(Particle) == 64);
    struct Emitter {
        std::uint64_t owner{}; // all values valid; unique within each Step
        std::uint32_t requested{}, maxAlive{}, appearance{}, seed{};
        float position[3]{}, lifetime{1}, direction[3]{0,1,0}, speed{1};
        float tint[3]{1,1,1}, radius{1}, spread{}; // additive unit-vector jitter before normalization
        ParticleEffectSettings effect{};
        std::uint32_t textureLayer{};
    };
    struct Programs { ID3D11ComputeShader *update{}, *admit{}, *spawn{}; };
    struct Views {
        ID3D11ShaderResourceView *particles{}, *activeIndices{}, *activeCount{};
        ID3D11Buffer* dispatchArgs{};
        // Safe CPU bound for sizing a sorting network; never an admission result.
        std::uint32_t upperAlive{UINT32_MAX};
        ID3D11ShaderResourceView* settings{}; // borrowed owner styles; null = identity defaults
        bool castsShadows{}; // some occupied slot's style casts shadows
    };
    struct EmitterStats { std::uint64_t owner; std::uint32_t alive, rejected; };
    struct Snapshot {
        std::uint64_t frame{}, rejected{}; // rejections for this Step, not cumulative
        std::uint32_t alive{};
        std::vector<EmitterStats> emitters;
    };
    ParticleSimulation();
    ~ParticleSimulation();
    // FL11+, immediate context, render thread only. Retains programs. Successful reinit resets all state.
    // Limits: capacity 1..64*65535, slots 1..4096. HRESULT failure leaves old state intact.
    long Initialize(ID3D11Device* device, Programs programs, std::uint32_t capacity, std::uint32_t slots);
    // One complete owner submission per step (empty is valid). Missing owners drain. Lowering maxAlive
    // gates new births only. GPU admissions in stable slot order. No free slot => all requests rejected.
    // Caller must unbind render views first. Clobbers CS, b0, t0..3, u0..4; leaves those slots null.
    long Step(float dt, std::span<const Emitter> emitters);
    // Borrowed; reacquire after Step. Structured strides 64,4,4 respectively; count at [0].
    // Indirect args at byte 0: ceil(count/64),1,1. Mask tail threads against activeCount[0].
    Views RenderViews() const;
    // Polls without flushing or waiting. Null until first completion. Ring-full frames skip telemetry.
    // Borrowed until next non-const operation. Compare frame for freshness (first frame = 1).
    const Snapshot* Latest();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace engine
