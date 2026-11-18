// Compile CSMain three times, defining exactly one of PARTICLE_SIM_UPDATE/ADMIT/SPAWN.
#if defined(PARTICLE_SIM_UPDATE) || defined(PARTICLE_SIM_SPAWN)
#include "ParticleHooks.hlsli"
#define PARTICLE_STYLE_SLOT t3
#include "ParticleStyle.hlsli"
#endif

struct Emitter { float4 positionLife, directionSpeed, tintRadius, jitter; uint4 requestLimitKindSeed; };
cbuffer Frame : register(b0) { float dt; uint capacity, slots, frame; };
// stats: alive, rejected, exclusive end of spawn range (absolute index), first birth index (slot 0).
#if defined(PARTICLE_SIM_UPDATE)
StructuredBuffer<Particle> previous : register(t0);
StructuredBuffer<uint> previousCount : register(t1);
RWStructuredBuffer<Particle> pool : register(u0);
RWStructuredBuffer<uint> count : register(u1);
RWStructuredBuffer<uint4> stats : register(u2);
[numthreads(64,1,1)] void CSMain(uint i : SV_DispatchThreadID) {
    if (i >= previousCount[0]) return;
    Particle p = previous[i];
    const uint4 originalMeta = p.meta;
    p.positionAge.w += dt;
    ParticleStyle style=Style(p);
    p.velocityLifetime.xyz=(p.velocityLifetime.xyz+style.accelerationDrag.xyz*dt)*exp(-max(0,style.accelerationDrag.w)*dt);
    p.positionAge.xyz += dt * p.velocityLifetime.xyz;
    ParticleUpdate(p, dt);
    p.meta.xzw = originalMeta.xzw;
    if (p.positionAge.w >= p.velocityLifetime.w) return;
    uint dest;
    InterlockedAdd(count[0], 1, dest);
    InterlockedAdd(stats[p.meta.x].x, 1);
    pool[dest] = p;
}
#elif defined(PARTICLE_SIM_ADMIT)
StructuredBuffer<Emitter> emitters : register(t0);
RWStructuredBuffer<uint> count : register(u1);
RWStructuredBuffer<uint4> stats : register(u2);
RWBuffer<uint> activeArgs : register(u3);
RWBuffer<uint> spawnArgs : register(u4);
// One reservation per slot rather than atomics per requested birth. Bounded even for UINT_MAX requests.
[numthreads(1,1,1)] void CSMain() {
    uint first = count[0], end = first;
    for (uint s = 0; s < slots; ++s) {
        uint2 r = emitters[s].requestLimitKindSeed.xy;
        uint alive = stats[s].x;
        uint n = min(r.x, min(r.y - min(alive,r.y), capacity-end));
        end += n;
        stats[s] = uint4(alive+n,r.x-n,end,first);
    }
    count[0] = end;
    activeArgs[0] = (end+63)/64; activeArgs[1] = 1; activeArgs[2] = 1;
    spawnArgs[0] = (end-first+63)/64; spawnArgs[1] = 1; spawnArgs[2] = 1;
}
#elif defined(PARTICLE_SIM_SPAWN)
StructuredBuffer<Emitter> emitters : register(t0);
StructuredBuffer<uint4> stats : register(t1);
StructuredBuffer<uint> count : register(t2);
RWStructuredBuffer<Particle> pool : register(u0);
uint Mix(uint x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16);
}
[numthreads(64,1,1)] void CSMain(uint lane : SV_DispatchThreadID) {
    uint i = stats[0].w + lane;
    if (i >= count[0]) return;
    uint lo = 0, hi = slots-1;
    while (lo < hi) { uint m = (lo+hi)/2; if (i < stats[m].z) hi=m; else lo=m+1; }
    Emitter e = emitters[lo];
    uint seed = Mix(i ^ Mix(frame) ^ Mix(e.requestLimitKindSeed.w));
    float2 uv = float2(seed >> 8, Mix(seed) >> 8) / 16777216.0;
    float z = 2*uv.x-1, a = uv.y*6.2831853;
    float3 jitter = float3(sqrt(saturate(1-z*z))*float2(cos(a),sin(a)), z);
    float3 d = e.directionSpeed.xyz + e.jitter.x*jitter;
    d /= max(max(abs(d.x),abs(d.y)),max(abs(d.z),1e-30));
    Particle p;
    p.positionAge = float4(e.positionLife.xyz,0);
    p.velocityLifetime = float4(d*rsqrt(max(dot(d,d),1e-20))*e.directionSpeed.w,e.positionLife.w);
    p.colorSize = e.tintRadius; p.meta = uint4(lo,e.requestLimitKindSeed.z,seed,0);
    ParticleStyle style=Style(p);
    p.velocityLifetime.w*=max(1e-4,1+(2*(Mix(seed+1)>>8)/16777216.0-1)*saturate(style.variationDensity.x));
    p.velocityLifetime.xyz*=1+(2*(Mix(seed+2)>>8)/16777216.0-1)*saturate(style.variationDensity.y);
    ParticleSpawn(p);
    p.meta.xzw = uint3(lo, seed, 0);
    pool[i] = p;
}
#endif
