#include "ParticleHooks.hlsli"
#include "ParticleStyle.hlsli"
Texture2DArray<float4> particleTextures : register(t9);
SamplerState particleSampler : register(s0);
float4 EvaluateParticle(Particle p,float2 uv) {
    ParticleStyle s=Style(p); float age=ParticleAge(p);
    float4 value;
    if(p.meta.y>=2) {
        value=ParticleAppearance(p,uv,age);
        // An authored texture also modulates procedural/custom appearances. Layer zero
        // is the engine soft disc: leave it out so untextured hooks retain their own shape.
        if(s.flags.x!=0) value*=particleTextures.SampleLevel(particleSampler,float3(uv*.5+.5,s.flags.x),0);
    }
    else {
        uint w,h,l,m; particleTextures.GetDimensions(0,w,h,l,m);
        value=l ? particleTextures.SampleLevel(particleSampler,float3(uv*.5+.5,s.flags.x),0) : float4(1,1,1,pow(saturate(1-dot(uv,uv)),2));
        value.rgb*=p.colorSize.rgb;
    }
    value.rgb*=lerp(s.colorStartFade.rgb,s.colorEndSoft.rgb,age);
    value.a*=lerp(s.sizeOpacity.z,s.sizeOpacity.w,age)*(s.colorStartFade.w>0 ? saturate(age/s.colorStartFade.w) : 1);
    return value;
}
