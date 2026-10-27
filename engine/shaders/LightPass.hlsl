#include "Common.hlsl"

struct LightBufferData {
    float3 color;
    float intensity;

    uint lightType;
    float spotInnerAngle;
    float spotOuterAngle;
    float range;

    float3 position;

    float3 direction;
};


cbuffer LightBuffer : register(b5) {
    LightBufferData Light;
};

Texture2D diffuseMap : register(t0);
Texture2D normalMap : register(t1);
Texture2D materialMap : register(t2);
Texture2D depthMap : register(t3);


Texture2DArray shadowMap : register(t4);
//Texture2DArray translucencyMap : register(t5);

struct VS_IN {
    float4 pos : POSITION0;
};

struct PS_IN {
    float4 pos : SV_POSITION;
};

PS_IN VSMain(VS_IN input) {
    float4 worldSpace = mul(input.pos, Object.worldMatrix);
    float4 cameraSpace = mul(worldSpace, View.viewMatrix);
    float4 clipSpace = mul(cameraSpace, View.projectionMatrix);

    PS_IN output = (PS_IN)0;
    output.pos = clipSpace;
    return output;
}

#ifndef PCF_RADIUS
#define PCF_RADIUS 1
#endif
#ifndef SHADOW_MAP_SIZE
#define SHADOW_MAP_SIZE 1024
#endif
#define PCF_TEXEL_SIZE (1.0f / SHADOW_MAP_SIZE)

float SampleShadowHybridPCF(float2 pixelCoords, int layer, float z, float2 depthPerTexel) {
    float sum = 0.0;
    int samples = 0;

    for (int y = -PCF_RADIUS; y <= PCF_RADIUS; y++) {
        for (int x = -PCF_RADIUS; x <= PCF_RADIUS; x++) {
            float2 offset = float2(x, y) * PCF_TEXEL_SIZE;
            float shadow = shadowMap.SampleCmpLevelZero(
                ShadowComparisonSampler,
                float3(pixelCoords + offset, layer),
                z + dot(float2(x, y), depthPerTexel)
            ).r;
            // float translucency =  1 - translucencyMap.SampleLevel(translucencySampler,
            //                                                  float3(pixelCoords + offset / 2, layer),
            //                                                  0.f).r;
            // sum += min(shadow, translucency);

            sum += shadow;
            samples++;
        }
    }

    return sum / samples;
}

float SampleCascadeShadow(float3 worldPos, float3 normal, float3 lightDir, float NdotL, int cascadeIndex) {
    float3 shadowWorldPos = worldPos;
    float4x4 lightMatrix = ShadowCascades.viewProjections[cascadeIndex];
    float4 lightSpacePos = mul(float4(shadowWorldPos, 1.0), lightMatrix);
    float3 ndcPos = lightSpacePos.xyz / lightSpacePos.w;

    // X[-1, 1] -> [0, 1], Y[-1, 1] -> [1, 0]
    float2 texCoords = float2(ndcPos.x * 0.5 + 0.5, -ndcPos.y * 0.5 + 0.5);


    float2 depthPerTexel = 0.0f;
    //Normal-derived approximation of receiver-plane depth correction
    if (NdotL > 0.0f) {
        float3 xAxis = float3(lightMatrix[0][0], lightMatrix[1][0], lightMatrix[2][0]);
        float3 yAxis = float3(lightMatrix[0][1], lightMatrix[1][1], lightMatrix[2][1]);
        float3 zAxis = float3(lightMatrix[0][2], lightMatrix[1][2], lightMatrix[2][2]);
        float3 dx = xAxis * (2.0f * PCF_TEXEL_SIZE / dot(xAxis, xAxis));
        float3 dy = -yAxis * (2.0f * PCF_TEXEL_SIZE / dot(yAxis, yAxis));
        float invNdotL = rcp(max(NdotL, 0.1f));
        depthPerTexel.x = dot(dx - lightDir * (dot(normal, dx) * invNdotL), zAxis);
        depthPerTexel.y = dot(dy - lightDir * (dot(normal, dy) * invNdotL), zAxis);
    }
    return SampleShadowHybridPCF(texCoords, cascadeIndex, ndcPos.z, depthPerTexel);
}

float CalculateShadow(float3 worldPos, float3 normal, float3 lightDir) {
    float NdotL = saturate(dot(normal, lightDir));
    float viewSpaceZ = mul(float4(worldPos, 1.0), View.viewMatrix).z;

    int cascadeIndex = 3;
    float depth = abs(viewSpaceZ);

    for (int i = 0; i < 4; i++) {
        if (depth < ShadowCascades.distances[i]) {
            cascadeIndex = i;
            break;
        }
    }

    float shadow = SampleCascadeShadow(worldPos, normal, lightDir, NdotL, cascadeIndex);

    if (cascadeIndex < 3 && depth >= ShadowCascades.transitionStarts[cascadeIndex]) {
        float start = ShadowCascades.transitionStarts[cascadeIndex];
        float end = ShadowCascades.distances[cascadeIndex];
        float t = saturate((depth - start) / (end - start));
        shadow = lerp(
            shadow,
            SampleCascadeShadow(worldPos, normal, lightDir, NdotL, cascadeIndex + 1),
            t
        );
    }

    return shadow;
}

