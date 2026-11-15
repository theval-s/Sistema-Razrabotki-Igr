#include "Common.hlsl"
#include "ShaderTypes.hlsl"
#include "Skinning.hlsl"

#ifdef MATERIAL_HAS_HOOK
void PatchGBufferVertex(inout MaterialVertex vertex);
#else
void PatchGBufferVertex(inout MaterialVertex vertex) {}
#endif

GBufferInput VSMain(GBufferVertexInput input) {
    MaterialVertex vertex = {input.pos, input.normal, input.tex};
#ifdef ENGINE_SKINNED
    SkinVertex(vertex, input.joints, input.weights);
#endif
    PatchGBufferVertex(vertex);
    float4 worldSpace = mul(vertex.position, Object.worldMatrix);
    GBufferInput output = (GBufferInput)0;
    output.pos = mul(mul(worldSpace, View.viewMatrix), View.projectionMatrix);
    output.normal = SkinningSafeNormal(mul(vertex.normal, (float3x3)Object.normalMatrix), float3(0, 1, 0));
    output.tex = vertex.uv;
    output.worldPos = worldSpace.xyz;
    return output;
}
