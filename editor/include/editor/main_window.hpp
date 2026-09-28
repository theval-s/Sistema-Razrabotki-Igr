#pragma once

#include <QMainWindow>

#include <memory>

#include <engine/engine.hpp>

class QLabel;
class QPlainTextEdit;
class QTimer;

namespace spdlog::sinks {
class sink;
}

namespace editor {

class ViewportWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void tickEngine();

private:
    void buildMenus();
    void buildLogDock();
    void refreshStatus();
    void appendLog(const QString& level, const QString& text);

    engine::Engine engine_;
    std::shared_ptr<spdlog::sinks::sink> log_sink_;

    ViewportWidget* viewport_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* tick_timer_ = nullptr;
};

}  // namespace editor
