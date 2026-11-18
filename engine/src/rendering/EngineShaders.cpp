#include <engine/rendering/EngineShaders.hpp>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/RenderingResources.hpp>

#include <array>
#include <string>

namespace engine {

const ShaderVariant& GetEngineShader(const EngineShader shader) {
    using enum ShaderType;
    static const std::array<ShaderVariant, static_cast<std::size_t>(EngineShader::Count)> variants{{
        {L"engine/MaterialTemplate.hlsl", Vertex, {.defines = {{"MATERIAL_GBUFFER_VS"}}}},
        {L"engine/MaterialTemplate.hlsl", Vertex, {.defines = {{"MATERIAL_GBUFFER_VS"}, {"ENGINE_SKINNED", "1"}}}},
        {L"engine/MaterialTemplate.hlsl", Pixel, {.defines = {{"MATERIAL_GBUFFER_PS"}}}},
        {L"engine/MaterialTemplate.hlsl", Vertex, {.defines = {{"MATERIAL_SHADOW_VS"}}}},
        {L"engine/MaterialTemplate.hlsl", Vertex, {.defines = {{"MATERIAL_SHADOW_VS"}, {"ENGINE_SKINNED", "1"}}}},
        {L"engine/ShadowCSMPass.hlsl", Geometry, {}},
        {L"engine/LightPass.hlsl", Vertex, {}},
        {L"engine/LightPass.hlsl", Pixel,
            {.defines = {{"PCF_RADIUS", "1"}, {"SHADOW_MAP_SIZE", std::to_string(ShadowMapSize)}}}},
        {L"engine/DirectionalLightVS.hlsl", Vertex, {}},
        {L"engine/PresentPass.hlsl", Vertex, {}},
        {L"engine/PresentPass.hlsl", Pixel, {}},
        {L"engine/PresentPass.hlsl", Pixel, {.defines = {{"SELECTION_OUTLINE"}}}},
        {L"engine/DebugDraw.hlsl", Vertex, {}},
        {L"engine/DebugDraw.hlsl", Vertex, {.defines = {{"SCREEN_SPACE"}}}},
        {L"engine/DebugDraw.hlsl", Geometry, {}},
        {L"engine/DebugDraw.hlsl", Pixel, {}},
        {L"engine/SelectionMask.hlsl", Pixel, {}},
        {L"engine/SelectionMask.hlsl", Pixel, {.defines = {{"OCCLUDED"}}}},
        {L"engine/ParticleSim.hlsl", Compute, {.defines = {{"PARTICLE_SIM_UPDATE"}}}},
        {L"engine/ParticleSim.hlsl", Compute, {.defines = {{"PARTICLE_SIM_ADMIT"}}}},
        {L"engine/ParticleSim.hlsl", Compute, {.defines = {{"PARTICLE_SIM_SPAWN"}}}},
        {L"engine/ParticleTiles.hlsl", Compute, {.defines = {{"PARTICLE_TILES_BUILD"}}}},
        {L"engine/ParticleTiles.hlsl", Compute, {.defines = {{"PARTICLE_TILES_MERGE"}}}},
        {L"engine/ParticleTiles.hlsl", Compute, {.defines = {{"PARTICLE_TILES_BIN"}}}},
        {L"engine/ParticleTiles.hlsl", Compute, {.defines = {{"PARTICLE_TILES_SHADE"}}}},
        {L"engine/ParticleTextures.hlsl", Compute, {}},
        {L"engine/ParticleShadows.hlsl", Vertex, {}},
        {L"engine/ParticleShadows.hlsl", Pixel, {}},
        {L"engine/SkyboxPass.hlsl", Vertex, {}},
        {L"engine/SkyboxPass.hlsl", Pixel, {}},
    }};
    return variants.at(static_cast<std::size_t>(shader));
}

std::filesystem::path GetProjectParticleHooksPath() {
    return AssetPathResolver::GetGameRoot() / "shaders" / "ParticleHooks.hlsli";
}

} // namespace engine
