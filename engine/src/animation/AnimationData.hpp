#pragma once

#include <string>
#include <vector>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <SimpleMath.h>

namespace engine::detail {

struct SkeletonData {
    ozz::animation::Skeleton skeleton;
    std::vector<std::string> nodePaths;
};

struct AnimationData {
    ozz::animation::Animation animation;
};

/// ozz stores column vectors; each stored column becomes a row in the engine.
inline DirectX::SimpleMath::Matrix ToEngineMatrix(const ozz::math::Float4x4& source) {
    DirectX::SimpleMath::Matrix result;
    ozz::math::StorePtrU(source.cols[0], &result._11);
    ozz::math::StorePtrU(source.cols[1], &result._21);
    ozz::math::StorePtrU(source.cols[2], &result._31);
    ozz::math::StorePtrU(source.cols[3], &result._41);
    return result;
}

} // namespace engine::detail
