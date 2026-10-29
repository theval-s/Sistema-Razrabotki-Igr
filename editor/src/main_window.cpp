#include <editor/main_window.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QCloseEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QTimer>

#include <utility>
#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <engine/assets/AssetSystem.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>

#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/module_loader.hpp>
#include <editor/paths.hpp>
#include <editor/project_dialog.hpp>
#include <editor/viewport_widget.hpp>

namespace editor {

namespace {

QString toQString(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
    setWindowTitle(tr("%1 — SRI Editor").arg(QString::fromStdString(project_.name)));
    resize(1280, 720);

    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_);

    // Becomes the main window's central widget. The viewport is the docking system's
    // central widget: always there, never floated, so its HWND (and swap chain) stays put.
    dock_manager_ = new ads::CDockManager(this);
    viewport_ = new ViewportWidget;
    auto* viewport_dock = new ads::CDockWidget(dock_manager_, tr("Viewport"));
    viewport_dock->setObjectName(QStringLiteral("Viewport"));
    viewport_dock->setWidget(viewport_, ads::CDockWidget::ForceNoScrollArea);
    dock_manager_->setCentralWidget(viewport_dock);

    context_ = std::make_unique<EditorContext>(engine_, project_, *this, *dock_manager_);
    buildMenus();
    buildLogDock();  // before modules and engine init, so their startup messages show up

    // Panels first, then the saved layout, then the engine: the swap chain is created at
    // the viewport's size and can't follow later layout changes without stretching.
    modules_ = std::make_unique<ModuleLoader>();
    modules_->loadAll(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("editor_modules")),
                      *context_);
    restoreLayout();

    tick_timer_ = new QTimer(this);
    tick_timer_->setTimerType(Qt::PreciseTimer);
    connect(tick_timer_, &QTimer::timeout, this, &MainWindow::tickEngine);

    // The swap chain needs the viewport's real size, which it only has once the window
    // is shown and laid out: start the engine from the event loop.
    QTimer::singleShot(0, this, &MainWindow::startEngine);

    refreshStatus();
}

MainWindow::~MainWindow() {
    tick_timer_->stop();
    // The viewport outlives the engine (Qt deletes children after our members).
    viewport_->setInput(nullptr);
    // First, so nothing logs into the Log panel once it is gone.
    std::erase(spdlog::default_logger()->sinks(), log_sink_);
    // Modules and panels go before the engine they use.
    modules_.reset();
    context_.reset();
}

void MainWindow::closeEvent(QCloseEvent* event) {
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
    // The editor owns the loop: the engine only advances when we Tick() it.
    engine_.SetSurface(viewport_->surface());
    // The camera's aspect ratio follows the surface (RenderExtractionSubsystem); the renderer keeps
    // its initial buffer size and DXGI stretches it, so proportions stay right if blurrier.
    connect(viewport_, &ViewportWidget::surfaceResized, this,
            [this](quint32 width, quint32 height) { engine_.ResizeSurface(width, height); });
    // Input first, so it's latched before anything reads it; the camera moves before
    // RenderExtractionSubsystem copies it into the RenderWorld.
    viewport_->setInput(engine_.AddSubsystem<engine::InputSubsystem>());
    engine_.AddSubsystem<EditorCameraSubsystem>();
    engine_.AddSubsystem<engine::RenderingSubsystem>();
    engine_.AddSubsystem<engine::RenderExtractionSubsystem>();

    // On failure keep the window up so the Log dock shows why, but don't tick.
    if (!engine_.Initialize()) {
        spdlog::critical("Engine failed to initialize, see log above");
        return;
    }
    setupDefaultScene();

    tick_timer_->start(16);
    refreshStatus();
}

void MainWindow::setupDefaultScene() {
    using DirectX::SimpleMath::Quaternion;
    using DirectX::SimpleMath::Vector3;
    engine::Scene& scene = engine_.GetScene();

    // Looking down at the origin; EditorCameraSubsystem flies it.
    const entt::entity camera = scene.CreateEntity("Camera");
    scene.GetTransform(camera).position = Vector3{0.0f, 4.0f, 9.0f};
    scene.GetTransform(camera).rotation = Quaternion::CreateFromYawPitchRoll(0.0f, -0.4f, 0.0f);
    scene.AddComponent<engine::Camera>(camera);
    scene.SetActiveCamera(camera);

    const entt::entity sun = scene.CreateEntity("Sun");
    Vector3 sun_direction{-0.45f, -1.0f, -0.3f};
    sun_direction.Normalize();
    scene.GetTransform(sun).rotation = Quaternion::FromToRotation(Vector3::Forward, sun_direction);
    scene.AddComponent<engine::DirectionalLight>(sun, Vector3{1.0f, 0.95f, 0.85f}, 1.25f);
    scene.SetDirectionalLight(sun);

    const entt::entity ground = scene.CreateEntity("Ground");
    engine::Transform& transform = scene.GetTransform(ground);
    transform.position = Vector3{0.0f, -0.05f, 0.0f};
    transform.scale = Vector3{20.0f, 0.1f, 20.0f};
    scene.AddComponent<engine::MeshRenderer>(ground,
        engine_.GetSceneAssets().RequestMesh(engine::BuiltinAssets::CubeMesh),
        engine_.GetSceneAssets().RequestMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance));
}

void MainWindow::tickEngine() {
    engine_.Tick();
    refreshStatus();
}

void MainWindow::buildMenus() {
    auto* file_menu = context_->menu(tr("&File"));
    file_menu->addAction(tr("New &Project..."), this, [this] { switchProject(createProjectInteractively(this)); });
    file_menu->addAction(tr("&Open Project..."), this, [this] { switchProject(pickProjectFile(this)); });
    file_menu->addSeparator();
    file_menu->addAction(tr("E&xit"), this, &QWidget::close);

    auto* engine_menu = context_->menu(tr("&Engine"));
    auto* pause_action = engine_menu->addAction(tr("&Pause simulation"));
    pause_action->setCheckable(true);
    connect(pause_action, &QAction::toggled, this, [this](bool paused) { engine_.SetPaused(paused); });

    // Filled by EditorContext::addPanel().
    context_->menu(tr("&View"));
}

void MainWindow::buildLogDock() {
    log_view_ = new QPlainTextEdit;
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(5000);

    context_->addPanel(QStringLiteral("LogDock"), tr("Log"), log_view_, Qt::BottomDockWidgetArea);

    // Hook into spdlog's default logger, which the engine logs to as well.
    // The sink runs on whatever thread logs: copy the text and hop to the GUI
    // thread. Queued calls are dropped if `this` is gone.
    log_sink_ = std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& msg) {
        const auto level = spdlog::level::to_string_view(msg.level);
        QMetaObject::invokeMethod(
            this,
            [this, level = toQString({level.data(), level.size()}),
             text = toQString({msg.payload.data(), msg.payload.size()})] { appendLog(level, text); },
            Qt::QueuedConnection);
    });
    spdlog::default_logger()->sinks().push_back(log_sink_);
}

void MainWindow::appendLog(const QString& level, const QString& text) {
    log_view_->appendPlainText(QStringLiteral("[%1] %2").arg(level, text));
}

void MainWindow::refreshStatus() {
    const auto& frame = engine_.GetFrame();
    status_label_->setText(tr("engine %1 | frame %2 | t=%3s%4")
                               .arg(toQString(engine::version()))
                               .arg(frame.frameIndex)
                               .arg(frame.simulationTimeSeconds, 0, 'f', 1)
                               .arg(engine_.IsPaused() ? tr(" | paused") : QString()));
}

}  // namespace editor
