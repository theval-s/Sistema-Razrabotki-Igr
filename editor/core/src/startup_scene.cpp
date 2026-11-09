#include <editor/startup_scene.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/project/Project.hpp>

#include <editor/editor_context.hpp>
#include <editor/editor_log.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>

namespace editor {

StartupScene::StartupScene(EditorContext& context, QObject* parent) : QObject(parent), context_(context) {
    connect(&context_.sceneDocument(), &SceneDocument::stateChanged, this, &StartupScene::adopt);
}

void StartupScene::open() {
    SceneDocument& document = context_.sceneDocument();
    const engine::Project& project = context_.project();
    std::string error;
    if (!project.startupScene.empty()) {
        if (!document.open(engine::AssetPathResolver::ToAbsolute(engine::FromUtf8(project.startupScene)), error)) {
            editorLog().error("Startup scene '{}': {}", project.startupScene, error);
        }
        return;
    }
    // A new project gets a scene file, which adopt() then makes its startup scene.
    const std::filesystem::path file = project.ContentDir() / "scenes" / "Main.scene.json";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const bool opened = std::filesystem::exists(file, ec) ? document.open(file, error) : document.create(file, error);
    if (!opened) {
        editorLog().error("Could not open {}: {}", engine::ToUtf8(file), error);
    }
}

void StartupScene::adopt() {
    if (context_.playSession().isPlaying()) return;
    const SceneDocument& document = context_.sceneDocument();
    const std::filesystem::path& file = document.filePath();
    if (file.empty() || document.isDirty()) {
        return;
    }
    // Keep a startup scene that still exists; one that was moved or deleted gets replaced.
    const engine::Project& project = context_.project();
    std::error_code ec;
    if (!project.startupScene.empty() &&
        std::filesystem::exists(engine::AssetPathResolver::ToAbsolute(engine::FromUtf8(project.startupScene)), ec)) {
        return;
    }
    const std::string name = engine::AssetPathResolver::ToName(file);
    if (!name.starts_with("game/")) {
        return;  // outside the project's content
    }
    engine::Project updated = project;
    updated.startupScene = name;
    if (std::string error; !context_.updateProject(std::move(updated), error)) {
        editorLog().warn("Could not remember {} as the startup scene: {}", name, error);
    }
}

}  // namespace editor
