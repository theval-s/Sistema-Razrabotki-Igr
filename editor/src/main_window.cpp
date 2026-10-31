#include <editor/main_window.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/scene/SceneQueries.hpp>

#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/light_lines.hpp>
#include <editor/module_loader.hpp>
#include <editor/paths.hpp>
#include <editor/project_dialog.hpp>
#include <editor/scene_document.hpp>
#include <editor/viewport_widget.hpp>
#include <editor/selection.hpp>

namespace editor {

namespace {

// Runs after scene extraction and the editor camera, before RenderingSubsystem::Render.
class SelectionRenderSubsystem final : public engine::Subsystem {
public:
    explicit SelectionRenderSubsystem(const Selection& selection) : selection_(selection) {}
    std::string_view GetName() const override { return "SelectionRenderSubsystem"; }
    void LateUpdate(const engine::FrameContext&) override {
        auto& world = GetEngine().GetSubsystem<engine::RenderingSubsystem>()->GetWorld();
        world.selectedSceneItems.clear();
        world.editorLines.clear();
        auto& assets = GetEngine().GetSceneAssets();
        const auto& scene = GetEngine().GetScene();
        for (const auto entity : selection_.entities()) {
            if (!scene.IsValid(entity)) continue;
            lightLines(world.editorLines, scene, entity);
            if (const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity)) {
                const auto mesh = assets.Peek(renderer->mesh);
                const auto material = assets.Peek(renderer->materialInstance);
                if (mesh && material) {
                    const auto override = GetEngine().GetEntityMaterialOverride(entity);
                    world.selectedSceneItems.push_back({scene.GetWorldMatrix(entity), mesh->mesh,
                                                        override ? override : material->instance});
                }
            }
        }
    }
    void Shutdown() override {
        GetEngine().GetSubsystem<engine::RenderingSubsystem>()->GetWorld().selectedSceneItems.clear();
        GetEngine().GetSubsystem<engine::RenderingSubsystem>()->GetWorld().editorLines.clear();
    }
private:
    const Selection& selection_;
};

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
    resize(1280, 720);

    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_);

    // Becomes the main window's central widget. The viewport is the docking system's
    // central widget: always there, never floated, so its HWND (and swap chain) stays put.
    dock_manager_ = new ads::CDockManager(this);
    viewport_ = new ViewportWidget;
    connect(viewport_, &ViewportWidget::pickRequested, this, &MainWindow::pickViewport);
    auto* viewport_dock = new ads::CDockWidget(dock_manager_, tr("Viewport"));
    viewport_dock->setObjectName(QStringLiteral("Viewport"));
    viewport_dock->setWidget(viewport_, ads::CDockWidget::ForceNoScrollArea);
    dock_manager_->setCentralWidget(viewport_dock);

    context_ = std::make_unique<EditorContext>(engine_, project_, *this, *dock_manager_);
    SceneDocument& document = context_->sceneDocument();
    connect(&document, &SceneDocument::stateChanged, this, [this] {
        refreshTitle();
        adoptStartupScene();
    });
    connect(&document, &SceneDocument::opened, this, [this] {
        has_last_pick_ = false;
        if (camera_ != nullptr) {
            camera_->ResetToSceneCamera();
        }
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
    if (!context_->sceneDocument().maybeSave(this)) {
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
    // The editor owns the loop: the engine only advances when we Tick() it.
    engine_.SetSurface(viewport_->surface());
    // The camera's aspect ratio follows the surface (RenderExtractionSubsystem); the renderer keeps
    // its initial buffer size and DXGI stretches it, so proportions stay right if blurrier.
    connect(viewport_, &ViewportWidget::surfaceResized, this,
            [this](quint32 width, quint32 height) { engine_.ResizeSurface(width, height); });
    // Input first, so it's latched before anything reads it; the editor camera last, so it
    // replaces the scene camera that RenderExtractionSubsystem copied into the RenderWorld.
    viewport_->setInput(engine_.AddSubsystem<engine::InputSubsystem>());
    engine_.AddSubsystem<engine::RenderingSubsystem>();
    engine_.AddSubsystem<engine::RenderExtractionSubsystem>();
    camera_ = engine_.AddSubsystem<EditorCameraSubsystem>();
    engine_.AddSubsystem<SelectionRenderSubsystem>(context_->selection());

    // On failure keep the window up so the Log dock shows why, but don't tick.
    if (!engine_.Initialize()) {
        spdlog::critical("Engine failed to initialize, see log above");
        return;
    }
    openStartupScene();

    tick_timer_->start(16);
    refreshStatus();
}

void MainWindow::openStartupScene() {
    if (!project_.startupScene.empty()) {
        std::string error;
        if (context_->sceneDocument().open(engine::AssetPathResolver::ToAbsolute(engine::FromUtf8(project_.startupScene)),
                                           error)) {
            return;
        }
        spdlog::error("Startup scene '{}': {}", project_.startupScene, error);
    }
    setupDefaultScene();
}

void MainWindow::setupDefaultScene() {
    using DirectX::SimpleMath::Quaternion;
    using DirectX::SimpleMath::Vector3;
    engine::Scene& scene = engine_.GetScene();

    // No camera: the editor has its own viewpoint.
    const entt::entity sun = scene.CreateEntity("Sun");
    Vector3 sun_direction{-0.45f, -1.0f, -0.3f};
    sun_direction.Normalize();
    scene.GetTransform(sun).rotation = Quaternion::FromToRotation(Vector3::Forward, sun_direction);
    scene.AddComponent<engine::DirectionalLight>(sun, Vector3{1.0f, 0.95f, 0.85f}, 1.25f);
    scene.SetDirectionalLight(sun);

    const entt::entity ground = scene.CreateEntity("Ground");
    scene.GetTransform(ground).scale = Vector3{20.0f, 1.0f, 20.0f};
    scene.AddComponent<engine::MeshRenderer>(ground,
        engine_.GetSceneAssets().RequestMesh(engine::BuiltinAssets::PlaneMesh),
        engine_.GetSceneAssets().RequestMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance));
    context_->sceneDocument().markClean();
}

void MainWindow::refreshTitle() {
    const SceneDocument& document = context_->sceneDocument();
    // [*] shows the unsaved-changes mark set by setWindowModified().
    setWindowTitle(tr("%1[*] — %2 — SRI Editor").arg(document.displayName(), QString::fromStdString(project_.name)));
    setWindowModified(document.isDirty());
}

void MainWindow::adoptStartupScene() {
    const std::filesystem::path& file = context_->sceneDocument().filePath();
    if (file.empty() || context_->sceneDocument().isDirty()) {
        return;
    }
    // Keep a startup scene that still exists; one that was moved or deleted gets replaced.
    std::error_code ec;
    if (!project_.startupScene.empty() &&
        std::filesystem::exists(engine::AssetPathResolver::ToAbsolute(engine::FromUtf8(project_.startupScene)), ec)) {
        return;
    }
    const std::string name = engine::AssetPathResolver::ToName(file);
    if (!name.starts_with("game/")) {
        return;  // outside the project's content
    }
    project_.startupScene = name;
    if (std::string error; !project_.Save(error)) {
        spdlog::warn("Could not remember {} as the startup scene: {}", name, error);
        project_.startupScene.clear();
    }
}

void MainWindow::tickEngine() {
    engine_.Tick();
    refreshStatus();
}

void MainWindow::pickViewport(const QPointF position, const bool toggle) {
    if (!engine_.IsInitialized() || camera_ == nullptr || viewport_->width() <= 0 || viewport_->height() <= 0) return;
    const auto& camera = engine_.GetSubsystem<engine::RenderingSubsystem>()->GetWorld().camera;
    const auto view_projection = camera.GetViewMatrix() * camera.GetProjectionMatrix();
    const auto inverse = view_projection.Invert();
    const float x = static_cast<float>(2.0 * position.x() / viewport_->width() - 1.0);
    const float y = static_cast<float>(1.0 - 2.0 * position.y() / viewport_->height());
    // Reverse-Z: near is 1, far is 0. Qt's local coordinates are normalized against
    // its logical size, which cancels the physical-pixel scaling of the swap chain.
    const auto near_point = DirectX::SimpleMath::Vector3::Transform({x, y, 1.0f}, inverse);
    const auto far_point = DirectX::SimpleMath::Vector3::Transform({x, y, 0.0f}, inverse);
    std::vector<entt::entity> hits;
    for (const auto& hit : engine::RaycastAll(engine_.GetScene(), engine_.GetSceneAssets(),
                                              near_point, far_point - near_point, camera.farPlane)) {
        // The general scene query also returns authored Bounds without a drawable mesh.
        const auto* renderer = engine_.GetScene().TryGetComponent<engine::MeshRenderer>(hit.entity);
        if (renderer && engine_.GetSceneAssets().Peek(renderer->mesh) &&
            engine_.GetSceneAssets().Peek(renderer->materialInstance)) hits.push_back(hit.entity);
    }

    auto& selection = context_->selection();
    const bool same_position = std::abs(position.x() - last_pick_position_.x()) <= 3.0 &&
                               std::abs(position.y() - last_pick_position_.y()) <= 3.0;
    const bool same_camera = std::memcmp(&view_projection, &last_pick_view_projection_, sizeof(view_projection)) == 0;
    const bool cycle = has_last_pick_ && same_position && same_camera && hits == last_pick_hits_ &&
                       (toggle || selection.primary() == last_picked_);
    std::size_t index = 0;
    if (cycle) {
        const auto previous = std::find(hits.begin(), hits.end(), last_picked_);
        if (previous != hits.end()) index = (static_cast<std::size_t>(previous - hits.begin()) + 1) % hits.size();
    }
    last_pick_position_ = position;
    last_pick_view_projection_ = view_projection;
    last_pick_hits_ = hits;
    last_picked_ = hits.empty() ? entt::null : hits[index];
    has_last_pick_ = !hits.empty();
    if (toggle) {
        if (last_picked_ != entt::null) selection.toggle(last_picked_);
    } else {
        selection.select(last_picked_);
    }
}

void MainWindow::buildMenus() {
    auto* file_menu = context_->menu(tr("&File"));
    file_menu->addAction(tr("New &Project..."), this, [this] { switchProject(createProjectInteractively(this)); });
    file_menu->addAction(tr("&Open Project..."), this, [this] { switchProject(pickProjectFile(this)); });
    file_menu->addSeparator();
    SceneDocument& document = context_->sceneDocument();
    file_menu->addAction(tr("&New Scene..."), QKeySequence::New, this,
                         [this, &document] { document.createInteractive(this); });
    file_menu->addAction(tr("Open &Scene..."), QKeySequence::Open, this,
                         [this, &document] { document.openInteractive(this); });
    file_menu->addAction(tr("&Save Scene"), QKeySequence::Save, this,
                         [this, &document] { document.saveInteractive(this, false); });
    file_menu->addAction(tr("Save Scene &As..."), QKeySequence::SaveAs, this,
                         [this, &document] { document.saveInteractive(this, true); });
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
