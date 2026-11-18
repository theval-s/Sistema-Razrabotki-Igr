#include <QDialogButtonBox>
 #include <QFormLayout>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <editor/main_window.hpp>

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QDir>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QTimer>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <string>
#include <utility>

#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <engine/core/DefaultSubsystems.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>

#include <editor/edit_actions.hpp>
#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/log_model.hpp>
#include <editor/log_panel.hpp>
#include <editor/menu_button.hpp>
#include <editor/module_loader.hpp>
#include <editor/paths.hpp>
#include <editor/play_session.hpp>
#include <editor/preferences_dialog.hpp>
#include <editor/project_dialog.hpp>
#include <editor/scene_document.hpp>
#include <editor/transform_gizmo.hpp>
#include <editor/viewport_overlay.hpp>
#include <editor/viewport_picker.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <editor/viewport_widget.hpp>
#include <editor/selection.hpp>
#include <editor/shortcut_registry.hpp>
#include <editor/startup_scene.hpp>
#include <editor/tool_icon.hpp>
#include <editor/editor_log.hpp>
#include <editor/rendering_status.hpp>
#include <editor/session_log.hpp>

namespace editor {

namespace {

QString toQString(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

DirectX::SimpleMath::Vector2 toVector(const QPointF point) {
    return {static_cast<float>(point.x()), static_cast<float>(point.y())};
}

// Bump when panel ids change incompatibly: restoreState() then ignores the old layout.
constexpr int LayoutVersion = 1;
const QString GeometryKey = QStringLiteral("layout/geometry");
const QString DockStateKey = QStringLiteral("layout/docks");

engine::EngineConfig makeEngineConfig() {
    engine::EngineConfig config;
    config.applicationName = "SRI Editor";
    return config;
}

}  // namespace

MainWindow::MainWindow(engine::Project project, QWidget* parent)
    : QMainWindow(parent), project_(std::move(project)), engine_(makeEngineConfig()) {
    resize(1280, 720);

    rendering_status_button_ = new QToolButton(this);
    rendering_status_button_->setObjectName(QStringLiteral("renderingStatusSummary"));
    rendering_status_button_->setAutoRaise(true);
    rendering_status_button_->setFocusPolicy(Qt::NoFocus);
    rendering_status_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    rendering_status_button_->setIconSize({16, 16});
    statusBar()->addPermanentWidget(rendering_status_button_);
    rendering_status_button_->hide();
    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_);
    play_hint_ = new QLabel(this);
    statusBar()->addWidget(play_hint_);

