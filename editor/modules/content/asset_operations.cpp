#include "asset_operations.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLatin1String>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <spdlog/spdlog.h>

#include <engine/assets/AssetAuthoring.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/scene_document.hpp>

namespace content {

namespace {

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// Ids of the assets in `paths`, including everything inside the folders among them.
std::vector<engine::AssetUuid> idsIn(const QStringList& paths) {
    std::vector<engine::AssetUuid> ids;
    const auto add = [&ids](const std::filesystem::path& file) {
        engine::AssetMetadata metadata;
        std::string ignored;  // without a sidecar nothing can refer to it by id
        if (engine::CatalogKindForFile(file) && engine::ReadAssetMetadata(file, metadata, ignored)) {
            ids.push_back(metadata.id);
        }
    };
    for (const QString& path : paths) {
        const std::filesystem::path file = editor::toPath(path);
        std::error_code ec;
        if (!std::filesystem::is_directory(file, ec)) {
            add(file);
            continue;
        }
        for (auto it = std::filesystem::recursive_directory_iterator(
                 file, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                add(it->path());
            }
        }
    }
    return ids;
}

}  // namespace

QString cleanPath(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString assetName(const QString& path) {
    const std::string name = engine::AssetPathResolver::ToName(editor::toPath(path));
    return name.empty() ? QFileInfo(path).fileName() : fromUtf8(name);
}

std::pair<QString, QString> splitAssetName(const QString& name, const bool dir) {
    if (dir) {
        return {name, {}};
    }
    // The engine's multi-part suffixes (CatalogKindForFile).
    //TODO: add if we get more suffixes?
    for (const QLatin1String suffix : {QLatin1String(".material-instance.json"), QLatin1String(".material.json"),
                                       QLatin1String(".scene.json")}) {
        if (name.size() > suffix.size() && name.endsWith(suffix, Qt::CaseInsensitive)) {
            return {name.chopped(suffix.size()), name.right(suffix.size())};
        }
    }
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return {name, {}};  // ".gitignore" is all name
    }
    return {name.left(dot), name.mid(dot)};
}

bool isWithin(const QString& path, const QString& root) {
    // Windows paths are case-insensitive.
    return path.compare(root, Qt::CaseInsensitive) == 0 ||
           (path.size() > root.size() && path.startsWith(root, Qt::CaseInsensitive) &&
            path[root.size()] == QLatin1Char('/'));
}

AssetOperations::AssetOperations(editor::EditorContext& context, QWidget* parent)
    : context_(context), parent_(parent) {}

QStringList AssetOperations::transfer(const QStringList& paths, const QString& folder, const bool copy) {
    QStringList result;
    QStringList errors;
    const QString target_folder = cleanPath(folder);
    for (const QString& path : paths) {
        const QString source = cleanPath(path);
        const QFileInfo info(source);
        if (isWithin(target_folder, source)) {
            errors << tr("%1: can't go inside itself").arg(info.fileName());
            continue;
        }
        if (!copy && cleanPath(info.absolutePath()).compare(target_folder, Qt::CaseInsensitive) == 0) {
            continue;  // already there
        }
        const QString target = copy ? uniquePath(target_folder, info.fileName(), info.isDir())
                                    : target_folder + QLatin1Char('/') + info.fileName();
        std::string error;
        const bool done = copy ? engine::CopyAsset(editor::toPath(source), editor::toPath(target), error)
                               : engine::MoveAsset(editor::toPath(source), editor::toPath(target), error);
        if (!done) {
            errors << QStringLiteral("%1: %2").arg(info.fileName(), fromUtf8(error));
            continue;
        }
        if (!copy) {
            followOpenScene(source, target);
        }
        result << target;
    }
    report(copy ? tr("Copy") : tr("Move"), errors);
    if (!result.isEmpty()) {
        rescan();
    }
    return result;
}

QStringList AssetOperations::duplicate(const QStringList& paths) {
    QStringList result;
    for (const QString& path : paths) {
        result << transfer({path}, QFileInfo(cleanPath(path)).absolutePath(), true);
    }
    return result;
}

QString AssetOperations::rename(const QString& path, const QString& name) {
    const QString source = cleanPath(path);
    const QFileInfo info(source);
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == info.fileName()) {
        return {};
    }
    if (trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('\\'))) {
        report(tr("Rename"), {tr("%1: a name can't contain slashes").arg(trimmed)});
        return {};
    }
    const QString target = cleanPath(info.absolutePath()) + QLatin1Char('/') + trimmed;
    std::string error;
    if (!engine::MoveAsset(editor::toPath(source), editor::toPath(target), error)) {
        report(tr("Rename"), {QStringLiteral("%1: %2").arg(info.fileName(), fromUtf8(error))});
        return {};
    }
    followOpenScene(source, target);
    rescan();
    return target;
}

