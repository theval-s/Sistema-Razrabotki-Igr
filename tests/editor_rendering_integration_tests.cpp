#include <editor/main_window.hpp>
#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/log_model.hpp>
#include <editor/log_panel.hpp>
#include <editor/paths.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>
#include <editor/viewport_widget.hpp>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/Subsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scene/Scene.hpp>

#include <DockWidget.h>
#include <DockManager.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <gtest/gtest.h>

namespace editor {
// Access only host orchestration; no mocked renderer lifecycle or status snapshot.
class MainWindowTestAccess {
public:
    static engine::Engine& engine(MainWindow& window) { return window.engine_; }
    static EditorContext& context(MainWindow& window) { return *window.context_; }
    static void initializeWithSurface(MainWindow& window, engine::NativeSurface surface) {
        window.wireEngine();
        window.engine_.SetSurface(surface);
        window.initializeEngine();
    }
    static bool pendingSurface(const MainWindow& window) { return window.startup_pending_surface_; }
    static ads::CDockWidget* logDock(MainWindow& window) {
        return window.dock_manager_->findDockWidget(QStringLiteral("LogDock"));
    }
    static LogPanel* logPanel(MainWindow& window) { return window.log_panel_; }
};
} // namespace editor

namespace {
using editor::MainWindowTestAccess;

bool screenshotsRequested() { return !qEnvironmentVariable("SRI_RENDERING_SCREENSHOT_DIR").isEmpty(); }

void captureNativeWindow(editor::MainWindow& window, const QString& filename) {
    const QString directory = qEnvironmentVariable("SRI_RENDERING_SCREENSHOT_DIR");
    ASSERT_FALSE(directory.isEmpty());
    ASSERT_NE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    ASSERT_TRUE(QDir().mkpath(directory));
    window.show();
    window.raise();
    window.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    // Windows can deny foreground activation to a CTest child process. Keep
    // this opt-in capture unobscured without recreating the viewport HWND.
    struct RestoreZOrder {
        HWND window;
        ~RestoreZOrder() { SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    } restore{hwnd};
    ASSERT_TRUE(SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW));
    QTest::qWait(200); // Let native presentation and the desktop compositor catch up.
    ASSERT_NE(window.screen(), nullptr);
    // Capture the composited desktop region occupied by the native window.
    // Capturing the parent HWND's DC directly can omit flip-model DXGI child
    // surfaces even though they are visible on screen.
    const QPoint origin = window.mapToGlobal(QPoint(0, 0));
    const QPoint center = window.mapToGlobal(window.rect().center());
    const qreal dpr = window.devicePixelRatioF();
    const POINT nativeCenter{qRound(center.x() * dpr), qRound(center.y() * dpr)};
    ASSERT_EQ(GetAncestor(WindowFromPoint(nativeCenter), GA_ROOT), hwnd)
        << "Editor is obscured; refusing to capture another application's desktop content";
    const auto image = window.screen()->grabWindow(0, origin.x(), origin.y(), window.width(), window.height());
    ASSERT_FALSE(image.isNull());
    const QString path = QDir(directory).filePath(filename);
    ASSERT_TRUE(image.save(path, "PNG")) << path.toStdString();
}

class StartupCounter final : public engine::Subsystem {
public:
    std::string_view GetName() const override { return "Editor integration startup counter"; }
    bool Initialize() override { ++initializations; return true; }
    void Update(const engine::FrameContext&) override { ++updates; }
    int initializations = 0;
    int updates = 0;
};

class EditorRenderingIntegration : public testing::Test {
protected:
    QTemporaryDir directory;
    std::filesystem::path previousGameRoot = engine::AssetPathResolver::GetGameRoot();
    engine::Project project;

    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        QSettings().clear();
        std::string error;
        const auto created = engine::Project::Create(editor::toPath(directory.path()), "Rendering integration", error);
        ASSERT_TRUE(created) << error;
        project = *created;
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());
    }
    void TearDown() override { engine::AssetPathResolver::SetGameRoot(previousGameRoot); }

    void duplicateMetadata() {
        const engine::AssetUuid uuid("11111111-1111-4111-8111-111111111111");
        for (const char* name : {"first.obj", "second.obj"}) {
            const auto path = project.ContentDir() / name;
            QFile file(editor::toQString(path));
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
            file.write("# Catalog fixture; never imported\n");
            file.close();
            std::string error;
            ASSERT_TRUE(engine::WriteAssetMetadata(path, {uuid, engine::CatalogAssetKind::Mesh}, error)) << error;
        }
    }
};

