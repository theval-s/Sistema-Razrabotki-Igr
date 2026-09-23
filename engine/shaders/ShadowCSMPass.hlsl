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

cbuffer EngineObject : register(b3) {
    EngineObjectData Object;
}


struct ShadowCascadesData {
    float4x4 viewProjections[4];
    float4 distances;
};

cbuffer EngineCascades : register(b4) {
    ShadowCascadesData ShadowCascades;
}

SamplerState PointClampSampler : register(s0);
SamplerState LinearClampSampler : register(s1);
SamplerState LinearWrapSampler : register(s2);
SamplerState AnisotropicWrapSampler : register(s3);
SamplerComparisonState ShadowComparisonSampler : register(s4);
//#include "Common.hlsl"



struct VS_IN {
    float4 pos : POSITION0;
};

struct GS_IN {
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD1;
};

struct GS_OUT {
    float4 pos : SV_POSITION;
    uint arrInd : SV_RenderTargetArrayIndex;
};

GS_IN VSMain(VS_IN input) {
    float4 worldSpace = mul(input.pos, Object.worldMatrix);

    GS_IN output = (GS_IN)0;
    output.pos = worldSpace;
    output.worldPos = output.pos;
    return output;
}

[instance(4)]
[maxvertexcount(3)]
void GSMain(triangle GS_IN p[3], in uint id : SV_GSInstanceID, inout TriangleStream<GS_OUT> stream) {
    for (int i = 0; i < 3; i++) {
        GS_OUT gs = (GS_OUT)0;
        gs.pos = mul(float4(p[i].worldPos.xyz, 1.0f), ShadowCascades.viewProjections[id]);
        gs.arrInd = id;
        stream.Append(gs);
    }
}