float3 GetWorldPosFromDepth(float2 texCoord, float depth, float4x4 inv) {
    float2 ndcXY = float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
    float4 ndc = float4(ndcXY, depth, 1.0);
    float4 worldPos = mul(ndc, inv);
    return worldPos.xyz / worldPos.w;
}

float3 Normalize0(float3 v) {
    float lenSq = dot(v, v);
    return lenSq > 1e-8f ? v * rsqrt(lenSq) : float3(0.0f, 0.0f, 0.0f);
}

float SpecularTerm(float3 viewDir, float3 reflectDir, float shininess) {
    return pow(saturate(dot(viewDir, reflectDir)), max(shininess * 256.0f, 0.001f));
}

float4 PSMain(PS_IN input) : SV_Target {
    float2 uv = input.pos.xy / View.viewportSize;
    float3 diffuse = diffuseMap.Sample(LinearWrapSampler, uv).rgb;

    float4 normalTex = normalMap.Load(int3(int2(input.pos.xy), 0));
    float3 normal = Normalize0(normalTex.xyz * 2.0 - 1.0);

    float4 materialTex = materialMap.Sample(LinearWrapSampler, uv);
    float3 m_specularColor = materialTex.rgb;
    float shininess = materialTex.a;

    // Reconstruct from the depth of this exact G-buffer pixel; filtering depth
    // across silhouette edges creates positions that lie on neither surface.
    float depth = depthMap.Load(int3(int2(input.pos.xy), 0)).r;
    float3 worldPos = GetWorldPosFromDepth(uv, depth, View.invViewProj);

    if (Light.lightType == 0) {
        //directional
        float3 l = Normalize0(-Light.direction);
        float3 v = Normalize0(View.cameraPos - worldPos);
        float3 n = normal;
        float3 r = Normalize0(reflect(-l, n));

        float3 materialDiffuse = diffuse;
        float NdotL = saturate(dot(n, l));
        float3 diffuseColor = materialDiffuse * Light.color * NdotL;

        float3 specularColor = m_specularColor * Light.color * SpecularTerm(v, r, shininess) * (NdotL > 0.0f ? 1.0f : 0.0f);
        float3 ambientColor = diffuse * 0.1;
        float shadow = CalculateShadow(worldPos, n, l);
        float3 finalColor = (diffuseColor + specularColor) * shadow + ambientColor;
        return float4(finalColor * Light.intensity, 1);
    }
    if (Light.lightType == 1) {
        // point

        float3 toLight = Light.position - worldPos;
        float distance = sqrt(max(dot(toLight, toLight), 1e-8f));
        if (distance >= Light.range) {
            return float4(0, 0, 0, 1);
        }
        float3 l = toLight / distance;

        float3 v = Normalize0(View.cameraPos - worldPos);
        float3 n = normal;
        float3 r = Normalize0(reflect(-l, n));

        float attenuation = max(0, 1.0 - distance / Light.range);
        attenuation *= attenuation;

        float3 lightColor = Light.color * attenuation * Light.intensity;

        float3 materialDiffuse = diffuse;
        float3 diffuseColor = materialDiffuse * lightColor * max(dot(l, n), 0);
        float3 specularColor = m_specularColor * lightColor * SpecularTerm(v, r, shininess);
        float3 finalColor = diffuseColor + specularColor;
        return float4(finalColor, 1);
    }
    if (Light.lightType == 2) {
        //spot light
        float3 toLight = Light.position - worldPos;
        float distance = sqrt(max(dot(toLight, toLight), 1e-8f));
        if (distance >= Light.range) {
            return float4(0, 0, 0, 1);
        }
        float3 l = toLight / distance;

        float3 d = Normalize0(Light.direction);
        float3 v = Normalize0(View.cameraPos - worldPos);
        float3 n = normal;
        float3 r = Normalize0(reflect(-l, n));

        float theta = dot(-l, d);
        float localIntensity = clamp((theta - Light.spotOuterAngle) / (Light.spotInnerAngle - Light.spotOuterAngle), 0, 1);

        float attenuation = max(0, 1.0 - distance / Light.range);
        attenuation *= attenuation;

        float3 lightColor = Light.color * attenuation * localIntensity * Light.intensity;

        float3 materialDiffuse = diffuse;
        float3 diffuseColor = materialDiffuse * lightColor * max(dot(l, n), 0);
        float3 specularColor = m_specularColor * lightColor * SpecularTerm(v, r, shininess);
        float3 finalColor = diffuseColor + specularColor;
        return float4(finalColor, 1);
    }
    return float4(0, 0, 0, 1);
}
