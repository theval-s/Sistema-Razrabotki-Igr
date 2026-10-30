#pragma once

#include <QMainWindow>
#include <QString>

#include <memory>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

namespace ads {
class CDockManager;
}

class QLabel;
class QPlainTextEdit;
class QTimer;

namespace spdlog::sinks {
class sink;
}

namespace editor {

class EditorCameraSubsystem;
class EditorContext;
class ModuleLoader;
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
    void buildMenus();
    void buildLogDock();
    /// Opens the project's startup scene, or builds an untitled one if it has none.
    void openStartupScene();
    void setupDefaultScene();
    /// Scene name and unsaved-changes mark.
    void refreshTitle();
    /// Remembers a saved scene as the project's startup scene if it has none yet.
    void adoptStartupScene();
    void restoreLayout();
    void refreshStatus();
    void appendLog(const QString& level, const QString& text);
    /// Closes the editor and starts a new one on `project_file`: assets are cached by
    /// name, so another project's game/... names can't be loaded into this engine.
    void switchProject(const QString& project_file);

    // Destroyed bottom-up: modules, then the context (and its panels), then the engine.
    engine::Project project_;
    engine::Engine engine_;
    std::unique_ptr<EditorContext> context_;
    std::unique_ptr<ModuleLoader> modules_;
    std::shared_ptr<spdlog::sinks::sink> log_sink_;

    ads::CDockManager* dock_manager_ = nullptr;
    ViewportWidget* viewport_ = nullptr;
    EditorCameraSubsystem* camera_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* tick_timer_ = nullptr;
    /// Set by switchProject(): started once the layout is saved.
    QString relaunch_project_;
};

}  // namespace editor
