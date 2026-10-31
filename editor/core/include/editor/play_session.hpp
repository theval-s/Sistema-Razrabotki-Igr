#pragma once

#include <QObject>

#include <string>
#include <vector>

#include <editor/editor_core_export.hpp>

namespace engine {
class Engine;
struct Project;
}  // namespace engine

namespace editor {

class SceneDocument;
class Selection;

/// Play mode: runs the open scene with the project's autoloads, and on Stop puts the edited scene
/// and selection back. Owned by EditorContext; the play controls drive it, and whatever behaves
/// differently while playing follows its signals.
class EDITOR_CORE_API PlaySession : public QObject {
    Q_OBJECT

public:
    PlaySession(engine::Engine& engine, const engine::Project& project, Selection& selection,
                SceneDocument& document);

    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] bool isPaused() const;

    /// Plays the open scene. False with `error` if it couldn't start; the editor is then unchanged.
    bool play(std::string& error);
    /// Back to editing. Does nothing when not playing.
    void stop();
    /// Only while playing; editing always keeps the engine paused.
    void setPaused(bool paused);

signals:
    void playingChanged(bool playing);
    void pausedChanged(bool paused);

private:
    void restoreSelection();

    engine::Engine& engine_;
    const engine::Project& project_;
    Selection& selection_;
    SceneDocument& document_;
    /// The edited selection, by entity id: play replaces the scene's entities.
    std::vector<std::string> edit_selection_;
};

}  // namespace editor
