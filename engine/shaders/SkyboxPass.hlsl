#include "SkyboxConstants.hlsli"

TextureCube<float4> skyboxTexture : register(t0);
SamplerState skyboxSampler : register(s0);

struct SkyboxVertex {
    float4 position : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

SkyboxVertex VSMain(uint vertexID : SV_VertexID) {
    SkyboxVertex output;
    float2 xy = float2((vertexID << 1) & 2, vertexID & 2) * 2 - 1;
    output.position = float4(xy, 0, 1);
    output.clipXY = xy;
    return output;
}

float4 PSMain(SkyboxVertex input) : SV_TARGET {
    if (skyboxUseCubemap == 0)
        return float4(skyboxFallbackColor, 1);

    // Reverse-Z near plane is finite even with an infinite far projection.
    float4 viewPoint = mul(float4(input.clipXY, 1, 1), skyboxInverseProjection);
    float3 viewRay = viewPoint.xyz / viewPoint.w;
    float3 direction = mul(float4(viewRay, 0), skyboxViewToCubemap).xyz;
    return float4(skyboxTexture.Sample(skyboxSampler, direction).rgb * skyboxTint * skyboxBrightness, 1);
}
