#pragma once

#include <QMainWindow>
#include <QString>
#include <QPointF>

#include <memory>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

namespace ads {
class CDockManager;
}

class QLabel;
class QComboBox;
class QPlainTextEdit;
class QDialog;
class QMenu;
class QTimer;

namespace spdlog::sinks {
class sink;
}

namespace editor {

class EditorCameraSubsystem;
class EditorContext;
class ModuleLoader;
class LogPanel;
class RenderingStatus;
class TransformGizmo;
class ViewportPicker;
class ViewportWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(engine::Project project, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    /// Saves the window geometry and dock layout, then starts the editor again if a
    /// project switch asked for it.
    void closeEvent(QCloseEvent* event) override;

private slots:
    void startEngine();
    void tickEngine();

private:
    friend class MainWindowTestAccess;
    void wireEngine();
    void initializeEngine();
    void surfaceResized(quint32 width, quint32 height);
    void buildMenus();
    /// Move/Rotate/Scale (W/E/R) and Local Axes (X), in `menu` and the viewport's toolbar.
    void buildGizmoActions(QMenu& menu);
    void buildLogDock();
    void showRenderingLog();
    /// Scene name and unsaved-changes mark.
    void refreshTitle();
    void restoreLayout();
    void refreshStatus();
    /// Whether the game has the input during Play, and how to change that.
    void refreshPlayHint();
    /// A left press grabs a gizmo handle, or else picks; a right press cancels a gizmo drag.
    void viewportPressed(QPointF position, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void viewportMoved(QPointF position);
    void viewportReleased(QPointF position, Qt::MouseButton button);
    /// Closes the editor and starts a new one on `project_file`: assets are cached by
    /// name, so another project's game/... names can't be loaded into this engine.
    void switchProject(const QString& project_file);

    // Destroyed bottom-up: modules, then the context (and its panels), then the engine.
    engine::Project project_;
    engine::Engine engine_;
    std::unique_ptr<EditorContext> context_;
    /// These use the scene and the selection: destroyed before them.
    std::unique_ptr<TransformGizmo> gizmo_;
    std::unique_ptr<ViewportPicker> picker_;
    std::unique_ptr<ModuleLoader> modules_;
    std::shared_ptr<spdlog::sinks::sink> log_sink_;
    std::unique_ptr<RenderingStatus> rendering_status_;

    ads::CDockManager* dock_manager_ = nullptr;
    ViewportWidget* viewport_ = nullptr;
    EditorCameraSubsystem* camera_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* play_hint_ = nullptr;
    QLabel* rendering_status_label_ = nullptr;
    QComboBox* debug_view_ = nullptr;
    LogPanel* log_panel_ = nullptr;
    QDialog* rendering_details_dialog_ = nullptr;
    QPlainTextEdit* rendering_details_ = nullptr;
    QString rendering_detail_text_;
    QString rendering_log_search_;
    bool startup_failed_ = false;
    bool engine_wired_ = false;
    bool initializing_engine_ = false;
    bool startup_pending_surface_ = false;
    bool engine_start_completed_ = false;
    QTimer* tick_timer_ = nullptr;
    /// Set by switchProject(): started once the layout is saved.
    QString relaunch_project_;
};

}  // namespace editor
