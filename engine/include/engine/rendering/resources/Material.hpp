#pragma once
#include <SimpleMath.h>
#include <cstdint>
#include <optional>
#include <string>

#include "Buffer.hpp"
#include "ShaderContext.hpp"
#include "Texture.hpp"

namespace engine {

using DirectX::SimpleMath::Vector3;

struct MaterialPBRParameters {
    Vector3 specularColor{0.25f, 0.25f, 0.25f};
    float shininess = 32.0f;
};

enum class MaterialHook : uint8_t {
    None = 0,
    GBufferVS = 1 << 0,
    GBufferPS = 1 << 1,
    ShadowVS = 1 << 2
};

constexpr MaterialHook operator|(MaterialHook a, MaterialHook b) {
    return static_cast<MaterialHook>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool HasHook(MaterialHook hooks, MaterialHook hook) {
    return (static_cast<uint8_t>(hooks) & static_cast<uint8_t>(hook)) != 0;
}

struct ShaderSet {
    ShaderHandle shadowPassVS;
    ShaderHandle gBufferPassVS;
    ShaderHandle gBufferPassPS;
};

struct MaterialParameters {
    TextureHandle albedoTexture;
    MaterialPBRParameters parameters;
    // Caller-owned constant buffer bound at b8. The caller uploads its contents.
    BufferHandle customBuffer;
};

// shaderFile uses executable-relative engine/... or game/... shader roots.
// Include engine/MaterialTemplate.hlsl first, then define the declared functions:
// MATERIAL_GBUFFER_VS: void PatchGBufferVertex(inout MaterialVertex vertex);
// MATERIAL_GBUFFER_PS: void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface);
// MATERIAL_SHADOW_VS:  void PatchShadowVertex(inout MaterialVertex vertex).
// Guard each definition with its stage define when sharing a file across stages.
struct MaterialDesc {
    std::wstring shaderFile;
    MaterialHook hooks = MaterialHook::None;
    MaterialParameters defaults;
};

struct Material {
    ShaderSet shaders;
    MaterialParameters defaultParameters;
    uint32_t instanceCount = 0;
};

using MaterialHandle = HandleMap<Material>::Handle;

struct MaterialInstance {
    MaterialHandle material;
    struct Overrides {
        std::optional<Vector3> specularColor;
        std::optional<float> shininess;
        // absent = inherit; present texture handle = override; present null handle = clear.
        std::optional<TextureHandle> albedoTexture;
    } overrides;
    // Caller-owned runtime state, independent of authored overrides.
    BufferHandle customBuffer;
};

// Resolve when binding a draw so inherited fields reflect the current material.
inline MaterialParameters ResolveMaterialParameters(const Material& material, const MaterialInstance& instance) {
    MaterialParameters result = material.defaultParameters;
    if (instance.overrides.specularColor) result.parameters.specularColor = *instance.overrides.specularColor;
    if (instance.overrides.shininess) result.parameters.shininess = *instance.overrides.shininess;
    if (instance.overrides.albedoTexture) result.albedoTexture = *instance.overrides.albedoTexture;
    if (instance.customBuffer) result.customBuffer = instance.customBuffer;
    return result;
}

using MaterialInstanceHandle = HandleMap<MaterialInstance>::Handle;

}
