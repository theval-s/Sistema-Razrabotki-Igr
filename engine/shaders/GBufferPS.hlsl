#include "Common.hlsl"
#include "ShaderTypes.hlsl"

Texture2D albedoTexture : register(t0);


#ifdef MATERIAL_HAS_HOOK
void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface);
#else
void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface){}
#endif

GBufferOutput PSMain(GBufferInput input) {
    MaterialSurface surface;
    surface.albedo = albedoTexture.Sample(LinearWrapSampler, input.tex);
    surface.normal = input.normal;
    surface.specularColor = Material.specularColor;
    surface.shininess = Material.shininess;
    PatchGBufferSurface(input, surface);
    GBufferOutput output;
    output.diffuse = surface.albedo;
    output.normal = float4(normalize(surface.normal) * 0.5 + 0.5, 1.0);
    output.material = float4(surface.specularColor, surface.shininess / 256.0);
    return output;
}