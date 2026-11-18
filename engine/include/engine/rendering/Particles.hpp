#pragma once

#include <SimpleMath.h>
#include <engine/assets/AssetCatalog.hpp>
#include <cstdint>
#include <vector>

namespace engine {
enum class ParticleBlendMode : std::uint32_t { Alpha, Additive };

/// Shared authoring defaults. Colors are straight linear HDR; distances are world units.
struct ParticleEffectSettings {
    std::uint32_t appearance{0}, maxAlive{256}; // 0 = standard textured particle, 1 = reserved, >= 2 = project hooks
    float rate{30}, lifetime{2}, speed{1}, radius{0.25f}, spread{0.35f};
    float lifetimeVariation{}, speedVariation{}; // symmetric fractions: lifetime [0,1), speed [0,1]
    DirectX::SimpleMath::Vector3 acceleration{};
    float drag{};
    float sizeStart{0.65f}, sizeEnd{1.8f};
    DirectX::SimpleMath::Vector3 colorStart{1, 1, 1}, colorEnd{1, 1, 1};
    float opacityStart{0.72f}, opacityEnd{}, fadeIn{0.1f}; // fadeIn is a fraction of lifetime
    float softDistance{0.25f}, shadowDensity{1};
    ParticleBlendMode blend{ParticleBlendMode::Alpha};
    bool castShadows{true}, randomRotation{true};
    bool operator==(const ParticleEffectSettings&) const = default;
};

/// Emits in world space along the entity's local up axis. Rates are births per simulation second.
struct ParticleEmitter {
    std::uint32_t appearance{0}, maxAlive{256}, seed{1}; // inline settings; appearance as in ParticleEffectSettings
    float rate{30}, lifetime{2}, speed{1}, radius{0.25f}, spread{0.35f};
    DirectX::SimpleMath::Vector3 tint{1, 1, 1}; // straight linear HDR
    AssetReference effect;
    // With an effect assigned, its emission settings apply unless explicitly overridden.
    // Seed and tint always remain per-instance. Without an effect, the inline settings apply.
    bool overrideEmission{false};
    bool operator==(const ParticleEmitter&) const = default;
};

/// Transient editor transport. Never serialized into scenes or effects.
struct ParticlePreview {
    bool enabled{}, paused{}, allEmitters{};
    std::uint64_t owner{};
    float speed{1};
    bool operator==(const ParticlePreview&) const = default;
};

inline constexpr std::uint32_t ParticleTextureSize = 256;
inline constexpr std::uint32_t ParticleTextureLayers = 64; // includes the built-in soft-disc layer
inline constexpr std::uint32_t ParticleShadowMapSize = 512;

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