bool AssetOperations::remove(const QStringList& paths) {
    QStringList sources;
    for (const QString& path : paths) {
        sources << cleanPath(path);
    }
    if (sources.isEmpty()) {
        return false;
    }
    const QString scene = openScene();
    for (const QString& source : sources) {
        if (!scene.isEmpty() && isWithin(scene, source)) {
            const QString text = scene.size() == source.size()
                                     ? tr("%1 is the open scene. Open another scene first.")
                                     : tr("%1 holds the open scene. Open another scene first.");
            QMessageBox::information(parent_, tr("Delete"), text.arg(assetName(source)));
            return false;
        }
    }

    // Files that would keep referring to what's deleted, apart from those deleted with it.
    QStringList users;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const std::filesystem::path content = context_.project().ContentDir();
    for (const engine::AssetUuid& id : idsIn(sources)) {
        for (const std::filesystem::path& user : engine::FindAssetReferences(content, id)) {
            const QString file = cleanPath(editor::toQString(user));
            const bool deleted =
                std::ranges::any_of(sources, [&file](const QString& source) { return isWithin(file, source); });
            if (const QString name = assetName(file); !deleted && !users.contains(name)) {
                users << name;
            }
        }
    }
    QGuiApplication::restoreOverrideCursor();

    QMessageBox box(QMessageBox::Question, tr("Delete"),
                    sources.size() == 1
                        ? tr("Move %1 to the Recycle Bin?").arg(assetName(sources.front()))
                        : tr("Move %n item(s) to the Recycle Bin?", nullptr, static_cast<int>(sources.size())),
                    QMessageBox::Cancel, parent_);
    const QPushButton* confirm = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    if (!users.isEmpty()) {
        users.sort(Qt::CaseInsensitive);
        constexpr qsizetype Listed = 8;
        QString listed = users.mid(0, Listed).join(QLatin1Char('\n'));
        if (users.size() > Listed) {
            listed += QLatin1Char('\n') + tr("and %n more", nullptr, static_cast<int>(users.size() - Listed));
            box.setDetailedText(users.join(QLatin1Char('\n')));
        }
        box.setIcon(QMessageBox::Warning);
        box.setInformativeText(tr("Still used by:\n%1\n\nTheir references will be missing.").arg(listed));
    }
    box.exec();
    if (box.clickedButton() != confirm) {
        return false;
    }

    // The source and its sidecar go to the bin separately: restore both to get the asset back.
    QStringList errors;
    for (const QString& source : sources) {
        const bool dir = QFileInfo(source).isDir();
        if (!QFile::moveToTrash(source)) {
            errors << tr("%1: could not move it to the Recycle Bin").arg(assetName(source));
            continue;
        }
        const QString sidecar = source + QLatin1String(".meta");
        if (!dir && QFileInfo::exists(sidecar) && !QFile::moveToTrash(sidecar)) {
            errors << tr("%1: deleted, but its .meta was not").arg(assetName(source));
        }
    }
    report(tr("Delete"), errors);
    rescan();
    return errors.isEmpty();
}

