#include <editor/main_window.hpp>

#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QString>
#include <QTimer>

#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <editor/viewport_widget.hpp>

namespace editor {

namespace {

QString toQString(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

engine::EngineConfig makeEngineConfig() {
    engine::EngineConfig config;
    config.applicationName = "SRI Editor";
    return config;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), engine_(makeEngineConfig()) {
    setWindowTitle(tr("SRI Editor"));
    resize(1280, 720);

    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_);

    viewport_ = new ViewportWidget(this);
    setCentralWidget(viewport_);

    buildLogDock();  // before engine init, so its startup messages show up
    buildMenus();

    // The editor owns the loop: the engine only advances when we Tick() it.
    engine_.SetSurface(viewport_->surface());
    connect(viewport_, &ViewportWidget::surfaceResized, this,
            [this](quint32 width, quint32 height) { engine_.ResizeSurface(width, height); });
    tick_timer_ = new QTimer(this);
    tick_timer_->setTimerType(Qt::PreciseTimer);
    connect(tick_timer_, &QTimer::timeout, this, &MainWindow::tickEngine);

    // On failure keep the window up so the Log dock shows why, but don't tick.
    if (engine_.Initialize()) {
        tick_timer_->start(16);
    } else {
        spdlog::critical("Engine failed to initialize, see log above");
    }

    refreshStatus();
}

MainWindow::~MainWindow() {
    std::erase(spdlog::default_logger()->sinks(), log_sink_);
}

void MainWindow::tickEngine() {
    engine_.Tick();
    refreshStatus();
}

void MainWindow::buildMenus() {
    auto* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->addAction(tr("E&xit"), this, &QWidget::close);

    auto* engine_menu = menuBar()->addMenu(tr("&Engine"));
    auto* pause_action = engine_menu->addAction(tr("&Pause simulation"));
    pause_action->setCheckable(true);
    connect(pause_action, &QAction::toggled, this, [this](bool paused) { engine_.SetPaused(paused); });

    auto* view_menu = menuBar()->addMenu(tr("&View"));
    if (auto* log_dock = findChild<QDockWidget*>(QStringLiteral("LogDock"))) {
        view_menu->addAction(log_dock->toggleViewAction());
    }
}

void MainWindow::buildLogDock() {
    log_view_ = new QPlainTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(5000);

    auto* dock = new QDockWidget(tr("Log"), this);
    dock->setObjectName(QStringLiteral("LogDock"));
    dock->setWidget(log_view_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

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
