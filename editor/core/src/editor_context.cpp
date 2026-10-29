#include <editor/editor_context.hpp>

#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>

#include <DockManager.h>
#include <DockWidget.h>

#include <engine/engine.hpp>

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
EditorContext::EditorContext(engine::Engine& engine, const engine::Project& project, QMainWindow& window,
                             ads::CDockManager& dock_manager)
    : engine_(engine), project_(project), window_(window), dock_manager_(dock_manager), selection_(engine.GetScene()),
      scene_document_(std::make_unique<SceneDocument>(engine, project, selection_)) {}

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

engine::Scene& EditorContext::scene() noexcept {
    return engine_.GetScene();
}

Selection& EditorContext::selection() noexcept {
    return selection_;
}

SceneDocument& EditorContext::sceneDocument() noexcept {
    return *scene_document_;
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

void EditorContext::addPanel(const QString& id, const QString& title, QWidget* widget,
                             const Qt::DockWidgetArea area) {
    auto* dock = new ads::CDockWidget(&dock_manager_, title);
    dock->setObjectName(id);
    // Panels manage their own scrolling (lists, text views).
    dock->setWidget(widget, ads::CDockWidget::ForceNoScrollArea);
    dock_manager_.addDockWidget(toAdsArea(area), dock);
    menu(QObject::tr("&View"))->addAction(dock->toggleViewAction());
    panels_.push_back({dock, area});
}

void EditorContext::placeNewPanels() {
    for (const Panel& panel : panels_) {
        // Panels in the saved layout have an area even when closed; the unknown ones have none.
        if (panel.dock != nullptr && panel.dock->dockAreaWidget() == nullptr) {
            dock_manager_.addDockWidget(toAdsArea(panel.area), panel.dock);
        }
    }
}

}  // namespace editor
