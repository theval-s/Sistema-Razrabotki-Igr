#pragma pack_matrix(row_major)

#pragma pack_matrix(row_major)

struct EngineFrameData {
    float4 unusedForNow; //todo
};

cbuffer EngineFrame : register(b0) {
    EngineFrameData Frame;
};

struct EngineViewData {
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
    float4x4 invViewProj;
    float3 cameraPos;
    float2 viewportSize;
};

cbuffer EngineView : register(b1) {
    EngineViewData View;
}

struct EngineMaterialData {
    float3 specularColor;
    float shininess;
};

cbuffer EngineMaterial : register(b2) {
    EngineMaterialData Material;
}

struct EngineObjectData {
    float4x4 worldMatrix;  
    float4x4 normalMatrix; //todo: may be split to separate object as only GBuffer and CS passes currently need this
};

cbuffer EngineObject : register(b2) {
    EngineObjectData Object;
}


struct ShadowCascades {
    float4x4 viewProjections[4];
    float4 distances;
};

cbuffer EngineCascades : register(b4) {
    ShadowCascades ShadowCascades;
}

SamplerState PointClampSampler : register(s0);
SamplerState LinearClampSampler : register(s1);
SamplerState LinearWrapSampler : register(s2);
SamplerState AnisotropicWrapSampler : register(s3);
SamplerComparisonState ShadowComparisonSampler : register(s4);
//#include "Common.hlsl"

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

#define PCF_RADIUS 1
#define PCF_TEXEL_SIZE 1.f / 1024 //todo: parametrize

float SampleShadowHybridPCF(float2 pixelCoords, int layer, float z) {
    float sum = 0.0;
    int samples = 0;

    for (int y = -PCF_RADIUS; y <= PCF_RADIUS; y++) {
        for (int x = -PCF_RADIUS; x <= PCF_RADIUS; x++) {
            float2 offset = float2(x, y) * (PCF_TEXEL_SIZE) * 2.0;
            float shadow = shadowMap.SampleCmpLevelZero(
                ShadowComparisonSampler,
                float3(pixelCoords + offset, layer),
                z
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

float CalculateShadow(float3 worldPos) {
    float viewSpaceZ = mul(float4(worldPos, 1.0), View.viewMatrix).z;


    int cascadeIndex = 3;
    float depth = abs(viewSpaceZ);
    for (int i = 0; i < 4; i++) {
        if (depth < ShadowCascades.distances[i]) {
            cascadeIndex = i;
            break;
        }
    }

    float4 lightSpacePos = mul(float4(worldPos, 1.0), ShadowCascades.viewProjections[cascadeIndex]);
    float3 ndcPos = lightSpacePos.xyz / lightSpacePos.w;

    //X[-1, 1] -> [0, 1], Y[-1, 1] -> [1, 0]
    float2 texCoords = float2(ndcPos.x * 0.5 + 0.5, -ndcPos.y * 0.5 + 0.5);
    return SampleShadowHybridPCF(texCoords, cascadeIndex, ndcPos.z);

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

    float4 normalTex = normalMap.Sample(LinearWrapSampler, uv);
    float3 normal = Normalize0(normalTex.xyz * 2.0 - 1.0);

    float4 materialTex = materialMap.Sample(LinearWrapSampler, uv);
    float3 m_specularColor = materialTex.rgb;
    float shininess = materialTex.a;

    float depth = depthMap.Sample(LinearWrapSampler, uv).r;
    float3 worldPos = GetWorldPosFromDepth(uv, depth, View.invViewProj);

    if (Light.lightType == 0) {
        //directional
        float3 l = Normalize0(-Light.direction);
        float3 v = Normalize0(View.cameraPos - worldPos);
        float3 n = normal;
        float3 r = Normalize0(reflect(-l, n));

        float3 materialDiffuse = diffuse;
        float3 diffuseColor = materialDiffuse * Light.color * max(dot(l, n), 0);
        float3 specularColor = m_specularColor * Light.color * SpecularTerm(v, r, shininess);
        float3 ambientColor = diffuse * 0.1;
        float shadow = CalculateShadow(worldPos);
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