    // Becomes the main window's central widget. The viewport is the docking system's
    // central widget: always there, never floated, so its HWND (and swap chain) stays put.
    dock_manager_ = new ads::CDockManager(this);
    viewport_ = new ViewportWidget;
    connect(viewport_, &ViewportWidget::mousePressed, this, &MainWindow::viewportPressed);
    connect(viewport_, &ViewportWidget::mouseMoved, this, &MainWindow::viewportMoved);
    connect(viewport_, &ViewportWidget::mouseReleased, this, &MainWindow::viewportReleased);
    auto* tool_bar = new ViewportToolBar;
    debug_view_ = new QComboBox;
    debug_view_->setObjectName(QStringLiteral("renderDebugView"));
    debug_view_->setFocusPolicy(Qt::NoFocus);
    debug_view_->setEnabled(false);
    const struct { const char* name; const char* tooltip; engine::RenderDebugView view; } views[] = {
        {"Final", "Lit scene with tone mapping. Selection outlines are shown only in Final.", engine::RenderDebugView::Final},
        {"Albedo", "Stored surface color without lighting or tone mapping. Selection outlines are Final-only.", engine::RenderDebugView::Albedo},
        {"Normals", "Encoded world-space normals (normal * 0.5 + 0.5). Selection outlines are Final-only.", engine::RenderDebugView::Normals},
        {"Depth", "Linear camera-space depth: near bright, far dark; background black (reverse-Z source). Selection outlines are Final-only.", engine::RenderDebugView::Depth},
    };
    for (const auto& view : views) {
        debug_view_->addItem(tr(view.name), static_cast<int>(view.view));
        debug_view_->setItemData(debug_view_->count() - 1, tr(view.tooltip), Qt::ToolTipRole);
    }
    debug_view_->setToolTip(debug_view_->itemData(0, Qt::ToolTipRole).toString());
    connect(debug_view_, &QComboBox::currentIndexChanged, this, [this](int index) {
        debug_view_->setToolTip(debug_view_->itemData(index, Qt::ToolTipRole).toString());
        if (auto* rendering = engine_.GetSubsystem<engine::RenderingSubsystem>(); rendering &&
            rendering->GetDiagnostics().lifecycle == engine::RenderingLifecycle::Ready) {
            (void)rendering->SetDebugView(static_cast<engine::RenderDebugView>(debug_view_->itemData(index).toInt()));
        }
    });
    tool_bar->section(ViewportToolBar::Section::Right)->addWidget(debug_view_);
    // Qt controls belong outside the native flip-model swap-chain surface. The overlays read
    // these viewport properties each frame.
    auto* overlays = new QMenu(this);
    overlays->setToolTipsVisible(true);
    const struct { const char* property; QString text; QString tooltip; } overlay_entries[] = {
        {"showSkeletonOverlay", tr("Skeletons"), tr("Bones of the selected skinned meshes")},
        {"showAudioOverlay", tr("Audio Sources"), tr("Audio sources and listeners, with their ranges")},
    };
    for (const auto& entry : overlay_entries) {
        auto* toggle = overlays->addAction(entry.text);
        toggle->setObjectName(QString::fromUtf8(entry.property));
        toggle->setToolTip(entry.tooltip);
        toggle->setCheckable(true);
        toggle->setChecked(true);
        viewport_->setProperty(entry.property, true);
        connect(toggle, &QAction::toggled, viewport_, [viewport = viewport_, property = entry.property](bool checked) {
            viewport->setProperty(property, checked);
        });
    }
    auto* show_overlays = makeMenuButton(tr("Show"), overlays);
    show_overlays->setObjectName(QStringLiteral("viewportShowMenu"));
    show_overlays->setFocusPolicy(Qt::NoFocus);
    show_overlays->setToolTip(tr("Editor overlays drawn over the viewport"));
    tool_bar->section(ViewportToolBar::Section::Right)->addWidget(show_overlays);
    auto* viewport_panel = new QWidget;
    auto* viewport_layout = new QVBoxLayout(viewport_panel);
    viewport_layout->setContentsMargins(0, 0, 0, 0);
    viewport_layout->setSpacing(0);
    viewport_layout->addWidget(tool_bar);
    rendering_details_dialog_ = new QDialog(this);
    rendering_details_dialog_->setWindowTitle(tr("Rendering diagnostics"));
    rendering_details_dialog_->resize(760, 440);
    auto* detail_layout = new QVBoxLayout(rendering_details_dialog_);
    rendering_details_ = new QPlainTextEdit;
    rendering_details_->setReadOnly(true);
    detail_layout->addWidget(rendering_details_);
    auto* detail_buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* detail_log = detail_buttons->addButton(tr("Show Log"), QDialogButtonBox::ActionRole);
    detail_log->setObjectName(QStringLiteral("showRenderingLog"));
    connect(detail_log, &QPushButton::clicked, this, &MainWindow::showRenderingLog);
    connect(detail_buttons, &QDialogButtonBox::rejected, rendering_details_dialog_, &QDialog::hide);
    detail_layout->addWidget(detail_buttons);
    connect(rendering_status_button_, &QToolButton::clicked, this, [this] {
        rendering_details_dialog_->show();
        rendering_details_dialog_->raise();
    });
    viewport_layout->addWidget(viewport_, 1);
    auto* viewport_dock = new ads::CDockWidget(dock_manager_, tr("Viewport"));
    viewport_dock->setObjectName(QStringLiteral("Viewport"));
    viewport_dock->setWidget(viewport_panel, ads::CDockWidget::ForceNoScrollArea);
    dock_manager_->setCentralWidget(viewport_dock);

