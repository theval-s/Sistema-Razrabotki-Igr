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
#ifdef SELECTION_OUTLINE
Texture2D<float2> SelectionMask : register(t16);
#endif
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
#ifdef SELECTION_OUTLINE
    uint width, height;
    SelectionMask.GetDimensions(width, height);
    int2 pixel = clamp(int2(input.pos.xy), int2(0, 0), int2(width - 1, height - 1));
    float2 center = SelectionMask.Load(int3(pixel, 0));
    if (center.r < 0.5 && center.g < 0.5) {
        float2 neighbors = 0;
        [unroll]
        for (int y = -2; y <= 2; ++y) {
            [unroll]
            for (int x = -2; x <= 2; ++x) {
                if (x * x + y * y > 4) continue;
                int2 samplePos = clamp(pixel + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
                neighbors = max(neighbors, SelectionMask.Load(int3(samplePos, 0)));
            }
        }
        if (neighbors.r >= 0.5) result = float3(1.0, 0.72, 0.12);
        else if (neighbors.g >= 0.5 && ((pixel.x + pixel.y) / 4) % 2 == 0)
            result = float3(0.22, 0.78, 1.0); // dashed outline for fully hidden selections
    }
#endif
    return float4(result, 1.0f);
}
