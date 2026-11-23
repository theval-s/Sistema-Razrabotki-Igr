#include "Common.hlsl"
#include "ShaderTypes.hlsl"

Texture2D albedoTexture : register(t0);
Texture2D emissionTexture : register(t1);

struct VS_IN {
    float4 pos : POSITION0;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
};

struct PS_IN {
    float4 pos : SV_POSITION;
    float3 normal : NORMAL0;
    float2 tex : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

PS_IN VSMain(VS_IN input) {
    float4 worldSpace = mul(input.pos, Object.worldMatrix);
    float3 newNormal = mul(input.normal, Object.normalMatrix);
    
    float4 cameraSpace = mul(worldSpace, View.viewMatrix);
    float4 clipSpace = mul(cameraSpace, View.projectionMatrix);

    PS_IN output = (PS_IN)0;
    output.pos = clipSpace;
    output.normal = normalize(newNormal);
    output.tex = input.tex;
    output.worldPos = worldSpace;
    return output;
}


GBufferOutput PSMain(PS_IN input) {
    MaterialSurface surface;
    surface.albedo = albedoTexture.Sample(LinearWrapSampler, input.tex);
    surface.normal = input.normal;
    surface.specularColor = Material.specularColor;
    surface.shininess = Material.shininess;
    surface.emission = Material.emission * emissionTexture.Sample(LinearWrapSampler, input.tex).rgb;
    surface.lightingWeight = Material.lightingWeight;
    return EncodeMaterialSurface(surface);
}