    context_ = std::make_unique<EditorContext>(engine_, project_, *this, *dock_manager_, *viewport_, *tool_bar);
    rendering_status_ = std::make_unique<RenderingStatus>(engine_);
    context_->editActions().attach(viewport_);
    viewport_->setShortcuts(context_->shortcuts());
    connect(viewport_, &ViewportWidget::inputCaptureChanged, this, &MainWindow::refreshPlayHint);
    gizmo_ = std::make_unique<TransformGizmo>(engine_.GetScene(), context_->selection(), &context_->history());
    picker_ = std::make_unique<ViewportPicker>(engine_, context_->selection());
    SceneDocument& document = context_->sceneDocument();
    connect(&document, &SceneDocument::stateChanged, this, &MainWindow::refreshTitle);
    connect(&document, &SceneDocument::opened, this, [this] {
        picker_->reset();
        if (camera_ != nullptr) {
            camera_->ResetToSceneCamera();
        }
    });
    connect(&context_->playSession(), &PlaySession::playingChanged, this, [this](const bool playing) {
        viewport_->setPlaying(playing);
        // The running copy isn't the scene being edited.
        gizmo_->setEnabled(!playing);
        refreshPlayHint();
    });
    connect(&context_->playSession(), &PlaySession::pausedChanged, this, [this](const bool paused) {
        viewport_->setPaused(paused);
        refreshPlayHint();
    });
    refreshTitle();
    buildMenus();
    buildLogDock();  // before modules and engine init, so their startup messages show up

    // Panels first, then the saved layout, then the engine: the swap chain is created at
    // the viewport's size and can't follow later layout changes without stretching.
    modules_ = std::make_unique<ModuleLoader>();
    modules_->loadAll(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("editor_modules")),
                      *context_);
    restoreLayout();

    tick_timer_ = new QTimer(this);
    tick_timer_->setObjectName(QStringLiteral("engineTickTimer"));
    tick_timer_->setTimerType(Qt::PreciseTimer);
    connect(tick_timer_, &QTimer::timeout, this, &MainWindow::tickEngine);

    // The swap chain needs the viewport's real size, which it only has once the window
    // is shown and laid out: start the engine from the event loop.
    QTimer::singleShot(0, this, &MainWindow::startEngine);

    refreshStatus();
}

