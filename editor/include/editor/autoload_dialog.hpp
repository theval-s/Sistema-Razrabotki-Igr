#pragma once

#include <QDialog>

#include <vector>

#include <engine/scripting/AutoloadDefinition.hpp>

class QTableWidget;
class QPushButton;

namespace engine {
class Engine;
struct Project;
}

namespace editor {

/// Edits a copy. The host commits it to Project only after a successful disk save.
class AutoloadDialog final : public QDialog {
    Q_OBJECT

public:
    AutoloadDialog(engine::Engine& engine, const engine::Project& project, QWidget* parent = nullptr);
    [[nodiscard]] std::vector<engine::AutoloadDefinition> definitions() const;
    void accept() override;

private:
    void addScript();
    void removeSelected();
    void moveSelected(int offset);
    void refreshRows(int selected);
    void refreshButtons();

    engine::Engine& engine_;
    const engine::Project& project_;
    std::vector<engine::AutoloadDefinition> definitions_;
    QTableWidget* table_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
};

}  // namespace editor
