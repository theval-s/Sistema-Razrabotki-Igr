#include "Common.hlsl"
#include "ShaderTypes.hlsl"

Texture2D albedoTexture : register(t0);
Texture2D emissionTexture : register(t1);


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
    surface.emission = Material.emission * emissionTexture.Sample(LinearWrapSampler, input.tex).rgb;
    surface.lightingWeight = Material.lightingWeight;
    PatchGBufferSurface(input, surface);
    return EncodeMaterialSurface(surface);
}