MainWindow::~MainWindow() {
    tick_timer_->stop();
    context_->playSession().stop();
    rendering_status_.reset();
    // The viewport outlives the engine (Qt deletes children after our members).
    viewport_->setInput(nullptr);
    viewport_->setCamera(nullptr);
    // First, so nothing logs into the Log panel once it is gone.
    engine::log::Sinks()->remove_sink(log_sink_);
    // Modules and panels go before the engine they use.
    modules_.reset();
    context_.reset();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    context_->playSession().stop();
    if (!context_->maybeSaveAll(this)) {
        relaunch_project_.clear();
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue(GeometryKey, saveGeometry());
    settings.setValue(DockStateKey, dock_manager_->saveState(LayoutVersion));
    QMainWindow::closeEvent(event);

    if (!relaunch_project_.isEmpty() &&
        !QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--project"), relaunch_project_})) {
        QMessageBox::warning(this, tr("Open Project"), tr("Could not start the editor for %1").arg(relaunch_project_));
    }
}

void MainWindow::switchProject(const QString& project_file) {
    if (project_file.isEmpty() || toPath(project_file) == project_.file) {
        return;
    }
    relaunch_project_ = project_file;
    close();
}

void MainWindow::restoreLayout() {
    // Saved panels whose module is gone are ignored; panels missing from the saved state
    // are put back at their default place by placeNewPanels().
    // Nothing saved yet on the first run; ADS would complain about the empty state.
    const QSettings settings;
    if (settings.contains(GeometryKey)) {
        restoreGeometry(settings.value(GeometryKey).toByteArray());
    }
    if (settings.contains(DockStateKey)) {
        dock_manager_->restoreState(settings.value(DockStateKey).toByteArray(), LayoutVersion);
        context_->placeNewPanels();
    }
}

void MainWindow::startEngine() {
    if (engine_wired_) return;
    wireEngine();
    initializeEngine();
}

void MainWindow::wireEngine() {
    if (engine_wired_) return;
    engine_wired_ = true;
    engine_.SetEditorMode();
    // The editor owns the loop: the engine only advances when we Tick() it.
    engine_.SetSurface(viewport_->surface());
    // Resize rendering and UI in physical surface pixels.
    connect(viewport_, &ViewportWidget::surfaceResized, this, &MainWindow::surfaceResized);
    // Input first, so it's latched before anything reads it; the editor camera last, so it
    // replaces the scene camera that RenderExtractionSubsystem copied into the RenderWorld.
    viewport_->setInput(engine::RegisterDefaultSubsystems(engine_, viewport_->uiHost()));
    camera_ = engine_.AddSubsystem<EditorCameraSubsystem>();
    engine_.AddSubsystem<ViewportOverlaySubsystem>(context_->selection(), *gizmo_, *viewport_);
}

void MainWindow::surfaceResized(quint32 width, quint32 height) {
    engine_.ResizeSurface(width, height);
    if (!startup_pending_surface_ || initializing_engine_ || !width || !height) return;
    const auto* rendering = engine_.GetSubsystem<engine::RenderingSubsystem>();
    // Failed is latched: a normal resize must never become a device-recovery loop.
    if (rendering && rendering->GetDiagnostics().lifecycle == engine::RenderingLifecycle::SurfaceUnavailable)
        initializeEngine();
}

void MainWindow::initializeEngine() {
    if (initializing_engine_ || engine_start_completed_) return;
    initializing_engine_ = true;
    startup_pending_surface_ = false;
    const bool initialized = engine_.Initialize();
    initializing_engine_ = false;

    // On failure keep the window up so the Log dock shows why, but don't tick.
    if (!initialized) {
        const auto* rendering = engine_.GetSubsystem<engine::RenderingSubsystem>();
        startup_pending_surface_ = rendering &&
            rendering->GetDiagnostics().lifecycle == engine::RenderingLifecycle::SurfaceUnavailable;
        startup_failed_ = !startup_pending_surface_;
        if (startup_failed_) editorLog().critical("Engine failed to initialize, see log above");
        refreshStatus(); // Initialization failure never starts the tick timer.
        return;
    }
    engine_start_completed_ = true;
    startup_failed_ = false;
    viewport_->setCamera(camera_);
    context_->startupScene().open();

    tick_timer_->start(16);
    refreshStatus();
}

void MainWindow::refreshTitle() {
    const SceneDocument& document = context_->sceneDocument();
    // [*] shows the unsaved-changes mark set by setWindowModified().
    setWindowTitle(tr("%1[*] — %2 — SRI Editor").arg(document.displayName(), QString::fromStdString(project_.name)));
    setWindowModified(document.isDirty());
}

void MainWindow::tickEngine() {
    // Poll the actual DPR too: a monitor/DPI change need not resize the pixel surface.
    const auto metrics = viewport_->uiMetrics();
    const auto& surface = engine_.GetSurface();
    if (surface.width != metrics.pixelWidth || surface.height != metrics.pixelHeight)
        engine_.ResizeSurface(metrics.pixelWidth, metrics.pixelHeight);
    engine_.Tick();
    viewport_->applyCursorMode();
    const auto* rendering = engine_.GetSubsystem<engine::RenderingSubsystem>();
    const bool ready = rendering && rendering->GetDiagnostics().lifecycle == engine::RenderingLifecycle::Ready;
    if (engine_.GetFrame().frameIndex % 15 == 0 || debug_view_->isEnabled() != ready) refreshStatus();
}

void MainWindow::refreshPlayHint() {
    const PlaySession& session = context_->playSession();
    if (!session.isPlaying()) {
        play_hint_->clear();
    } else if (!viewport_->capturesInput()) {
        play_hint_->setText(tr("Click the viewport to give the game input"));
    } else if (const auto release = context_->shortcuts().shortcuts(ViewportWidget::ReleaseInputId); !release.isEmpty()) {
        play_hint_->setText(tr("Game has input, %1 to release").arg(release[0].toString(QKeySequence::NativeText)));
    } else {
        play_hint_->setText(tr("Game has input, click outside the viewport to release"));
    }
}

void MainWindow::viewportPressed(const QPointF position, const Qt::MouseButton button,
                                 const Qt::KeyboardModifiers modifiers) {
    if (button == Qt::RightButton) {
        gizmo_->cancel();
        return;
    }
    const auto view = context_->viewportView();
    if (button != Qt::LeftButton || !view) return;
    // The gizmo's handles are in front of everything.
    if (!gizmo_->press(*view, toVector(position))) {
        picker_->pick(*view, toVector(position), modifiers.testFlag(Qt::ControlModifier));
    }
}

void MainWindow::viewportMoved(const QPointF position) {
    const auto view = context_->viewportView();
    if (!view) return;
    if (gizmo_->dragging()) {
        gizmo_->drag(*view, toVector(position));
    } else {
        gizmo_->hover(*view, toVector(position));
    }
}

void MainWindow::viewportReleased(QPointF, const Qt::MouseButton button) {
    if (button == Qt::LeftButton) gizmo_->release();
}

void MainWindow::buildMenus() {
    auto* file_menu = context_->menu(tr("&File"));
    const auto add = [this, file_menu](const QString& id, const QString& text, const QKeySequence& shortcut,
                                       auto&& slot) {
        context_->shortcuts().bind(file_menu->addAction(text, this, slot), id, tr("File"), shortcut);
    };
    add(QStringLiteral("file.new-project"), tr("New &Project..."), {},
        [this] { switchProject(createProjectInteractively(this)); });
    add(QStringLiteral("file.open-project"), tr("&Open Project..."), {},
        [this] { switchProject(pickProjectFile(this)); });
    file_menu->addSeparator();
    SceneDocument& document = context_->sceneDocument();
    add(QStringLiteral("file.new-scene"), tr("&New Scene..."), QKeySequence::New,
        [this, &document] { document.createInteractive(this); });
    add(QStringLiteral("file.open-scene"), tr("Open &Scene..."), QKeySequence::Open,
        [this, &document] { document.openInteractive(this); });
    add(QStringLiteral("file.save-scene"), tr("&Save Scene"), QKeySequence::Save,
        [this, &document] { document.saveInteractive(this, false); });
    add(QStringLiteral("file.save-scene-as"), tr("Save Scene &As..."), QKeySequence::SaveAs,
        [this, &document] { document.saveInteractive(this, true); });
    file_menu->addSeparator();

    add(QStringLiteral("file.exit"), tr("E&xit"), {}, [this] { close(); });
    // Scenes and projects can't be opened or saved while the scene is a running copy.
    connect(&context_->playSession(), &PlaySession::playingChanged, file_menu, &QWidget::setDisabled);

    const EditActions& edit = context_->editActions();
    auto* edit_menu = context_->menu(tr("&Edit"));
    edit_menu->addAction(edit.undoAction());
    edit_menu->addAction(edit.redoAction());
    edit_menu->addSeparator();
    edit_menu->addAction(edit.cutAction());
    edit_menu->addAction(edit.copyAction());
    edit_menu->addAction(edit.pasteAction());
    edit_menu->addAction(edit.duplicateAction());
    edit_menu->addAction(edit.deleteAction());
    edit_menu->addSeparator();
    edit_menu->addAction(edit.selectAllAction());
    edit_menu->addAction(edit.deselectAction());
    edit_menu->addSeparator();
    edit_menu->addAction(edit.focusAction());
    edit_menu->addSeparator();
    buildGizmoActions(*edit_menu);
    edit_menu->addSeparator();
    auto* preferences = edit_menu->addAction(tr("Prefere&nces..."), this, [this] {
        PreferencesDialog dialog(context_->shortcuts(), this);
        dialog.exec();
    });
    context_->shortcuts().bind(preferences, QStringLiteral("edit.preferences"), tr("Edit"),
                               QKeySequence(Qt::CTRL | Qt::Key_Comma));

    // The menu bar's order. Modules fill Project and Engine, EditorContext::addPanel() fills View.
    for (const QString& title : {tr("&Project"), tr("&Engine"), tr("&View")}) {
        context_->menu(title);
    }
}

void MainWindow::buildGizmoActions(QMenu& menu) {
    // Shortcuts only while the viewport has focus, so W/E/R/X still type in fields elsewhere.
    // While the right button turns the view, the viewport keeps them for the camera.
    QToolBar* bar = context_->toolBar(ViewportToolBar::Section::Left);
    const auto add = [this, &menu, bar](const QString& id, const QString& text, const Qt::Key key, const QString& icon) {
        auto* action = menu.addAction(toolIcon(icon), text);
        context_->shortcuts().bind(action, id, tr("Gizmo"), QKeySequence(key));
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        action->setCheckable(true);
        viewport_->addAction(action);
        bar->addAction(action);
        // During Play these keys are the game's.
        connect(&context_->playSession(), &PlaySession::playingChanged, action, &QAction::setDisabled);
        return action;
    };
    auto* modes = new QActionGroup(this);
    const struct {
        QString id;
        QString text;
        Qt::Key key;
        QString icon;
        TransformGizmo::Mode mode;
    } entries[] = {
        {QStringLiteral("gizmo.move"), tr("&Move"), Qt::Key_W, QStringLiteral("move"), TransformGizmo::Mode::Translate},
        {QStringLiteral("gizmo.rotate"), tr("&Rotate"), Qt::Key_E, QStringLiteral("rotate"), TransformGizmo::Mode::Rotate},
        {QStringLiteral("gizmo.scale"), tr("S&cale"), Qt::Key_R, QStringLiteral("scale"), TransformGizmo::Mode::Scale},
    };
    for (const auto& entry : entries) {
        const auto mode = entry.mode;
        auto* action = add(entry.id, entry.text, entry.key, entry.icon);
        action->setActionGroup(modes);
        action->setChecked(gizmo_->mode() == mode);
        connect(action, &QAction::triggered, this, [this, mode] { gizmo_->setMode(mode); });
    }
    bar->addSeparator();
    auto* local = add(QStringLiteral("gizmo.local-axes"), tr("&Local Axes"), Qt::Key_X, QStringLiteral("local-axes"));
    local->setChecked(gizmo_->space() == TransformGizmo::Space::Local);
    connect(local, &QAction::toggled, this, [this](const bool checked) {
        gizmo_->setSpace(checked ? TransformGizmo::Space::Local : TransformGizmo::Space::World);
    });
}

void MainWindow::buildLogDock() {
    auto* panel = new LogPanel(context_->shortcuts());
    log_panel_ = panel;
    context_->addPanel(QStringLiteral("LogDock"), tr("Log"), panel, Qt::BottomDockWidgetArea);

    // Every log channel writes to these sinks, the engine's and the editor's. The panel's
    // only buffers, so logging from other threads never touches the panel.
    log_sink_ = panel->sink();
    engine::log::Sinks()->add_sink(log_sink_);
}

void MainWindow::showRenderingLog() {
    if (!log_panel_) return;
    context_->showPanel(log_panel_);
    log_panel_->revealDiagnostic(rendering_log_search_, rendering_detail_text_);
}

void MainWindow::refreshStatus() {
    auto* rendering = engine_.GetSubsystem<engine::RenderingSubsystem>();
    const auto diagnostics = rendering ? rendering->GetDiagnostics() : engine::RenderingDiagnostics{};
    debug_view_->setEnabled(diagnostics.lifecycle == engine::RenderingLifecycle::Ready);
    if (rendering) {
        const QSignalBlocker blocker(debug_view_);
        debug_view_->setCurrentIndex(debug_view_->findData(static_cast<int>(rendering->GetDebugView())));
        debug_view_->setToolTip(debug_view_->currentData(Qt::ToolTipRole).toString());
    }
    auto status = rendering_status_->describe(diagnostics, startup_failed_, context_->playSession().isPlaying(),
        rendering ? rendering->GetWorld().sceneItems.size() : 0);
    if (!fileLogError().isEmpty()) {
        if (status.summary.isEmpty()) status.summary = tr("Session file log unavailable — see Details");
        status.detail += tr("\nSession file log: %1").arg(fileLogError());
        status.warning = true;
    }
    if (rendering_summary_ != status.summary || rendering_warning_ != status.warning) {
        rendering_summary_ = status.summary;
        rendering_warning_ = status.warning;
        // A renderer failure names its pass, operation and path: the tooltip keeps the full text.
        const int width = rendering_status_button_->fontMetrics().averageCharWidth() * 80;
        rendering_status_button_->setText(
            rendering_status_button_->fontMetrics().elidedText(status.summary, Qt::ElideRight, width));
        rendering_status_button_->setToolTip(status.summary);
        rendering_status_button_->setIcon(status.warning ? style()->standardIcon(QStyle::SP_MessageBoxWarning) : QIcon());
        rendering_status_button_->setStyleSheet(status.warning ? QStringLiteral("color: #e6a15a;") : QString());
        rendering_status_button_->setVisible(!status.summary.isEmpty());
    }
    rendering_log_search_ = status.logSearch;
    if (rendering_detail_text_ != status.detail) {
        rendering_detail_text_ = status.detail;
        rendering_details_->setPlainText(status.detail);
    }
    const auto& frame = engine_.GetFrame();
    status_label_->setText(tr("engine %1 | frame %2 | t=%3s%4")
                               .arg(toQString(engine::version()))
                               .arg(frame.frameIndex)
                               .arg(frame.simulationTimeSeconds, 0, 'f', 1)
                               .arg(engine_.IsPaused() ? tr(" | paused") : QString()));
}

}  // namespace editor