TEST_F(EditorRenderingIntegration, StatusAndExistingControlsAreSiblingsOutsideNativeViewport) {
    editor::MainWindow window(project);
    auto* viewport = window.findChild<editor::ViewportWidget*>();
    auto* status = window.findChild<QWidget*>(QStringLiteral("renderingStatusBar"));
    auto* selector = window.findChild<QComboBox*>(QStringLiteral("renderDebugView"));
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(status->parentWidget(), viewport->parentWidget());
    EXPECT_FALSE(viewport->isAncestorOf(status));
    EXPECT_FALSE(viewport->isAncestorOf(selector));
    EXPECT_TRUE(viewport->findChildren<QWidget*>().empty());
    for (const auto* name : {"showSkeletonOverlay", "showAudioOverlay"}) {
        auto* control = window.findChild<QCheckBox*>(QString::fromUtf8(name));
        ASSERT_NE(control, nullptr);
        EXPECT_FALSE(viewport->isAncestorOf(control));
    }
    EXPECT_FALSE(selector->isEnabled());
    EXPECT_EQ(selector->count(), 4);
    for (int index = 0; index < selector->count(); ++index) {
        EXPECT_FALSE(selector->itemData(index, Qt::ToolTipRole).toString().isEmpty());
        EXPECT_TRUE(selector->itemData(index, Qt::ToolTipRole).toString().contains(QStringLiteral("Final")));
    }
}

