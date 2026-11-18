#pragma once

#include <engine/engine_export.hpp>
#include <engine/rendering/Particles.hpp>
#include <string>
#include <string_view>

namespace engine {
/// Versioned .particle-effect.json asset. Empty texture selects the engine's soft disc.
struct ParticleEffectDefinition {
    int version{1};
    AssetReference texture;
    ParticleEffectSettings settings;
    bool operator==(const ParticleEffectDefinition&) const = default;
};

/// Transactional codecs: outputs are unchanged on error. Unknown fields are rejected.
[[nodiscard]] ENGINE_API bool ParseParticleEffectDefinition(std::string_view json,
    ParticleEffectDefinition& value, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeParticleEffectDefinition(const ParticleEffectDefinition& value,
    std::string& json, std::string& error);
[[nodiscard]] ENGINE_API bool ValidateParticleEffectSettings(const ParticleEffectSettings& value,
    std::string& error);
} // namespace engine
