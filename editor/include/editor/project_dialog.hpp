#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;

namespace editor {

/// Recently opened .sriproject files, most recent first; files that are gone are left out.
[[nodiscard]] QStringList recentProjects();
void addRecentProject(const QString& file);

/// Asks for a parent folder and a name and creates <folder>/<name>/<name>.sriproject.
/// Returns the project file; empty if cancelled or failed (failures are reported).
[[nodiscard]] QString createProjectInteractively(QWidget* parent);
/// Empty if cancelled.
[[nodiscard]] QString pickProjectFile(QWidget* parent);

/// Shown when the editor starts without --project: recent projects, New, Open.
class ProjectDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProjectDialog(QWidget* parent = nullptr);

    /// The chosen .sriproject; set once exec() returned Accepted.
    [[nodiscard]] QString projectFile() const;

private:
    void choose(const QString& file);

    QListWidget* recent_ = nullptr;
    QString file_;
};

}  // namespace editor
