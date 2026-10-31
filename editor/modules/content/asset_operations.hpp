#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <utility>

#include <engine/assets/AssetCatalog.hpp>

class QWidget;

namespace engine {
struct MaterialDefinition;
struct MaterialInstanceDefinition;
}  // namespace engine

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

/// A model to import into a folder of its own, laid out as the engine looks a model folder up:
/// model.<ext>, texture.<ext> and optionally <name>.material-instance.json using the texture.
struct ModelImport {
    QString source;          // the model file
    QString name;            // of the new folder
    bool center_on_load = true;
    QString texture;         // image to bring along; empty for none
    std::optional<engine::AssetReference> material;  // base of the material instance; none for no instance
};

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
    /// Creates the folder `folder`/`import.name` with the model in it. Returns the folder, or empty on failure.
    QString importModel(const ModelImport& import, const QString& folder);
    /// Write a new .material.json / .material-instance.json at `path`, with its meta. False on failure.
    bool createMaterial(const QString& path, const engine::MaterialDefinition& definition);
    bool createMaterialInstance(const QString& path, const engine::MaterialInstanceDefinition& definition);
    /// Replaces an existing definition without touching its metadata/UUID.
    bool updateMaterial(const QString& path, const engine::MaterialDefinition& definition);
    bool updateMaterialInstance(const QString& path, const engine::MaterialInstanceDefinition& definition);
    /// Adds metas and rescans
    void adoptNewFiles();
    void rescan();

    /// `folder`/`name`, or with " (1)", " (2)", ... before the asset suffix if that is taken.
    [[nodiscard]] static QString uniquePath(const QString& folder, const QString& name, bool dir);

private:
    /// Shows the errors of one command, if any.
    void report(const QString& command, const QStringList& errors) const;
    /// Writes `text` to the new file `path` and gives it a meta of `kind`, then rescans.
    bool createFile(const QString& command, const QString& path, const std::string& text,
                    engine::CatalogAssetKind kind);
    bool updateFile(const QString& command, const QString& path, const std::string& text,
                    engine::CatalogAssetKind kind);
    /// Keeps the scene document on the open scene after `from` (it, or a folder holding it) moved to `to`.
    void followOpenScene(const QString& from, const QString& to);
    [[nodiscard]] QString openScene() const;

    editor::EditorContext& context_;
    QWidget* parent_;
};

}  // namespace content