QString AssetOperations::importModel(const ModelImport& import, const QString& folder) {
    const QString command = tr("Import Model");
    const QString name = import.name.trimmed();
    const QString target = cleanPath(folder) + QLatin1Char('/') + name;
    if (QFileInfo::exists(target)) {
        report(command, {tr("%1 already exists").arg(assetName(target))});
        return {};
    }
    if (!QDir().mkdir(target)) {
        report(command, {tr("Could not create %1").arg(QDir::toNativeSeparators(target))});
        return {};
    }

    QString error;
    // Copies `from` into the new folder as `file`, with a meta when it's an asset.
    const auto bring = [&](const QString& from, const QString& file, const engine::AssetMetadata* metadata) {
        const QString to = target + QLatin1Char('/') + file;
        if (QFile source(from); !source.copy(to)) {
            error = QStringLiteral("%1: %2").arg(QFileInfo(from).fileName(), source.errorString());
            return false;
        }
        std::string written;
        if (metadata != nullptr && !engine::WriteAssetMetadata(editor::toPath(to), *metadata, written)) {
            error = fromUtf8(written);
            return false;
        }
        return true;
    };

    const QFileInfo source(import.source);
    // The name AssetPathResolver::ResolveMesh looks for in a model folder.
    const engine::AssetMetadata mesh{engine::NewUuid(), engine::CatalogAssetKind::Mesh, import.center_on_load};
    bool done = bring(import.source, QStringLiteral("model.") + source.suffix().toLower(), &mesh);
    // Files the model refers to by name, so they keep theirs: an .obj's materials, a .gltf's buffers.
    for (const QLatin1String extension : {QLatin1String("mtl"), QLatin1String("bin")}) {
        const QFileInfo companion(source.absolutePath() + QLatin1Char('/') + source.completeBaseName() +
                                  QLatin1Char('.') + extension);
        if (done && companion.isFile()) {
            done = bring(companion.filePath(), companion.fileName(), nullptr);
        }
    }

    const engine::AssetMetadata texture{engine::NewUuid(), engine::CatalogAssetKind::Texture};
    const QString texture_file = QStringLiteral("texture.") + QFileInfo(import.texture).suffix().toLower();
    if (done && !import.texture.isEmpty()) {
        done = bring(import.texture, texture_file, &texture);
    }

    if (done && import.material) {
        engine::MaterialInstanceDefinition definition;
        definition.version = 2;
        definition.material = import.material->path;
        definition.materialReference = import.material;
        if (!import.texture.isEmpty()) {
            // A path hint keeps its case, unlike asset names.
            const QString content = cleanPath(editor::toQString(context_.project().ContentDir()));
            const std::string hint =
                (QStringLiteral("game/") + QDir(content).relativeFilePath(target + QLatin1Char('/') + texture_file))
                    .toStdString();
            definition.parameters.albedoTexture = hint;
            definition.parameters.albedoReference = engine::AssetReference{texture.id, hint};
        }
        const std::filesystem::path file = editor::toPath(target + QLatin1Char('/') + name +
                                                          QLatin1String(".material-instance.json"));
        std::string text;
        std::string written;
        done = engine::SerializeMaterialInstanceDefinition(definition, text, written) &&
               engine::CreateNewFile(file, text + '\n', written) &&
               engine::WriteAssetMetadata(file, {engine::NewUuid(), engine::CatalogAssetKind::MaterialInstance},
                                          written);
        if (!done) {
            error = fromUtf8(written);
        }
    }

    if (!done) {
        QDir(target).removeRecursively();  // nothing half-imported stays behind
        report(command, {error});
        return {};
    }
    rescan();
    return target;
}

bool AssetOperations::createMaterial(const QString& path, const engine::MaterialDefinition& definition) {
    std::string text;
    if (std::string error; !engine::SerializeMaterialDefinition(definition, text, error)) {
        report(tr("New Material"), {fromUtf8(error)});
        return false;
    }
    return createFile(tr("New Material"), path, text, engine::CatalogAssetKind::Material);
}

bool AssetOperations::createMaterialInstance(const QString& path,
                                              const engine::MaterialInstanceDefinition& definition) {
    std::string text;
    if (std::string error; !engine::SerializeMaterialInstanceDefinition(definition, text, error)) {
        report(tr("New Material Instance"), {fromUtf8(error)});
        return false;
    }
    return createFile(tr("New Material Instance"), path, text, engine::CatalogAssetKind::MaterialInstance);
}

bool AssetOperations::updateMaterial(const QString& path, const engine::MaterialDefinition& definition) {
    std::string text;
    if (std::string error; !engine::SerializeMaterialDefinition(definition, text, error)) {
        report(tr("Edit Material"), {fromUtf8(error)});
        return false;
    }
    return updateFile(tr("Edit Material"), path, text, engine::CatalogAssetKind::Material);
}

