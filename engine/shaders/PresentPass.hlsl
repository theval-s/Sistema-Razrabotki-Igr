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
// One register, matching PresentPass::PresentBufferData. Mode values follow
// RenderDebugView: Final=0, Albedo=1, Normals=2, Depth=3.
cbuffer PresentParameters : register(b5) {
    uint DebugView;
    float NearPlane;
    float FarPlane;
    uint BuiltinTonemappingEnabled;
};
#ifdef SELECTION_OUTLINE
Texture2D<float2> SelectionMask : register(t16);
#endif

SamplerState PointClampSampler : register(s0);

float3 ReinhardTonemap(float3 color)
{
    return color / (1.0f + color);
}

float4 PSMain(PS_IN input) : SV_Target
{
    float4 sceneColor = SceneResult.Sample(PointClampSampler, input.tex);
    float3 result = sceneColor.rgb;
    if (DebugView == 3) {
        // FPSCamera uses RH perspective with far/near swapped. For positive
        // camera distance z, raw reverse-Z d = (n*f/z - n)/(f-n).
        // Cleared depth (0) is background, including geometry at the far plane.
        float d = sceneColor.r;
        result = 0;
        if (isfinite(d) && d > 0 && d <= 1 && isfinite(NearPlane) &&
            isfinite(FarPlane) && NearPlane > 0 && FarPlane > NearPlane) {
            // Algebraically n*f/(n+d*(f-n)), without overflowing n*f.
            float z = NearPlane / (NearPlane / FarPlane + d * (1 - NearPlane / FarPlane));
            float linearDepth = 1 - saturate((z - NearPlane) / (FarPlane - NearPlane));
            result = isfinite(linearDepth) ? linearDepth.xxx : float3(0, 0, 0);
        }
    } else if (DebugView == 0 && BuiltinTonemappingEnabled != 0) {
        result = ReinhardTonemap(result);
    }
    // Albedo is stored color; normals already contain worldNormal * .5 + .5.
#ifdef SELECTION_OUTLINE
    if (DebugView == 0) {
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
    }
#endif
    return float4(result, 1.0f);
}
