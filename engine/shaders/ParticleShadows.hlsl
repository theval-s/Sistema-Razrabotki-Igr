#include "ParticleAppearance.hlsli"
StructuredBuffer<Particle> pool : register(t0);
StructuredBuffer<uint> indices : register(t1);
StructuredBuffer<uint> live : register(t2);
Texture2DArray<float> opaque : register(t3);
cbuffer Frame : register(b0) { row_major float4x4 lightMatrix; uint cascade, targetSize; uint2 pad; };
struct Varying { float4 position:SV_POSITION; float2 uv:TEXCOORD0; nointerpolation uint id:TEXCOORD1; };
Varying VSMain(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    Varying o=(Varying)0; o.position=float4(2,2,2,1);
    if(instance>=live[0]) return o;
    o.id=indices[instance]; Particle p=pool[o.id]; if(!Style(p).flags.z) return o;
    float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    o.uv=corners[vertex]; float a=ParticleRotation(p),c=cos(a),s=sin(a);
    float2 q=float2(c*o.uv.x-s*o.uv.y,s*o.uv.x+c*o.uv.y);
    float2 scale=float2(length(float3(lightMatrix._11,lightMatrix._21,lightMatrix._31)),length(float3(lightMatrix._12,lightMatrix._22,lightMatrix._32)));
    o.position=mul(float4(p.positionAge.xyz,1),lightMatrix);
    o.position.xy+=q*float2(1,-1)*scale*ParticleRadius(p);
    return o;
}
float4 PSMain(Varying input):SV_TARGET {
    // Conservative nearest reverse-Z receiver over this low-resolution texel's footprint.
    uint w,h,l; opaque.GetDimensions(w,h,l);
    uint2 lo=uint2(input.position.xy)*uint2(w,h)/targetSize;
    uint2 hi=((uint2(input.position.xy)+1)*uint2(w,h)+targetSize-1)/targetSize;
    float receiver=0;
    for(uint y=lo.y;y<max(hi.y,lo.y+1);++y) for(uint x=lo.x;x<max(hi.x,lo.x+1);++x)
        receiver=max(receiver,opaque.Load(int4(min(uint2(x,y),uint2(w-1,h-1)),cascade,0)));
    if(input.position.z<receiver) discard;
    Particle p=pool[input.id];
    float alpha=saturate(EvaluateParticle(p,input.uv).a);
    float density=max(0,Style(p).variationDensity.z);
    alpha=density>0 ? 1-pow(1-alpha,density) : 0;
    return float4(0,0,0,alpha);
}
