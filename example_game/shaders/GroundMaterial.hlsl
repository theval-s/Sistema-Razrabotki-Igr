#include <engine/MaterialTemplate.hlsl>

// The ground is one big plane: repeat the checker texture across it.
static const float TextureRepetitions = 12.0;

void PatchGBufferVertex(inout MaterialVertex vertex) {
    vertex.position.y += 0.05 * sin(vertex.position.x);
    vertex.uv *= TextureRepetitions;
}

void PatchShadowVertex(inout MaterialVertex vertex) {
    vertex.position.y += 0.05 * sin(vertex.position.x);
}

void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) {
    surface.albedo.rgb *= float3(0.9, 1.0, 0.9);
}

