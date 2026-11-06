#include "Common.hlsl"

// DebugDraw's lines, as quads LineWidth pixels wide: a hardware line is always one pixel thin.
// SCREEN_SPACE: positions are pixels from the top-left corner instead of world space.

static const float LineWidth = 2.0f;

struct VSInput {
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
#ifdef SCREEN_SPACE
    const float2 ndc = input.position.xy / View.viewportSize * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    output.position = float4(ndc, 0.5f, 1.0f);
#else
    output.position = mul(mul(float4(input.position, 1.0f), View.viewMatrix), View.projectionMatrix);
#endif
    output.color = input.color;
    return output;
}

VSOutput Corner(float4 position, float2 offset, float4 color) {
    VSOutput output;
    // The offset is in NDC, so it's scaled by w to stay the same on screen.
    output.position = float4(position.xy + offset * position.w, position.zw);
    output.color = color;
    return output;
}

[maxvertexcount(4)]
void GSMain(line VSOutput input[2], inout TriangleStream<VSOutput> output) {
    float4 a = input[0].position;
    float4 b = input[1].position;
    // Reverse-Z: the near plane is z == w, and w - z grows with the distance past it. Cut off
    // what is in front of it, where w goes to zero and then flips the line.
    const float nearA = a.w - a.z;
    const float nearB = b.w - b.z;
    if (nearA < 0.0f && nearB < 0.0f) return;
    if (nearA < 0.0f) a = lerp(a, b, nearA / (nearA - nearB));
    if (nearB < 0.0f) b = lerp(b, a, nearB / (nearB - nearA));

    const float2 halfViewport = View.viewportSize * 0.5f;
    float2 direction = (b.xy / b.w - a.xy / a.w) * halfViewport;  // in pixels
    direction = dot(direction, direction) > 1e-8f ? normalize(direction) : float2(1.0f, 0.0f);
    // Half a width out to each side, and past each end so that joined lines don't show a gap.
    const float2 side = float2(-direction.y, direction.x) * (LineWidth * 0.5f) / halfViewport;
    const float2 along = direction * (LineWidth * 0.5f) / halfViewport;

    output.Append(Corner(a, side - along, input[0].color));
    output.Append(Corner(a, -side - along, input[0].color));
    output.Append(Corner(b, side + along, input[1].color));
    output.Append(Corner(b, -side + along, input[1].color));
}

float4 PSMain(VSOutput input) : SV_TARGET {
    return input.color;
}
