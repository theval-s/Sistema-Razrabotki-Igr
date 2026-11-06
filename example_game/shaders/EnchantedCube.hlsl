#include <engine/MaterialTemplate.hlsl>

void TwistCube(inout MaterialVertex vertex) {
    float3 p = vertex.position.xyz;
    // Top and bottom rotate in opposite directions; every duplicate corner
    // gets the same displacement, so the six faces stay joined.
    float angle = p.y * (0.9 + 0.25 * sin(Frame.simulationTimeSeconds * 1.7));
    float s, c;
    sincos(angle, s, c);
    float2 twisted = float2(c * p.x + s * p.z, -s * p.x + c * p.z);
    float pulse = 1.0 + 0.22 * sin(Frame.simulationTimeSeconds * 2.8 + dot(p, float3(2.0, 1.4, 1.7)));
    vertex.position.xyz = float3(twisted.x, p.y, twisted.y) * pulse;

    float3 n = vertex.normal;
    vertex.normal = float3(c * n.x + s * n.z, n.y, -s * n.x + c * n.z);
}


void PatchGBufferVertex(inout MaterialVertex vertex) {
    TwistCube(vertex);
}


void PatchShadowVertex(inout MaterialVertex vertex) {
    TwistCube(vertex);
}


void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) {
    float2 uv = input.tex;
    float drift = Frame.simulationTimeSeconds * 0.45;

    // Two crossing, scrolling diagonal glint bands, with small sparkling stars.
    float diagonalA = abs(frac(uv.x * 3.0 + uv.y * 2.0 - drift) - 0.5);
    float diagonalB = abs(frac(uv.x * 2.0 - uv.y * 3.0 + drift * 0.8) - 0.5);
    float glint = (1.0 - smoothstep(0.035, 0.11, diagonalA)) * 0.65
                + (1.0 - smoothstep(0.02, 0.07, diagonalB)) * 0.45;
    float2 cells = uv * 9.0 + float2(drift, -drift * 0.7);
    float2 center = frac(cells) - 0.5;
    float2 cell = floor(cells);
    float sparkle = step(0.86, frac(sin(dot(cell, float2(127.1, 311.7))) * 43758.5453))
                  * (1.0 - smoothstep(0.03, 0.18, length(center)))
                   * (0.65 + 0.35 * sin(Frame.simulationTimeSeconds * 5.0 + cell.x));

    float shimmer = 0.5 + 0.5 * sin((uv.x + uv.y) * 10.0 - Frame.simulationTimeSeconds * 2.0);
    float3 violet = lerp(float3(0.13, 0.025, 0.38), float3(0.40, 0.09, 0.70), shimmer);
    surface.albedo = float4(saturate(violet + (glint + sparkle) * float3(0.45, 0.55, 0.85)), 1.0);
    surface.specularColor = float3(0.75, 0.48, 0.95);
    surface.shininess = 100.0;
}

