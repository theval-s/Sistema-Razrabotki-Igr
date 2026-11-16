#include "Common.hlsl"
#include "ShaderTypes.hlsl"
#include "Skinning.hlsl"

#ifdef MATERIAL_HAS_HOOK
void PatchShadowVertex(inout MaterialVertex vertex);
#else
void PatchShadowVertex(inout MaterialVertex vertex) {}
#endif

ShadowVertexOutput VSMain(ShadowVertexInput input) {
    MaterialVertex vertex = {input.pos, input.normal, input.tex};
#ifdef ENGINE_SKINNED
    SkinVertex(vertex, input.joints, input.weights);
#endif
    PatchShadowVertex(vertex);
    float4 worldSpace = mul(vertex.position, Object.worldMatrix);
    ShadowVertexOutput output = (ShadowVertexOutput)0;
    output.pos = worldSpace;
    output.worldPos = worldSpace.xyz;
    return output;
}
