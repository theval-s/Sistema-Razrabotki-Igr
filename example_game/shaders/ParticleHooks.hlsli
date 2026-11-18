void ParticleSpawn(inout Particle p) {
    if (p.meta.y != 2) return;
    const float variation = float(p.meta.z & 255u) / 255.0;
    p.colorSize.w *= lerp(0.8, 1.2, variation);
}

void ParticleUpdate(inout Particle p, float dt) {
    if (p.meta.y == 2) p.velocityLifetime.y += 2.0 * dt;
}

float4 ParticleAppearance(Particle p, float2 uv, float life01) {
    const float r = length(uv);
    const float ring = saturate(1.0 - 6.0 * abs(r - lerp(0.25, 0.8, life01)));
    return float4(p.colorSize.rgb * 3.0, ring * (1.0 - life01));
}