TEST_F(EditorRenderingIntegration, FatalStartupShowsBannerAndUnfilteredLogBeforeAnyTickAndNeverRetriesOnResize) {
    duplicateMetadata();
    ASSERT_FALSE(HasFatalFailure());
    editor::MainWindow window(project);
    auto& engine = MainWindowTestAccess::engine(window);
    auto* counter = engine.AddSubsystem<StartupCounter>();
    ASSERT_NE(counter, nullptr);
    // A real missing-surface failure, requiring neither GPU availability nor a
    // damaged required shader. Exercise the same initialization method as the slot.
    MainWindowTestAccess::initializeWithSurface(window, {});
    auto* rendering = engine.GetSubsystem<engine::RenderingSubsystem>();
    ASSERT_NE(rendering, nullptr);
    const auto diagnostic = rendering->GetDiagnostics();
    ASSERT_EQ(diagnostic.lifecycle, engine::RenderingLifecycle::Failed);
    ASSERT_TRUE(diagnostic.failure);
    EXPECT_FALSE(engine.IsInitialized());
    EXPECT_EQ(counter->initializations, 1);
    EXPECT_EQ(counter->updates, 0);
    EXPECT_FALSE(MainWindowTestAccess::pendingSurface(window));
    auto* timer = window.findChild<QTimer*>(QStringLiteral("engineTickTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_FALSE(timer->isActive());
    auto* summary = window.findChild<QLabel*>(QStringLiteral("renderingStatusSummary"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().startsWith(QStringLiteral("Renderer failed:")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("ValidateSurface")));

    const auto& catalogError = engine.GetSceneAssets().GetCatalogError();
    ASSERT_FALSE(catalogError.empty());
    // ADS's DockAreaLayout detaches inactive tab widgets from the QWidget tree.
    // Its dock registry remains authoritative when another tab (Content) is current.
    auto* dock = MainWindowTestAccess::logDock(window);
    auto* panel = MainWindowTestAccess::logPanel(window);
    ASSERT_NE(dock, nullptr);
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(dock->widget(), panel);
    dock->toggleView(false);
    auto* filter = panel->findChild<editor::LogFilterModel*>();
    ASSERT_NE(filter, nullptr);
    filter->setLevelShown(spdlog::level::err, false);
    filter->setLevelShown(spdlog::level::critical, false);
    filter->setSourceShown(QStringLiteral("Render"), false);
    panel->findChild<QLineEdit*>()->setText(QStringLiteral("hiding the startup failure"));
    auto* showLog = window.findChild<QPushButton*>(QStringLiteral("showRenderingLog"));
    ASSERT_NE(showLog, nullptr);
    showLog->click(); // Actual MainWindow action -> existing dock -> existing panel.
    EXPECT_FALSE(dock->isClosed());
    EXPECT_TRUE(filter->isLevelShown(spdlog::level::err));
    EXPECT_TRUE(filter->isLevelShown(spdlog::level::critical));
    EXPECT_TRUE(filter->isSourceShown(QStringLiteral("Render")));
    EXPECT_TRUE(panel->findChild<QLineEdit*>()->text().isEmpty());
    auto* details = panel->findChild<QPlainTextEdit*>();
    ASSERT_NE(details, nullptr);
    EXPECT_FALSE(details->isHidden());
    EXPECT_TRUE(details->toPlainText().contains(QStringLiteral("Operation: ValidateSurface")));
    EXPECT_TRUE(details->toPlainText().contains(QString::fromUtf8(diagnostic.failure->detail.c_str())));
    EXPECT_TRUE(details->toPlainText().contains(QString::fromUtf8(catalogError.c_str())));

    auto* viewport = window.findChild<editor::ViewportWidget*>();
    ASSERT_NE(viewport, nullptr);
    viewport->surfaceResized(640, 480);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "startEngine", Qt::DirectConnection));
    EXPECT_EQ(counter->initializations, 1);
    EXPECT_EQ(counter->updates, 0);
    EXPECT_FALSE(timer->isActive());
    EXPECT_EQ(rendering->GetDiagnostics().failure->detail, diagnostic.failure->detail);
    if (screenshotsRequested()) {
        ASSERT_NO_FATAL_FAILURE(captureNativeWindow(window, QStringLiteral("editor-fatal-startup.png")));
        EXPECT_TRUE(panel->isVisible());
        EXPECT_TRUE(summary->isVisible());
        EXPECT_FALSE(timer->isActive());
        EXPECT_EQ(counter->updates, 0);
    }
}

TEST_F(EditorRenderingIntegration, SuspendedStartupRecoversOnceAndSelectorIsTransient) {
    editor::MainWindow window(project);
    auto* viewport = window.findChild<editor::ViewportWidget*>();
    ASSERT_NE(viewport, nullptr);
    const auto surface = viewport->surface();
    ASSERT_NE(surface.windowHandle, nullptr);
    // Use the same native HWND/probe pattern as viewport_overlay_tests. Failure
    // UI tests above remain deterministic even on a machine without graphics.
    {
        engine::RenderingContext probe;
        try {
            if (!probe.Initialize({surface.windowHandle, 320, 240}, false))
                GTEST_SKIP() << "Native D3D11/swap-chain initialization unavailable";
        } catch (const engine::RenderingException& error) {
            GTEST_SKIP() << "Native D3D11/swap-chain initialization unavailable: " << error.what();
        }
    }
    auto& engine = MainWindowTestAccess::engine(window);
    auto* counter = engine.AddSubsystem<StartupCounter>();
    ASSERT_NE(counter, nullptr);
    auto& context = MainWindowTestAccess::context(window);
    QSignalSpy opened(&context.sceneDocument(), &editor::SceneDocument::opened);
    MainWindowTestAccess::initializeWithSurface(window, {surface.windowHandle, 0, 240});
    auto* rendering = engine.GetSubsystem<engine::RenderingSubsystem>();
    ASSERT_NE(rendering, nullptr);
    ASSERT_EQ(rendering->GetDiagnostics().lifecycle, engine::RenderingLifecycle::SurfaceUnavailable);
    EXPECT_TRUE(MainWindowTestAccess::pendingSurface(window));
    EXPECT_FALSE(engine.IsInitialized());
    EXPECT_EQ(counter->initializations, 1);
    auto* timer = window.findChild<QTimer*>(QStringLiteral("engineTickTimer"));
    auto* selector = window.findChild<QComboBox*>(QStringLiteral("renderDebugView"));
    auto* summary = window.findChild<QLabel*>(QStringLiteral("renderingStatusSummary"));
    ASSERT_NE(timer, nullptr);
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(summary, nullptr);
    EXPECT_FALSE(timer->isActive());
    EXPECT_FALSE(selector->isEnabled());
    EXPECT_TRUE(summary->text().contains(QStringLiteral("suspended")));
    viewport->surfaceResized(0, 0);
    EXPECT_EQ(counter->initializations, 1);

    // No tick and no manual engine initialization: the real viewport resize
    // connection must restart initialization, preserving registered objects.
    viewport->surfaceResized(640, 480);
    ASSERT_TRUE(engine.IsInitialized()) << summary->text().toStdString();
    EXPECT_EQ(engine.GetSubsystem<engine::RenderingSubsystem>(), rendering);
    EXPECT_NE(engine.GetSubsystem<editor::EditorCameraSubsystem>(), nullptr);
    EXPECT_EQ(counter->initializations, 2);
    EXPECT_EQ(counter->updates, 0);
    EXPECT_FALSE(MainWindowTestAccess::pendingSurface(window));
    EXPECT_TRUE(timer->isActive());
    EXPECT_TRUE(selector->isEnabled());
    ASSERT_EQ(opened.count(), 1);
    timer->stop(); // Keep inspection assertions strictly before the first tick.
    const bool dirty = context.sceneDocument().isDirty();
    const bool playing = context.playSession().isPlaying();
    const auto revision = engine.GetScene().GetRevision();
    for (const auto mode : {engine::RenderDebugView::Albedo, engine::RenderDebugView::Normals,
                            engine::RenderDebugView::Depth, engine::RenderDebugView::Final}) {
        selector->setCurrentIndex(selector->findData(static_cast<int>(mode)));
        EXPECT_EQ(rendering->GetDebugView(), mode);
        EXPECT_EQ(context.sceneDocument().isDirty(), dirty);
        EXPECT_EQ(context.playSession().isPlaying(), playing);
        EXPECT_EQ(engine.GetScene().GetRevision(), revision);
        EXPECT_EQ(selector->toolTip(), selector->currentData(Qt::ToolTipRole).toString());
    }
    viewport->surfaceResized(0, 0);
    viewport->surfaceResized(640, 480);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "startEngine", Qt::DirectConnection));
    EXPECT_EQ(counter->initializations, 2);
    EXPECT_EQ(opened.count(), 1);
    EXPECT_FALSE(timer->isActive()); // Completion is not run again by resize/start.
}

