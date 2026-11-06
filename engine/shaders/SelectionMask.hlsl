#include "Common.hlsl"
#include "ShaderTypes.hlsl"

float4 PSMain(GBufferInput input) : SV_Target {
#ifdef OCCLUDED
    return float4(0.0f, 1.0f, 0.0f, 1.0f);
#else
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
#endif
}
