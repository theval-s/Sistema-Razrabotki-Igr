#include "scene_placement.hpp"

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <utility>
#include <vector>

#include <SimpleMath.h>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>

#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/selection.hpp>

#include "asset_operations.hpp"

namespace content {

namespace {

using DirectX::SimpleMath::Vector3;

/// how far from camera pov
constexpr float SpawnDistance = 5.0f;
/// Farthest point on the ground (y = 0) that new entities are put on.
constexpr float MaxGroundDistance = 20.0f;
/// Between entities added together.
constexpr float Spacing = 1.5f;

const engine::CatalogEntry* findEntry(const engine::AssetCatalog& catalog, const QString& path,
                                      const engine::CatalogAssetKind kind) {
    const engine::CatalogEntry* entry = catalog.FindPath(assetName(path).toStdString());
    return entry != nullptr && entry->metadata.kind == kind ? entry : nullptr;
}

/// The first material instance next to mesh or the default one.
const engine::CatalogEntry* materialFor(const engine::AssetCatalog& catalog, const engine::CatalogEntry& mesh) {
    const QDir folder = QFileInfo(editor::toQString(mesh.source)).absoluteDir();
    for (const QString& file : folder.entryList({QStringLiteral("*.material-instance.json")}, QDir::Files, QDir::Name)) {
        if (const auto* entry = findEntry(catalog, folder.filePath(file), engine::CatalogAssetKind::MaterialInstance)) {
            return entry;
        }
    }
    return catalog.FindPath(engine::BuiltinAssets::DefaultMaterialInstance);
}

/// The model folder's name for its model.<ext>, the file's otherwise.
QString entityName(const engine::CatalogEntry& mesh) {
    const QFileInfo file(editor::toQString(mesh.source));
    const QString stem = splitAssetName(file.fileName(), false).first;
    return stem.compare(QLatin1String("model"), Qt::CaseInsensitive) == 0 ? file.dir().dirName() : stem;
}

/// Just some distance from POV or on the ground
std::pair<Vector3, Vector3> spawnPoint(editor::EditorContext& context) {
    const editor::EditorCameraSubsystem* camera = context.camera();
    if (camera == nullptr) {
        return {Vector3::Zero, Vector3::Right};
    }
    const Vector3 position = camera->GetPosition();
    const Vector3 forward = camera->GetForward();
    if (forward.y < 0.0f) {
        const float distance = -position.y / forward.y;
        if (distance > 0.0f && distance <= MaxGroundDistance) {
            return {position + forward * distance, camera->GetRight()};
        }
    }
    return {position + forward * SpawnDistance, camera->GetRight()};
}

}  // namespace

const engine::CatalogEntry* placeableMesh(const engine::AssetCatalog& catalog, const QString& path) {
    const QFileInfo info(path);
    if (!info.isDir()) {
        return findEntry(catalog, path, engine::CatalogAssetKind::Mesh);
    }
    // The name AssetPathResolver::ResolveMesh looks for in a model folder.
    const QDir folder(path);
    for (const QString& file : folder.entryList({QStringLiteral("model.*")}, QDir::Files, QDir::Name)) {
        if (const auto* entry = findEntry(catalog, folder.filePath(file), engine::CatalogAssetKind::Mesh)) {
            return entry;
        }
    }
    return nullptr;
}

int addToScene(editor::EditorContext& context, const QStringList& paths) {
    engine::AssetSystem& assets = context.engine().GetSceneAssets();
    const engine::AssetCatalog& catalog = assets.GetCatalog();
    std::vector<const engine::CatalogEntry*> meshes;
    for (const QString& path : paths) {
        if (const auto* mesh = placeableMesh(catalog, path)) {
            meshes.push_back(mesh);
        }
    }
    if (meshes.empty()) {
        return 0;
    }

    const auto [center, right] = spawnPoint(context);
    engine::Scene& scene = context.scene();
    std::vector<entt::entity> added;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const engine::CatalogEntry& mesh = *meshes[i];
        const engine::CatalogEntry* material = materialFor(catalog, mesh);
        if (material == nullptr) {
            continue;  // not even the default one: the catalog is broken, and its banner says so
        }
        const engine::AssetReference mesh_reference{mesh.metadata.id, mesh.path};
        const engine::AssetReference material_reference{material->metadata.id, material->path};

        const entt::entity entity = scene.CreateEntity(entityName(mesh).toStdString());
        const float offset = (static_cast<float>(i) - static_cast<float>(meshes.size() - 1) / 2.0f) * Spacing;
        scene.GetTransform(entity).position = center + right * offset;
        // Asynchronous: the entity shows up once the mesh is loaded. The references are what gets saved.
        scene.AddComponent<engine::MeshRenderer>(entity, assets.RequestMesh(mesh_reference),
                                                 assets.RequestMaterialInstance(material_reference), mesh_reference,
                                                 material_reference);
        added.push_back(entity);
    }
    context.selection().setEntities(added);
    return static_cast<int>(added.size());
}

}  // namespace content
