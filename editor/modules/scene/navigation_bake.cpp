#include "navigation_bake.hpp"

#include <QObject>

#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/navigation/NavigationMesh.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/editor_log.hpp>
#include <editor/paths.hpp>
#include <editor/scene_history.hpp>

namespace {

using Region = engine::NavigationRegion;

const QLatin1String SceneSuffix(".scene.json");
const QLatin1String Extension(".navmesh");

/// Who a .navmesh belongs to; nullopt if it can't be read.
std::optional<engine::EntityUuid> ownerOf(const std::filesystem::path& file) {
    std::vector<std::uint8_t> data;
    std::string error;
    engine::EntityUuid owner;
    engine::NavigationMesh mesh;
    if (!engine::ReadBinaryFile(file, data, error) || !mesh.Deserialize(data, owner, error)) return std::nullopt;
    return owner;
}

/// "<scene name>.", which starts the name of every bake of that scene.
QString bakePrefix(const std::filesystem::path& scene_file) {
    QString name = editor::toQString(scene_file.filename());
    if (name.endsWith(SceneSuffix, Qt::CaseInsensitive)) name.chop(SceneSuffix.size());
    return name + QLatin1Char('.');
}

/// The entity name with what can't go in a file name replaced by '_'.
QString fileNamePart(const std::string& name) {
    QString part = QString::fromStdString(name);
    for (QChar& c : part)
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_')) c = QLatin1Char('_');
    return part.isEmpty() ? QStringLiteral("NavMesh") : part;
}

}  // namespace

std::filesystem::path navigationBakeFile(engine::Engine& engine, const std::filesystem::path& scene_file,
                                         const entt::entity region) {
    const engine::Scene& scene = engine.GetScene();
    const auto id = scene.GetId(region);
    const auto folder = scene_file.parent_path();
    const QString prefix = bakePrefix(scene_file);
    // An unreadable file is no loss: no scene can load it.
    const auto usable = [&](const std::filesystem::path& file) {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) return true;
        const auto owner = ownerOf(file);
        return !owner || *owner == id;
    };

    // Another scene's file may hold the same region after a Save As, and a copied region keeps the reference.
    if (const auto* current = scene.TryGetComponent<Region>(region); current && current->navMesh.id.IsValid()) {
        const auto* entry = engine.GetSceneAssets().GetCatalog().Find(current->navMesh.id, engine::CatalogAssetKind::NavMesh);
        std::error_code ec;
        if (entry && std::filesystem::equivalent(entry->source.parent_path(), folder, ec) &&
            editor::toQString(entry->source.filename()).startsWith(prefix, Qt::CaseInsensitive) && usable(entry->source))
            return entry->source;
    }
    const QString name = prefix + fileNamePart(scene.GetName(region));
    if (auto file = folder / editor::toPath(name + Extension); usable(file)) return file;
    const auto suffix = QString::fromStdString(id.ToString().substr(0, 8));
    return folder / editor::toPath(name + QLatin1Char('.') + suffix + Extension);
}

bool bakeNavigation(engine::Engine& engine, editor::SceneHistory& history, const std::filesystem::path& scene_file,
                    const entt::entity region, std::string& error) {
    if (scene_file.empty()) {
        error = "Save the scene first: the NavMesh is saved next to it";
        return false;
    }
    if (engine::AssetPathResolver::ToName(scene_file).empty()) {
        error = "The scene is outside the project content, so its NavMesh can't be saved";
        return false;
    }
    if (!engine.BuildNavigation(region, error)) return false;
    const auto file = navigationBakeFile(engine, scene_file, region);
    if (!engine.SaveNavigation(region, file, error)) return false;

    // A file baked before keeps its id, so references to it stay valid.
    engine::AssetMetadata metadata;
    if (std::string ignored; !engine::ReadAssetMetadata(file, metadata, ignored) ||
                             metadata.kind != engine::CatalogAssetKind::NavMesh) {
        metadata = {engine::NewUuid(), engine::CatalogAssetKind::NavMesh};
        if (!engine::WriteAssetMetadata(file, metadata, error)) return false;
    }
    if (std::string rescan; !engine.RescanAssetCatalogs(rescan)) editor::editorLog().warn("Asset catalog: {}", rescan);

    // The catalog keeps the file name's case; ToName() lowercases it.
    const auto* entry = engine.GetSceneAssets().GetCatalog().Find(metadata.id, engine::CatalogAssetKind::NavMesh);
    const engine::AssetReference reference{metadata.id, entry ? entry->path : engine::AssetPathResolver::ToName(file)};
    engine::Scene& scene = engine.GetScene();
    if (const auto* current = std::as_const(scene).TryGetComponent<Region>(region); current && current->navMesh != reference) {
        history.edit(QObject::tr("Bake NavMesh"), [&] {
            scene.Modify<Region>(region, [&](Region& changed) { changed.navMesh = reference; });
        });
    }
    error.clear();
    return true;
}

QString navigationBakeStatus(engine::Engine& engine, const entt::entity region) {
    const auto* current = std::as_const(engine.GetScene()).TryGetComponent<Region>(region);
    if (current == nullptr) return {};
    const auto* mesh = engine.GetNavigationMesh(region);
    const auto& reference = current->navMesh;
    if (!reference.id.IsValid()) return mesh ? QObject::tr("Not saved: bake again") : QObject::tr("Not baked");

    const auto* entry = engine.GetSceneAssets().GetCatalog().Find(reference.id, engine::CatalogAssetKind::NavMesh);
    const QString name = entry ? editor::toQString(entry->source.filename())
                               : QString::fromStdString(reference.path.empty() ? reference.id.ToString() : reference.path)
                                     .section(QLatin1Char('/'), -1);
    if (entry == nullptr) return QObject::tr("%1 is missing: bake again").arg(name);
    if (mesh == nullptr) return QObject::tr("%1 isn't loaded: bake again").arg(name);
    if (mesh->GetSettings() != current->settings) return QObject::tr("%1 is out of date: bake again").arg(name);
    return name;
}
