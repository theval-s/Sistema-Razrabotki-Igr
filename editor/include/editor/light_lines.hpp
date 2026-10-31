#pragma once

#include <vector>

#include <SimpleMath.h>
#include <entt/entity/entity.hpp>

namespace engine {
class Scene;
}

namespace editor {

// Appends line segments for an entity's lights to the render world's editor lines.
void lightLines(std::vector<DirectX::SimpleMath::Vector3>& lines,
                const engine::Scene& scene, entt::entity entity);

}  // namespace editor
