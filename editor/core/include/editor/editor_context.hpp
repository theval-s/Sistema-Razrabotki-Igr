#pragma once

#include <QPointer>
#include <QString>
#include <Qt>

#include <memory>
#include <vector>

#include <editor/editor_core_export.hpp>
#include <editor/selection.hpp>

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

class EditorCameraSubsystem;
class SceneDocument;

/// What the editor hands to its modules: the engine, the project, the scene being edited, the
/// selection and the places a module may add UI to. Owned by MainWindow; outlives every module and is
/// destroyed before the engine.
class EDITOR_CORE_API EditorContext {
public:
    EditorContext(engine::Engine& engine, const engine::Project& project, QMainWindow& window,
                  ads::CDockManager& dock_manager);
    /// Deletes the panels added through addPanel().
    ~EditorContext();

    EditorContext(const EditorContext&) = delete;
    EditorContext& operator=(const EditorContext&) = delete;

    [[nodiscard]] engine::Engine& engine() noexcept;
    /// The open project; switching projects restarts the editor because we're unreal
    [[nodiscard]] const engine::Project& project() const noexcept;
    [[nodiscard]] engine::Scene& scene() noexcept;
    /// What the user has selected in the scene. Panels follow and change it through here.
    [[nodiscard]] Selection& selection() noexcept;
    /// The scene file being edited: open, create and save scenes through it.
    [[nodiscard]] SceneDocument& sceneDocument() noexcept;
    /// The viewport's POV
    [[nodiscard]] EditorCameraSubsystem* camera() noexcept;

    /// Finds the menu-bar menu with this title (ignoring '&' mnemonics) or appends a new one.
    QMenu* menu(const QString& title);
    /// The editor's main toolbar, created on first use.
    QToolBar* toolBar();

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
    const engine::Project& project_;
    QMainWindow& window_;
    ads::CDockManager& dock_manager_;
    QToolBar* tool_bar_ = nullptr;
    std::vector<Panel> panels_;
    Selection selection_;
    std::unique_ptr<SceneDocument> scene_document_;
};

}  // namespace editor
