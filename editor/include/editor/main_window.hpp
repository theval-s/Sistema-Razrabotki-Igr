#pragma once

#include <QMainWindow>

#include <engine/engine.hpp>

class QLabel;

namespace editor {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:

private:
    void buildMenus();
    void refreshStatus();

    engine::Engine engine_;
    QLabel* status_label_ = nullptr;
};

}  // namespace editor
