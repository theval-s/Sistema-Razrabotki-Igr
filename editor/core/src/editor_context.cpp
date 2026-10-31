#include <editor/editor_context.hpp>

#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>
#include <utility>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/editor_camera.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>

namespace editor {

namespace {

ads::DockWidgetArea toAdsArea(const Qt::DockWidgetArea area) {
    switch (area) {
    case Qt::LeftDockWidgetArea:
        return ads::LeftDockWidgetArea;
    case Qt::TopDockWidgetArea:
        return ads::TopDockWidgetArea;
    case Qt::BottomDockWidgetArea:
        return ads::BottomDockWidgetArea;
    default:
        return ads::RightDockWidgetArea;
    }
}

}  // namespace


/// Context to pass to Modules instead of passing the whole editor to them
EditorContext::EditorContext(engine::Engine& engine, engine::Project& project, QMainWindow& window,
                             ads::CDockManager& dock_manager)
    : engine_(engine), project_(project), window_(window), dock_manager_(dock_manager), selection_(engine.GetScene()),
      scene_document_(std::make_unique<SceneDocument>(engine, project, selection_)),
      play_session_(std::make_unique<PlaySession>(engine, project, selection_, *scene_document_)) {}

EditorContext::~EditorContext() {
    // Panels would otherwise die with the dock manager, after the engine they show.
    // Not deleteDockWidget(): that defers the delete past the engine's shutdown.
    for (const Panel& panel : panels_) {
        if (panel.dock != nullptr) {
            dock_manager_.removeDockWidget(panel.dock);
            delete panel.dock;
        }
    }
}

engine::Engine& EditorContext::engine() noexcept {
    return engine_;
}

const engine::Project& EditorContext::project() const noexcept {
    return project_;
}

bool EditorContext::updateProject(engine::Project project, std::string& error) {
    if (!project.Save(error)) return false;
    project_ = std::move(project);
    return true;
}

engine::Scene& EditorContext::scene() noexcept {
    return engine_.GetScene();
}

Selection& EditorContext::selection() noexcept {
    return selection_;
}

SceneDocument& EditorContext::sceneDocument() noexcept {
    return *scene_document_;
}

PlaySession& EditorContext::playSession() noexcept {
    return *play_session_;
}

EditorCameraSubsystem* EditorContext::camera() noexcept {
    // MainWindow registers it when it starts the engine.
    return engine_.GetSubsystem<EditorCameraSubsystem>();
}

QMenu* EditorContext::menu(const QString& title) {
    const auto plain = [](QString text) { return text.remove(QLatin1Char('&')); };
    for (QAction* action : window_.menuBar()->actions()) {
        if (QMenu* existing = action->menu(); existing != nullptr && plain(existing->title()) == plain(title)) {
            return existing;
        }
    }
    return window_.menuBar()->addMenu(title);
}

QToolBar* EditorContext::toolBar() {
    if (tool_bar_ == nullptr) {
        tool_bar_ = window_.addToolBar(QObject::tr("Main"));
        tool_bar_->setObjectName(QStringLiteral("MainToolBar"));
    }
    return tool_bar_;
}

QWidget* EditorContext::mainWindow() noexcept {
    return &window_;
}

void EditorContext::showStatus(const QString& message, const int timeout_ms) {
    window_.statusBar()->showMessage(message, timeout_ms);
}

void EditorContext::addPanel(const QString& id, const QString& title, QWidget* widget,
                             const Qt::DockWidgetArea area) {
    auto* dock = new ads::CDockWidget(&dock_manager_, title);
    dock->setObjectName(id);
    // Panels manage their own scrolling (lists, text views).
    dock->setWidget(widget, ads::CDockWidget::ForceNoScrollArea);
    dock_manager_.addDockWidgetTab(toAdsArea(area), dock);
    menu(QObject::tr("&View"))->addAction(dock->toggleViewAction());
    panels_.push_back({dock, area});
}

void EditorContext::placeNewPanels() {
    const auto placed = [](const Panel& panel) { return panel.dock != nullptr && panel.dock->dockAreaWidget() != nullptr; };
    for (const Panel& panel : panels_) {
        if (panel.dock == nullptr || placed(panel)) {
            continue;
        }
        // The restored layout doesn't tell ADS which areas count as bottom, left...
        const auto sibling = std::ranges::find_if(panels_, [&](const Panel& other) {
            return other.area == panel.area && placed(other);
        });
        if (sibling != panels_.end()) {
            dock_manager_.addDockWidgetTabToArea(panel.dock, sibling->dock->dockAreaWidget());
        } else {
            dock_manager_.addDockWidgetTab(toAdsArea(panel.area), panel.dock);
        }
    }
}

}  // namespace editor
