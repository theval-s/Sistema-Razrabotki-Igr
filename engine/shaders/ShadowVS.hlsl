#include "Common.hlsl"
#include "ShaderTypes.hlsl"

#ifdef MATERIAL_HAS_HOOK
void PatchShadowVertex(inout MaterialVertex vertex);
#else
void PatchShadowVertex(inout MaterialVertex vertex) {
}
#endif

ShadowVertexOutput VSMain(ShadowVertexInput input) {
    MaterialVertex vertex = {input.pos, input.normal, input.tex};
    PatchShadowVertex(vertex);
    float4 worldSpace = mul(vertex.position, Object.worldMatrix);
    ShadowVertexOutput output = (ShadowVertexOutput)0;
    output.pos = worldSpace;
    output.worldPos = worldSpace.xyz;
    return output;
}
