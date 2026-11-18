#pragma once

#include <SimpleMath.h>
#include <cstdint>
#include <vector>

namespace engine {
/// Emits in world space along the entity's local up axis. Rates are births per simulation second.
struct ParticleEmitter {
    std::uint32_t appearance{0}, maxAlive{256}, seed{1}; // 0 = smoke, 1 = fire, >= 2 = project hooks
    float rate{30}, lifetime{2}, speed{1}, radius{0.25f}, spread{0.35f};
    DirectX::SimpleMath::Vector3 tint{1, 1, 1}; // straight linear HDR
    bool operator==(const ParticleEmitter&) const = default;
};

/// Fixed pool budgets per renderer. Changes take effect when starting a new particle system.
struct ParticleBudget {
    std::uint32_t maxParticles{16384};
    std::uint32_t maxEmitters{256}; // includes removed emitters until their particles drain
    bool operator==(const ParticleBudget&) const = default;
};

struct ParticleEmitterStats {
    std::uint64_t owner{}; // scene emitter: integral EnTT handle, including its version
    std::uint32_t alive{}, rejected{};
};

/// Latest independently completed GPU samples. Frame numbers start at 1; zero = no sample yet.
/// Rejected is per simulation step, not cumulative. All counts are observational, never quotas.
struct ParticleStats {
    std::uint64_t simulationStep{}, rejected{}, tileFrame{};
    std::uint32_t alive{}, peakTileOccupancy{}, multiBatchTiles{};
    std::vector<ParticleEmitterStats> emitters;
};
} // namespace engine
