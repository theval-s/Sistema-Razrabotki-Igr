#pragma once

#include <cstdint>

#include <engine/engine_export.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

namespace engine {

/// Every shader variant the engine's render passes compile. A cooked build precompiles exactly
/// these, so a pass must take its shaders from here rather than name a file itself.
enum class EngineShader : std::uint8_t {
    GBufferVS, SkinnedGBufferVS, GBufferPS,
    ShadowVS, SkinnedShadowVS, ShadowGS,
    LightVS, LightPS, DirectionalLightVS,
    PresentVS, PresentPS, OutlinedPresentPS,
    LinesVS, ScreenLinesVS, LinesGS, LinesPS,
    SelectionVisiblePS, SelectionOccludedPS,
    ParticleUpdate, ParticleAdmit, ParticleSpawn,
    ParticleBuild, ParticleMerge, ParticleBin, ParticleShade,
    ParticleTextureResample, ParticleShadowVS, ParticleShadowPS,
    Count
};

/// `file` is a logical engine/ shader name, resolved like RenderingResourceManager::CompileShader does.
[[nodiscard]] ENGINE_API const ShaderVariant& GetEngineShader(EngineShader shader);

} // namespace engine
