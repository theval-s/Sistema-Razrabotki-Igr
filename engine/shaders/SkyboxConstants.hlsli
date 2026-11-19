#ifndef SRI_SKYBOX_CONSTANTS_HLSLI
#define SRI_SKYBOX_CONSTANTS_HLSLI

// engine/src/rendering/SkyboxPass.hpp: SkyboxConstants (160 bytes).
cbuffer SkyboxConstants : register(b0)
{
    row_major float4x4 skyboxInverseProjection;
    row_major float4x4 skyboxViewToCubemap;
    float3 skyboxTint;
    float skyboxBrightness;
    float3 skyboxFallbackColor;
    uint skyboxUseCubemap;
};

#endif
