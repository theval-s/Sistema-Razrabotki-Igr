#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <utility>

class QWidget;

namespace editor {
class EditorContext;
}

namespace content {

/// Absolute path with '/' separators and no "." or ".." parts, as QFileSystemModel reports paths.
[[nodiscard]] QString cleanPath(const QString& path);

/// The asset name of `path` ("game/models/rock.png"), or its file name outside the asset roots.
[[nodiscard]] QString assetName(const QString& path);

/// Returns {the actual name, the suffix}, since we have long suffixes
[[nodiscard]] std::pair<QString, QString> splitAssetName(const QString& name, bool dir);

/// Whether `path` is `root` or lies inside it. Both absolute and '/'-separated.
[[nodiscard]] bool isWithin(const QString& path, const QString& root);

/// The content browser's file commands. We need custom ones to move both meta files and asset files
class AssetOperations {
    Q_DECLARE_TR_FUNCTIONS(content::AssetOperations)

public:
    AssetOperations(editor::EditorContext& context, QWidget* parent);

    /// Moves, or copies, each of `paths` into `folder`. A copy whose name is taken gets a number.
    /// Returns the new paths.
    QStringList transfer(const QStringList& paths, const QString& folder, bool copy);
    /// Copies each of `paths` next to itself. Returns the new paths.
    QStringList duplicate(const QStringList& paths);
    /// Renames `path` within its folder. Returns the new path, or empty if nothing changed.
    QString rename(const QString& path, const QString& name);
    /// Move to trash, warn if it's still referenced
    bool remove(const QStringList& paths);
    /// Adds metas and rescans
    void adoptNewFiles();
    void rescan();

    /// `folder`/`name`, or with " (1)", " (2)", ... before the asset suffix if that is taken.
    [[nodiscard]] static QString uniquePath(const QString& folder, const QString& name, bool dir);

private:
    /// Shows the errors of one command, if any.
    void report(const QString& command, const QStringList& errors) const;
    /// Keeps the scene document on the open scene after `from` (it, or a folder holding it) moved to `to`.
    void followOpenScene(const QString& from, const QString& to);
    [[nodiscard]] QString openScene() const;

    editor::EditorContext& context_;
    QWidget* parent_;
};

}  // namespace content
