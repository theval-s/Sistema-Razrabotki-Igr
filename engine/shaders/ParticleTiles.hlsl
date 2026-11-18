// See ParticleTiles.hpp for frame and shader variant contracts.
#if defined(PARTICLE_TILES_SHADE)
#include "ParticleAppearance.hlsli"
#else
#include "Particle.hlsli"
#endif
#include "ParticleStyle.hlsli"
struct Sprite { float2 center, extent; float depth, radius; uint2 pad; };
cbuffer Frame : register(b0) {
    row_major float4x4 view, projection;
    uint2 size; uint coarse, words;
    uint columns, level, stepSize, padding;
};
StructuredBuffer<Particle> pool : register(t0);
StructuredBuffer<uint> indices : register(t1);
StructuredBuffer<uint> live : register(t2);
// Lexicographic ascending key: inverted positive camera distance (far first), seed,
// emitter, pool index. All-ones sorts invisible and padding records last.
bool Less(uint4 a, uint4 b) {
    return a.x!=b.x ? a.x<b.x : a.y!=b.y ? a.y<b.y : a.z!=b.z ? a.z<b.z : a.w<b.w;
}
#if defined(PARTICLE_TILES_BUILD) || defined(PARTICLE_TILES_MERGE)
RWStructuredBuffer<uint4> keys : register(u0);
RWStructuredBuffer<Sprite> projected : register(u1);
groupshared uint4 block[256];
void LocalStep(uint i, uint lane, uint k, uint j) {
    uint4 a=block[lane], b=block[lane^j];
    bool wantMin=((i&k)==0)==((lane&j)==0);
    GroupMemoryBarrierWithGroupSync();
    block[lane]=(Less(b,a)==wantMin) ? b : a;
    GroupMemoryBarrierWithGroupSync();
}
[numthreads(256,1,1)] void CSMain(uint i:SV_DispatchThreadID, uint lane:SV_GroupIndex, uint group:SV_GroupID) {
    // GPU count only: restrict the network to the smallest power of two containing all
    // live keys. Whole groups outside it need no initialization or compare/exchange.
    uint count=live[0], extent=1u<<(firstbithigh(max(256u,count)-1u)+1u);
    if(group>=extent/256) return;
#if defined(PARTICLE_TILES_BUILD)
    uint4 key=0xffffffffu;
    if (i<count) {
        uint index=indices[i]; Particle p=pool[index];
        float4 v=mul(float4(p.positionAge.xyz,1),view), clip=mul(v,projection);
        Sprite s=(Sprite)0;
        s.depth=-v.z; s.radius=ParticleRadius(p);
        if (s.depth>0 && clip.w>0 && clip.z>=0 && clip.z<=clip.w && s.radius>0) {
            s.center=(clip.xy/clip.w*float2(.5,-.5)+.5)*size;
            s.extent=.5*size*abs(float2(projection._11,projection._22))*s.radius/clip.w;
            float angle=ParticleRotation(p); s.extent*=abs(cos(angle))+abs(sin(angle));
            if (all(s.center+s.extent>=0) && all(s.center-s.extent<float2(size))) {
                projected[index]=s; key=uint4(~asuint(s.depth),p.meta.z,p.meta.x,index);
            }
        }
    }
    block[lane]=key;
    GroupMemoryBarrierWithGroupSync();
    [loop] for(uint k=2;k<=256;k*=2)
        [loop] for(uint j=k/2;j;j/=2) LocalStep(i,lane,k,j);
#else
    if(level>extent) return;
    if(stepSize>=256) {
        // Each disjoint pair has exactly one writer in this dispatch.
        if(!(i&stepSize)) {
            uint other=i^stepSize; uint4 a=keys[i], b=keys[other];
            if(Less(b,a)==((i&level)==0)) { keys[i]=b; keys[other]=a; }
        }
        return;
    }
    block[lane]=keys[i]; GroupMemoryBarrierWithGroupSync();
    [loop] for(uint j=128;j;j/=2) LocalStep(i,lane,level,j);
#endif
    keys[i]=block[lane];
}
#else
StructuredBuffer<uint4> sorted : register(t3);
StructuredBuffer<Sprite> sprites : register(t4);
#if defined(PARTICLE_TILES_BIN)
RWStructuredBuffer<uint> masks : register(u0);
[numthreads(64,1,1)] void CSMain(uint rank:SV_DispatchThreadID) {
    if(rank>=live[0]) return;
    uint index=sorted[rank].w; if(index==0xffffffffu) return;
    Sprite s=sprites[index];
    uint2 lo=(uint2)clamp(s.center-s.extent,0,float2(size-1))/coarse;
    uint2 hi=(uint2)clamp(s.center+s.extent,0,float2(size-1))/coarse;
    for(uint y=lo.y;y<=hi.y;++y) for(uint x=lo.x;x<=hi.x;++x)
        InterlockedOr(masks[(y*columns+x)*words+rank/32],1u<<(rank%32));
}
#elif defined(PARTICLE_TILES_SHADE)
StructuredBuffer<uint> masks : register(t5);
Texture2D<float> depthMap : register(t6);
Texture2D<float4> scene : register(t7);
RWTexture2D<float4> result : register(u0);
RWStructuredBuffer<uint> stats : register(u1);
// ParticleHooks.hlsli resolves the project's ParticleAppearance override or engine default.
// uv is signed unit-square space. Return straight linear HDR RGB + opacity; depth fade
// and premultiplication are applied here.
float4 Appearance(Particle p,float2 uv,float age) {
    return EvaluateParticle(p,uv);
}
groupshared uint ids[64], batchCount, finished;
groupshared Sprite batchSprite[64];
groupshared Particle batchParticle[64];
[numthreads(8,8,1)] void CSMain(uint2 pixel:SV_DispatchThreadID,uint2 tile:SV_GroupID,uint lane:SV_GroupIndex) {
    uint2 origin=tile*8, region=origin/coarse;
    uint base=(region.y*columns+region.x)*words;
    uint end=(live[0]+31)/32, word=0,bits=0,total=0;
    bool valid=all(pixel<size); float4 dst=0; float sceneDistance=3.4e38;
    if(valid) {
        dst=scene.Load(int3(pixel,0)); float d=depthMap.Load(int3(pixel,0));
        if(d>0) sceneDistance=(projection._43-d*projection._44)/(projection._33-d*projection._34);
    }
    [loop] for(;;) {
        if(!lane) {
            uint n=0;
            [loop] while(n<64 && (bits || word<end)) {
                if(!bits) { bits=masks[base+word++]; if(!bits) continue; }
                uint rank=(word-1)*32+firstbitlow(bits); bits &= bits-1;
                uint id=sorted[rank].w; Sprite s=sprites[id];
                if(all(s.center+s.extent>=origin+.5) && all(s.center-s.extent<=min(origin+8,size)-.5)) ids[n++]=id;
            }
            batchCount=n; total+=n; finished=(!bits && word==end);
        }
        GroupMemoryBarrierWithGroupSync();
        if(lane<batchCount) { batchSprite[lane]=sprites[ids[lane]]; batchParticle[lane]=pool[ids[lane]]; }
        GroupMemoryBarrierWithGroupSync();
        [loop] for(uint i=0;i<batchCount;++i) {
            Sprite s=batchSprite[i]; Particle p=batchParticle[i];
            float angle=ParticleRotation(p);
            float2 uv=ParticleUV(p,(pixel+.5-s.center)/s.extent*(abs(cos(angle))+abs(sin(angle))));
            if(all(abs(uv)<=1) && sceneDistance>s.depth) {
                ParticleStyle style=Style(p); float4 src=Appearance(p,uv,ParticleAge(p));
                float alpha=saturate(src.a)* (style.colorEndSoft.w>0 ? saturate((sceneDistance-s.depth)/style.colorEndSoft.w) : 1);
                if(style.flags.y==1) dst.rgb+=src.rgb*alpha;
                else dst=float4(src.rgb,1)*alpha+dst*(1-alpha);
            }
        }
        if(finished) break;
        GroupMemoryBarrierWithGroupSync();
    }
    if(valid) result[pixel]=dst;
    if(!lane && total) { InterlockedMax(stats[0],total); if(total>64) InterlockedAdd(stats[1],1); }
}
#endif
#endif