bool AssetOperations::updateMaterialInstance(const QString& path,
                                             const engine::MaterialInstanceDefinition& definition) {
    std::string text;
    if (std::string error; !engine::SerializeMaterialInstanceDefinition(definition, text, error)) {
        report(tr("Edit Material Instance"), {fromUtf8(error)});
        return false;
    }
    if (!updateFile(tr("Edit Material Instance"), path, text, engine::CatalogAssetKind::MaterialInstance)) return false;
    const std::string name = assetName(path).toStdString();
    QStringList errors;
    for (engine::AssetSystem* assets : {&context_.engine().GetAssets(), &context_.engine().GetSceneAssets()}) {
        std::string error;
        if (!assets->RefreshMaterialInstance(name, definition, error)) errors << fromUtf8(error);
    }
    report(tr("Edit Material Instance"), errors);
    return errors.isEmpty();
}

void AssetOperations::adoptNewFiles() {
    std::size_t generated = 0;
    std::string error;
    if (!engine::GenerateMissingAssetMetadata(context_.project().ContentDir(), generated, error)) {
        spdlog::warn("Could not create metadata for new content files: {}", error);
    } else if (generated > 0) {
        spdlog::info("Created metadata for {} new content file(s)", generated);
    }
    rescan();
}

void AssetOperations::rescan() {
    std::string error;  // the catalog keeps it, for the browser's banner
    (void)context_.engine().RescanAssetCatalogs(error);
}

QString AssetOperations::uniquePath(const QString& folder, const QString& name, const bool dir) {
    const auto taken = [](const QString& path) {
        return QFileInfo::exists(path) || QFileInfo::exists(path + QLatin1String(".meta"));
    };
    QString path = folder + QLatin1Char('/') + name;
    if (!taken(path)) {
        return path;
    }
    auto [stem, suffix] = splitAssetName(name, dir);
    // A copy of "rock (2)" becomes "rock (3)", not "rock (2) (1)".
    static const QRegularExpression numbered(QStringLiteral(R"(^(.*) \((\d+)\)$)"));
    int number = 1;
    if (const QRegularExpressionMatch match = numbered.match(stem); match.hasMatch()) {
        stem = match.captured(1);
        number = match.captured(2).toInt() + 1;
    }
    do {
        path = folder + QLatin1Char('/') + stem + QStringLiteral(" (%1)").arg(number++) + suffix;
    } while (taken(path));
    return path;
}

void AssetOperations::report(const QString& command, const QStringList& errors) const {
    if (errors.isEmpty()) {
        return;
    }
    for (const QString& error : errors) {
        spdlog::warn("{}: {}", command.toStdString(), error.toStdString());
    }
    QMessageBox::warning(parent_, command, errors.join(QLatin1Char('\n')));
}

bool AssetOperations::createFile(const QString& command, const QString& path, const std::string& text,
                                 const engine::CatalogAssetKind kind) {
    const std::filesystem::path file = editor::toPath(path);
    std::string error;
    if (!engine::CreateNewFile(file, text + '\n', error)) {
        report(command, {fromUtf8(error)});
        return false;
    }
    if (!engine::WriteAssetMetadata(file, {engine::NewUuid(), kind}, error)) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        report(command, {fromUtf8(error)});
        return false;
    }
    rescan();
    return true;
}

bool AssetOperations::updateFile(const QString& command, const QString& path, const std::string& text,
                                 const engine::CatalogAssetKind kind) {
    const std::filesystem::path file = editor::toPath(path);
    const auto* entry = context_.engine().GetAssets().GetCatalog().FindPath(assetName(path).toStdString());
    if (entry == nullptr || entry->metadata.kind != kind || entry->source != file ||
        !isWithin(path, cleanPath(editor::toQString(context_.project().ContentDir())))) {
        report(command, {tr("This asset is not an editable project asset.")});
        return false;
    }
    std::string error;
    if (!engine::WriteFileAtomic(file, text + '\n', error)) {
        report(command, {fromUtf8(error)});
        return false;
    }
    rescan();
    return true;
}

void AssetOperations::followOpenScene(const QString& from, const QString& to) {
    const QString scene = openScene();
    if (!scene.isEmpty() && isWithin(scene, from)) {
        context_.sceneDocument().relocate(editor::toPath(to + scene.mid(from.size())));
    }
}

QString AssetOperations::openScene() const {
    const std::filesystem::path& file = context_.sceneDocument().filePath();
    return file.empty() ? QString() : cleanPath(editor::toQString(file));
}

}  // namespace content
