#ifndef ENGINE_SKINNING_INCLUDE_H
#define ENGINE_SKINNING_INCLUDE_H

bool SkinningFinite(float value) {
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

// Scale before normalizing to avoid overflow, zero-length division and NaNs.
float3 SkinningSafeNormal(float3 value, float3 fallback) {
    float3 result = fallback;
    float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
    if (all((asuint(value) & 0x7f800000u) != 0x7f800000u) && scale > 1e-20) {
        value /= scale;
        result = value * rsqrt(dot(value, value));
    }
    return result;
}

#ifdef ENGINE_SKINNED
// Matches MaxSkinningJoints and SkinningBufferData: 256 row-vector matrices, 16 KiB.
cbuffer EngineSkinning : register(b9) {
    row_major float4x4 SkinningMatrices[256];
};

void SkinVertex(inout MaterialVertex vertex, uint4 joints, float4 weights) {
    weights = float4(SkinningFinite(weights.x) ? max(weights.x, 0.0) : 0.0,
                     SkinningFinite(weights.y) ? max(weights.y, 0.0) : 0.0,
                     SkinningFinite(weights.z) ? max(weights.z, 0.0) : 0.0,
                     SkinningFinite(weights.w) ? max(weights.w, 0.0) : 0.0);
    float maxWeight = max(max(weights.x, weights.y), max(weights.z, weights.w));
    if (maxWeight <= 0.0) {
        vertex.normal = SkinningSafeNormal(vertex.normal, float3(0, 1, 0));
        return;
    }
    weights /= maxWeight;
    weights /= dot(weights, float4(1, 1, 1, 1));
    const float4x4 identity = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    float4x4 blended = (float4x4)0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        // Bound even malformed vertex indices; absent palette entries are identity.
        float4x4 bone = SkinningMatrices[min(joints[i], 255u)];
        if (joints[i] >= 256u) bone = identity;
        blended += weights[i] * bone;
    }
    vertex.position = mul(vertex.position, blended);

    // For row vectors the cofactor matrix is det(A) * inverse-transpose(A).
    // Positive scaling preserves the normalized result and bounds the cofactors.
    float3x3 affine = (float3x3)blended;
    float scale = max(max(max(abs(affine[0].x), abs(affine[0].y)), abs(affine[0].z)),
                      max(max(max(abs(affine[1].x), abs(affine[1].y)), abs(affine[1].z)),
                          max(max(abs(affine[2].x), abs(affine[2].y)), abs(affine[2].z))));
    float3 original = SkinningSafeNormal(vertex.normal, float3(0, 1, 0));
    if (!(scale > 1e-20) || !SkinningFinite(scale)) {
        vertex.normal = original;
        return;
    }
    affine /= scale;
    float3x3 cofactor = float3x3(cross(affine[1], affine[2]),
                               cross(affine[2], affine[0]),
                               cross(affine[0], affine[1]));
    float determinant = dot(affine[0], cofactor[0]);
    float3 fallback = SkinningSafeNormal(mul(original, affine), original);
    vertex.normal = abs(determinant) > 1e-8 && SkinningFinite(determinant)
        ? SkinningSafeNormal(mul(original, cofactor) * (determinant < 0 ? -1.0 : 1.0), fallback)
        : fallback;
}
#endif
#endif
