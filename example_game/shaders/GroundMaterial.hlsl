#include <engine/MaterialTemplate.hlsl>

#if defined(MATERIAL_GBUFFER_VS)
void PatchGBufferVertex(inout MaterialVertex vertex) {
    vertex.position.y += 0.05 * sin(vertex.position.x);
}
#endif

#if defined(MATERIAL_SHADOW_VS)
void PatchShadowVertex(inout MaterialVertex vertex) {
    vertex.position.y += 0.05 * sin(vertex.position.x);
}
#endif

#if defined(MATERIAL_GBUFFER_PS)
void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) {
    surface.albedo.rgb *= float3(0.9, 1.0, 0.9);
}
#endif

