#pragma once

#include <engine/assets/AssetPipeline.hpp>

namespace engine::detail {

/// Recenters when asked, then derives the physics geometry. Shared by source and cooked mesh loads.
void FinishMesh(MeshCpu& mesh, bool centerOnLoad);

} // namespace engine::detail
