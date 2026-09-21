#pragma pack_matrix(row_major)

struct PS_IN {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

PS_IN VSMain(uint vertexId : SV_VertexID) {
    static const float2 positions[6] = {
        float2(1.0f, 1.0f),
        float2(-1.0f, 1.0f),
        float2(-1.0f, -1.0f),
        float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f),
        float2(1.0f, -1.0f)
    };

    static const float2 uvs[6] = {
        float2(1.0f, 0.0f),
        float2(0.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 1.0f)
    };

    PS_IN output = (PS_IN)0;
    output.pos = float4(positions[vertexId], 0.0f, 1.0f);
    output.tex = uvs[vertexId];
    return output;
}

Texture2D SceneResult : register(t5);
// Texture2D particleTex : register(t1);

SamplerState PointClampSampler : register(s0);

float3 ReinhardTonemap(float3 color)
{
    return color / (1.0f + color);
}

float4 PSMain(PS_IN input) : SV_Target
{
    float4 sceneColor = SceneResult.Sample(PointClampSampler, input.tex);
    //float4 particleColor = particleTex.Sample(pointSampler, input.tex);
    
    float3 result = sceneColor.rgb;
    //float3 result = particleColor.rgb * particleColor.a + sceneColor.rgb * (1.0 - particleColor.a);
    result = ReinhardTonemap(result);
    return float4(result, 1.0f);
}
