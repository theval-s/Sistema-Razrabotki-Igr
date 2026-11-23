#ifndef SHADER_TYPES_INCLUDE_H
#define SHADER_TYPES_INCLUDE_H

// Raw material shaders must use this four-target contract. Ordinary material
// hooks inherit it from MaterialTemplate.hlsl when recompiled.
#define ENGINE_MATERIAL_ABI_VERSION 2

struct MaterialVertex {
    float4 position;
    float3 normal;
    float2 uv;
};

struct GBufferVertexInput {
    float4 pos : POSITION0;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
#ifdef ENGINE_SKINNED
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
#endif
};

struct ShadowVertexInput {
    float4 pos : POSITION0;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
#ifdef ENGINE_SKINNED
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
#endif
};

struct ShadowVertexOutput {
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD1;
};

struct GBufferInput {
    float4 pos : SV_POSITION;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

struct MaterialSurface {
    float4 albedo;
    float3 normal; // world-space, normalized when written to the G-buffer
    float3 specularColor;
    float shininess;
    float3 emission; // linear HDR RGB, independent of lighting
    float lightingWeight; // 0 = unlit albedo, 1 = engine lighting
};

struct GBufferOutput {
    float4 diffuse : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 material : SV_TARGET2;
    float4 sceneColor : SV_TARGET3;
};

GBufferOutput EncodeMaterialSurface(MaterialSurface surface) {
    float weight = saturate(surface.lightingWeight);
    float lengthSquared = dot(surface.normal, surface.normal);
    float3 normal = lengthSquared > 1e-8
        ? surface.normal * rsqrt(lengthSquared) : float3(0, 0, 0);
    GBufferOutput output;
    output.diffuse = surface.albedo;
    output.normal = float4(normal * 0.5 + 0.5, weight);
    output.material = float4(surface.specularColor, surface.shininess / 256.0);
    // Every surviving geometry fragment overwrites this, including lit surfaces
    // with zero emission: occluded geometry must not leave its emission behind.
    output.sceneColor = float4(surface.emission + (1 - weight) * surface.albedo.rgb, 1);
    return output;
}
#endif
