Texture2D InputColor : register(t16);
SamplerState PointClampSampler : register(s0);

float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return InputColor.SampleLevel(PointClampSampler, uv, 0);
}
