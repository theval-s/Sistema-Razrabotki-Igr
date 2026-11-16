#pragma once

#include <QString>

#include <filesystem>
#include <string>

#include <entt/entity/entity.hpp>

namespace engine {
class Engine;
}

namespace editor {
class SceneHistory;
}

/// Where `region` saves its bake: its current file if that is this scene's and not another region's,
/// else "<scene>.<region name>.navmesh" beside the scene, with the region's id added if that is taken.
[[nodiscard]] std::filesystem::path navigationBakeFile(engine::Engine& engine, const std::filesystem::path& scene_file,
                                                       entt::entity region);

/// Builds `region`, saves it to navigationBakeFile() with a .meta and points the region at it as one
/// undo step. `scene_file` is the saved scene in the project content. False with `error` (UTF-8).
bool bakeNavigation(engine::Engine& engine, editor::SceneHistory& history, const std::filesystem::path& scene_file,
                    entt::entity region, std::string& error);

/// The region's bake for the inspector: its file, or what to do about it. Cheap enough for every refresh.
[[nodiscard]] QString navigationBakeStatus(engine::Engine& engine, entt::entity region);
