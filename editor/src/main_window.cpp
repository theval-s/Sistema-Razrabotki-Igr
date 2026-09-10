#include <editor/main_window.hpp>

#include <QAction>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QString>

namespace editor {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("SRI Editor"));
    resize(1280, 720);

    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_);

    setCentralWidget(new QLabel(
        tr("Viewport."), this));

    buildMenus();
    refreshStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    auto* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->addAction(tr("E&xit"), this, &QWidget::close);

    auto* engine_menu = menuBar()->addMenu(tr("&Engine"));
}

void MainWindow::refreshStatus() {
    const auto version = QString::fromUtf8(engine::version().data(),
                                           static_cast<qsizetype>(engine::version().size()));
    status_label_->setText(tr("engine %1").arg(version));
}

}  // namespace editor
