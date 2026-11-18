#ifndef SRI_PARTICLE_STYLE
#define SRI_PARTICLE_STYLE
#include "Particle.hlsli"
struct ParticleStyle {
    float4 accelerationDrag, sizeOpacity, colorStartFade, colorEndSoft, variationDensity;
    uint4 flags; // layer, blend, casts, random rotation
};
#ifndef PARTICLE_STYLE_SLOT
#define PARTICLE_STYLE_SLOT t8
#endif
StructuredBuffer<ParticleStyle> particleStyles : register(PARTICLE_STYLE_SLOT);
ParticleStyle Style(Particle p) {
    uint n,stride; particleStyles.GetDimensions(n,stride);
    if(p.meta.x<n) return particleStyles[p.meta.x];
    ParticleStyle s=(ParticleStyle)0;
    s.sizeOpacity=float4(1,1,1,1); s.colorStartFade=float4(1,1,1,0);
    s.colorEndSoft=float4(1,1,1,p.colorSize.w); s.variationDensity.z=1; s.flags.z=1;
    return s;
}
float ParticleAge(Particle p) { return saturate(p.positionAge.w/max(p.velocityLifetime.w,1e-6)); }
float ParticleRadius(Particle p) { ParticleStyle s=Style(p); return max(0,p.colorSize.w*lerp(s.sizeOpacity.x,s.sizeOpacity.y,ParticleAge(p))); }
float ParticleRotation(Particle p) { return Style(p).flags.w ? (p.meta.z>>8)*(6.2831853/16777216.0) : 0; }
float2 ParticleUV(Particle p,float2 q) { float a=ParticleRotation(p),c=cos(a),s=sin(a); return float2(c*q.x+s*q.y,-s*q.x+c*q.y); }
#endif
