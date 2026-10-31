#include <editor/play_session.hpp>

#include <spdlog/spdlog.h>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/editor_camera.hpp>
#include <editor/scene_document.hpp>
#include <editor/selection.hpp>

namespace editor {

PlaySession::PlaySession(engine::Engine& engine, const engine::Project& project, Selection& selection,
                         SceneDocument& document)
    : engine_(engine), project_(project), selection_(selection), document_(document) {}

bool PlaySession::isPlaying() const {
    return engine_.IsPlaying();
}

bool PlaySession::isPaused() const {
    return isPlaying() && engine_.IsPaused();
}

bool PlaySession::play(std::string& error) {
    if (isPlaying()) {
        error.clear();
        return true;
    }
    if (!engine_.IsInitialized()) {
        error = "The engine is not running, see the Log";
        return false;
    }
    edit_selection_.clear();
    for (const auto entity : selection_.entities()) {
        edit_selection_.emplace_back(engine_.GetScene().GetId(entity).ToString());
    }
    document_.beginRuntime();
    if (!engine_.PlayCurrentScene(project_.ContentDir(), project_.autoloads, error)) {
        document_.endRuntime();
        restoreSelection();
        return false;
    }
    selection_.clear();
    if (auto* camera = engine_.GetSubsystem<EditorCameraSubsystem>()) {
        camera->SetNavigationEnabled(false);
    }
    if (engine_.GetScene().GetActiveCamera() == entt::null) {
        spdlog::warn("Play: no game camera; using the frozen editor viewpoint");
    }
    emit playingChanged(true);
    return true;
}

void PlaySession::stop() {
    if (!isPlaying()) return;
    engine_.StopPlay();
    if (auto* camera = engine_.GetSubsystem<EditorCameraSubsystem>()) {
        camera->SetNavigationEnabled(true);
    }
    document_.endRuntime();
    restoreSelection();
    emit playingChanged(false);
}

void PlaySession::setPaused(const bool paused) {
    if (!isPlaying() || engine_.IsPaused() == paused) return;
    engine_.SetPaused(paused);
    emit pausedChanged(paused);
}

void PlaySession::restoreSelection() {
    std::vector<entt::entity> entities;
    for (const auto& id : edit_selection_) {
        entities.push_back(engine_.GetScene().FindById(id));
    }
    selection_.setEntities(entities);
}

}  // namespace editor
