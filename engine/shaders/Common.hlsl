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