TEST_F(EditorRenderingIntegration, NativeScreenshotsWhenRequested) {
    if (!screenshotsRequested()) GTEST_SKIP() << "Set SRI_RENDERING_SCREENSHOT_DIR to capture the native editor";
    ASSERT_NE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    editor::MainWindow window(project);
    window.show();
    window.raise();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    auto& engine = MainWindowTestAccess::engine(window);
    QElapsedTimer wait;
    wait.start();
    engine::RenderingSubsystem* rendering = nullptr;
    bool settled = false;
    while (wait.elapsed() < 15000) {
        QTest::qWait(25); // Actual MainWindow timer, engine ticks and native Present.
        rendering = engine.GetSubsystem<engine::RenderingSubsystem>();
        if (!engine.IsInitialized() || !rendering) continue;
        ASSERT_EQ(rendering->GetDiagnostics().lifecycle, engine::RenderingLifecycle::Ready);
        settled = !rendering->GetWorld().sceneItems.empty();
        auto& assets = engine.GetSceneAssets();
        engine.GetScene().ForEach<engine::MeshRenderer>([&](auto, const auto& renderer) {
            settled &= assets.GetState(renderer.mesh) == engine::AssetState::Ready &&
                       assets.GetState(renderer.materialInstance) == engine::AssetState::Ready;
        });
        if (settled && engine.GetFrame().frameIndex > 2) break;
    }
    ASSERT_TRUE(engine.IsInitialized());
    ASSERT_NE(rendering, nullptr);
    ASSERT_TRUE(settled) << "Temporary project's default plane never reached submitted/Ready state";
    auto* selector = window.findChild<QComboBox*>(QStringLiteral("renderDebugView"));
    ASSERT_NE(selector, nullptr);
    ASSERT_TRUE(selector->isEnabled());
    const std::pair<engine::RenderDebugView, const char*> modes[] = {
        {engine::RenderDebugView::Final, "editor-final.png"},
        {engine::RenderDebugView::Albedo, "editor-albedo.png"},
        {engine::RenderDebugView::Normals, "editor-normals.png"},
        {engine::RenderDebugView::Depth, "editor-depth.png"},
        {engine::RenderDebugView::Final, "editor-final-restored.png"},
    };
    for (const auto& [mode, filename] : modes) {
        selector->setCurrentIndex(selector->findData(static_cast<int>(mode)));
        const auto frame = engine.GetFrame().frameIndex;
        wait.restart();
        while (engine.GetFrame().frameIndex <= frame + 2 && wait.elapsed() < 5000) QTest::qWait(20);
        ASSERT_GT(engine.GetFrame().frameIndex, frame + 2);
        ASSERT_EQ(rendering->GetDiagnostics().lifecycle, engine::RenderingLifecycle::Ready);
        ASSERT_EQ(rendering->GetDebugView(), mode);
        ASSERT_FALSE(rendering->GetWorld().sceneItems.empty());
        ASSERT_TRUE(selector->isVisible());
        ASSERT_NO_FATAL_FAILURE(captureNativeWindow(window, QString::fromLatin1(filename)));
    }
}
} // namespace

int main(int argc, char** argv) {
    // Keep a native Windows Qt platform: these tests exercise the actual HWND
    // hierarchy, not an offscreen stand-in. No optional external project is used.
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SRI.Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("editor_rendering_integration_tests"));
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
