#include "Common.hlsl"

struct VSOutput {
    float4 position : SV_POSITION;
};

VSOutput VSMain(float3 position : POSITION) {
    VSOutput output;
    output.position = mul(mul(float4(position, 1.0f), View.viewMatrix), View.projectionMatrix);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET {
    return float4(1.0f, 0.8f, 0.25f, 1.0f);
}
