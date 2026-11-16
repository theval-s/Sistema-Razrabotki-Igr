#pragma once

#include <cstddef>
#include <vector>
#include <SimpleMath.h>

namespace engine {

/// Per-section limit, independent of the number of joints in the skeleton.
inline constexpr std::size_t MaxSkinningJoints = 256;

/// Immutable for the lifetime of a submitted draw. Matrices transform mesh-local
/// positions into asset-local space, using the engine's row-vector convention.
struct SkinningPalette {
    std::vector<DirectX::SimpleMath::Matrix> matrices;
};

} // namespace engine
