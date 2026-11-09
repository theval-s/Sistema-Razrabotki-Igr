#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

#include <editor/shortcut_registry.hpp>

class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace editor {

/// Editor-wide settings. Shortcut edits apply and save at once, so there's only Close.
class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(ShortcutRegistry& shortcuts, QWidget* parent = nullptr);

private:
    [[nodiscard]] QWidget* buildShortcutsPage();
    /// Empty while a category is selected.
    [[nodiscard]] QString currentId() const;
    /// Which other commands share the shortcuts of `id`; empty if none.
    [[nodiscard]] QString conflictText(const QString& id) const;
    void loadEdits();
    void applyEdits();
    void refreshRows();
    /// The selected command's Reset and conflict warning.
    void refreshCurrent();
    void applyFilter(const QString& text);
    void resetAll();

    ShortcutRegistry& shortcuts_;
    /// For titles, categories and defaults; the current shortcuts come from the registry.
    QHash<QString, ShortcutRegistry::Command> commands_;
    QTreeWidget* tree_ = nullptr;
    QKeySequenceEdit* primary_ = nullptr;
    QKeySequenceEdit* alternate_ = nullptr;
    QPushButton* reset_ = nullptr;
    QPushButton* reset_all_ = nullptr;
    QWidget* conflict_ = nullptr;
    QLabel* conflict_text_ = nullptr;
};

}  // namespace editor
