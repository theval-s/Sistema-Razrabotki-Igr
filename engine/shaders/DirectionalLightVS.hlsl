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
    output.pos = float4(positions[vertexId], 1.0f, 1.0f);
    output.tex = uvs[vertexId];
    return output;
}