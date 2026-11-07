#pragma once

#include <QList>
#include <QObject>

#include <editor/editor_core_export.hpp>

class QAction;
class QWidget;

namespace engine {
class Engine;
}

namespace editor {

class PlaySession;
class SceneHistory;
class Selection;

/// The Edit menu's scene commands, shared by panels, only available when not in play mode
class EDITOR_CORE_API EditActions : public QObject {
    Q_OBJECT

public:
    EditActions(engine::Engine& engine, Selection& selection, SceneHistory& history, PlaySession& play_session,
                QObject* parent = nullptr);

    /// Makes the shortcuts work while `scope` or one of its children has focus.
    void attach(QWidget* scope) const;
    /// All of them, in menu order.
    [[nodiscard]] QList<QAction*> actions() const;

    [[nodiscard]] QAction* undoAction() const noexcept { return undo_; }
    [[nodiscard]] QAction* redoAction() const noexcept { return redo_; }
    /// Copies the selected entities with their children, in place, and selects the copies.
    [[nodiscard]] QAction* duplicateAction() const noexcept { return duplicate_; }
    /// Deletes the selected entities, and with them their children.
    [[nodiscard]] QAction* deleteAction() const noexcept { return delete_; }
    [[nodiscard]] QAction* selectAllAction() const noexcept { return select_all_; }
    [[nodiscard]] QAction* deselectAction() const noexcept { return deselect_; }
    /// Moves the editor camera so the selection fills the view.
    [[nodiscard]] QAction* focusAction() const noexcept { return focus_; }

private:
    void duplicateSelected();
    void duplicate();
    void deleteSelected();
    void remove();
    void selectAll();
    void focusSelection();
    /// Enables what makes sense for the current selection, and nothing during Play.
    void refresh();

    engine::Engine& engine_;
    Selection& selection_;
    SceneHistory& history_;
    PlaySession& play_session_;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* duplicate_ = nullptr;
    QAction* delete_ = nullptr;
    QAction* select_all_ = nullptr;
    QAction* deselect_ = nullptr;
    QAction* focus_ = nullptr;
};

}  // namespace editor
