#ifndef SHADER_TYPES_INCLUDE_H
#define SHADER_TYPES_INCLUDE_H

struct MaterialVertex {
    float4 position;
    float3 normal;
    float2 uv;
};

struct GBufferVertexInput {
    float4 pos : POSITION0;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
};

struct ShadowVertexInput {
    float4 pos : POSITION0;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
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
};

struct GBufferOutput {
    float4 diffuse : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 material : SV_TARGET2;
};
#endif
