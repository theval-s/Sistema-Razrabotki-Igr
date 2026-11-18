#pragma once
#include "ParticleSimulation.hpp"

namespace engine {
class ParticleTiles {
public:
    // Caller registers engine/ParticleTiles.hlsl CSMain (cs_5_0) four times via EngineShader,
    // defining respectively PARTICLE_TILES_BUILD, _MERGE, _BIN, _SHADE (same prefix).
    struct Programs { ID3D11ComputeShader *build{}, *merge{}, *bin{}, *shade{}; };
    struct Camera {
        // Untransposed row-major, row-vector view and projection (SimpleMath storage).
        // Rigid RH view, camera looks down -Z. Axis-aligned perspective or orthographic,
        // reverse Z, [0,1] clip depth, full texture viewport, depth clear = 0.
        float view[16], projection[16];
    };
    struct Stats { std::uint64_t frame{}; std::uint32_t peakOccupancy{}, batchingTiles{}; };
    ParticleTiles();
    ~ParticleTiles();
    // FL11 immediate context from device. Capacity must match simulation. Reinitialize on resize;
    // failure preserves old resources. Fine tiles 8x8, batches of 64; square coarse regions start
    // at 64 pixels and double until their combined rank masks fit in 32 MiB.
    long Initialize(ID3D11Device*, Programs, std::uint32_t capacity, std::uint32_t width, std::uint32_t height);
    std::uint32_t CoarsePixels() const;
    // Fresh RenderViews(), R32_FLOAT depth SRV and RGBA16_FLOAT result SRV (also copy destination).
    // Both textures: mip 0, single mip/slice/sample, initialized dimensions. All resources and
    // programs on same device; release conflicting bindings (including OM RTV/DSV and other SRVs)
    // first. Clobbers CS shader/b0/t0..7/u0..2, leaves them null. Render thread only.
    long Render(ParticleSimulation::Views, const Camera&, ID3D11ShaderResourceView* depth,
                ID3D11ShaderResourceView* lit);
    // Last completed diagnostic copy; frame 0 until available. No wait/flush, drops samples
    // if all staging slots busy. Occupancy counts projected AABB intersections before depth tests.
    Stats Latest();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
