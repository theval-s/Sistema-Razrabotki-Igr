#ifndef SRI_PARTICLE_HLSLI
#define SRI_PARTICLE_HLSLI

struct Particle {
    float4 positionAge;
    float4 velocityLifetime;
    float4 colorSize;
    uint4 meta; // emitter slot, appearance ID, identity/seed, reserved
};

#endif
