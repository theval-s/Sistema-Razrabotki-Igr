void ParticleSpawn(inout Particle p) {}
void ParticleUpdate(inout Particle p, float dt) {}

float4 ParticleAppearance(Particle p, float2 uv, float life01) {
    return float4(p.colorSize.rgb, saturate(1.0 - dot(uv, uv)) * (1.0 - life01));
}
