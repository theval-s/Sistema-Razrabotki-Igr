#pragma once

#include <QPointer>
#include <QString>
#include <Qt>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <editor/editor_core_export.hpp>
#include <editor/selection.hpp>
#include <editor/viewport_tool_bar.hpp>

class QMainWindow;
class QMenu;
class QToolBar;
class QWidget;

namespace ads {
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace engine {
class Engine;
class Scene;
struct Project;
}  // namespace engine

namespace editor {

class EditActions;
class EditorCameraSubsystem;
class PlaySession;
class SceneDocument;
class SceneEvents;
class SceneHistory;
class ShortcutRegistry;
class StartupScene;
struct ViewportView;

/// What the editor hands to its modules: the engine, the project, the scene being edited, the
/// selection and the places a module may add UI to. Owned by MainWindow; outlives every module and is
/// destroyed before the engine.
class EDITOR_CORE_API EditorContext {
public:
    EditorContext(engine::Engine& engine, engine::Project& project, QMainWindow& window,
                  ads::CDockManager& dock_manager, QWidget& viewport, ViewportToolBar& tool_bar);
    /// Deletes the panels added through addPanel().
    ~EditorContext();

    EditorContext(const EditorContext&) = delete;
    EditorContext& operator=(const EditorContext&) = delete;

    [[nodiscard]] engine::Engine& engine() noexcept;
    /// The open project; switching projects restarts the editor because we're unreal
    [[nodiscard]] const engine::Project& project() const noexcept;
    /// Saves `project` to its file and makes it the open project. False with `error` if it
    /// couldn't be saved; the open project is then unchanged.
    bool updateProject(engine::Project project, std::string& error);
    [[nodiscard]] engine::Scene& scene() noexcept;
    /// The scene's changes as a Qt signal: follow the scene through here instead of polling it.
    [[nodiscard]] SceneEvents& sceneEvents() noexcept;
    /// What the user has selected in the scene. Panels follow and change it through here.
    [[nodiscard]] Selection& selection() noexcept;
    /// Undo/redo: make every edit to the scene through it, so it can be undone.
    [[nodiscard]] SceneHistory& history() noexcept;
    /// The scene file being edited: open, create and save scenes through it.
    [[nodiscard]] SceneDocument& sceneDocument() noexcept;
    /// The scene the project opens with.
    [[nodiscard]] StartupScene& startupScene() noexcept;
    /// Play mode: start and stop it, or follow it, through here.
    [[nodiscard]] PlaySession& playSession() noexcept;
    /// Delete, Select All, Focus... for the panels that show the scene: attach() them to a panel
    /// to make their shortcuts work there.
    [[nodiscard]] EditActions& editActions() noexcept;
    /// The viewport's POV
    [[nodiscard]] EditorCameraSubsystem* camera() noexcept;
    /// Where the scene is drawn; viewport tools (e.g. drop targets) filter its events.
    [[nodiscard]] QWidget* viewport() noexcept;
    /// How the viewport sees the scene right now; nothing before the engine runs.
    [[nodiscard]] std::optional<ViewportView> viewportView() const;
    /// Every shortcut: bind actions through it instead of setShortcut(), so the user can change them.
    [[nodiscard]] ShortcutRegistry& shortcuts() noexcept;

    /// Finds the menu-bar menu with this title (ignoring '&' mnemonics) or appends a new one.
    QMenu* menu(const QString& title);
    /// A section of the toolbar above the viewport.
    QToolBar* toolBar(ViewportToolBar::Section section);
    /// Parent for the dialogs and message boxes a module opens outside its own panels.
    [[nodiscard]] QWidget* mainWindow() noexcept;
    /// Shows `message` in the status bar for `timeout_ms`.
    void showStatus(const QString& message, int timeout_ms);

    /// Docks `widget` (reparented into a new dock panel) at `area`, as a tab beside the panels
    /// already there, and lists it under View.
    /// The user can then move, tab, float or close it. `id` must be unique and stable:
    /// it is the panel's objectName, which the saved layout refers to.
    void addPanel(const QString& id, const QString& title, QWidget* widget, Qt::DockWidgetArea area);

    /// Call after restoring a saved layout: docks the panels it didn't know about (e.g. from
    /// a newly added module) at their default area. ADS leaves those hidden and unplaced.
    void placeNewPanels();

private:
    struct Panel {
        QPointer<ads::CDockWidget> dock;
        Qt::DockWidgetArea area;
    };

    engine::Engine& engine_;
    engine::Project& project_;
    QMainWindow& window_;
    ads::CDockManager& dock_manager_;
    QWidget& viewport_;
    ViewportToolBar& tool_bar_;
    std::vector<Panel> panels_;
    std::unique_ptr<ShortcutRegistry> shortcuts_;
    std::unique_ptr<SceneEvents> scene_events_;  // before everything that follows the scene
    Selection selection_;
    std::unique_ptr<SceneHistory> history_;
    std::unique_ptr<SceneDocument> scene_document_;
    std::unique_ptr<PlaySession> play_session_;
    std::unique_ptr<EditActions> edit_actions_;
    std::unique_ptr<StartupScene> startup_scene_;  // last: it uses the ones above
};

}  // namespace editor
