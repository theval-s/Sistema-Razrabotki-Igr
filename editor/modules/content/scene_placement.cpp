#include "scene_placement.hpp"

#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <vector>

#include <SimpleMath.h>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/SceneQueries.hpp>

#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>
#include <editor/viewport_view.hpp>

#include "asset_operations.hpp"

namespace content {

namespace {

using DirectX::SimpleMath::Vector3;

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

int addToScene(editor::EditorContext& context, const QStringList& paths, const std::optional<Vector3> at) {
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

    const editor::EditorCameraSubsystem* camera = context.camera();
    const Vector3 center = at ? *at : camera != nullptr ? camera->GetSpawnPoint() : Vector3::Zero;
    const Vector3 right = camera != nullptr ? camera->GetRight() : Vector3::Right;
    engine::Scene& scene = context.scene();
    std::vector<entt::entity> added;
    context.history().begin(meshes.size() == 1 ? QObject::tr("Place Mesh") : QObject::tr("Place Meshes"));
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
        engine::Transform transform = scene.GetTransform(entity);
        transform.position = center + right * offset;
        scene.SetTransform(entity, transform);
        // Asynchronous: the entity shows up once the mesh is loaded. The references are what gets saved.
        scene.AddComponent<engine::MeshRenderer>(entity, assets.RequestMesh(mesh_reference),
                                                 assets.RequestMaterialInstance(material_reference), mesh_reference,
                                                 material_reference);
        added.push_back(entity);
    }
    context.selection().setEntities(added);
    context.history().end();
    return static_cast<int>(added.size());
}

ViewportDropTarget::ViewportDropTarget(editor::EditorContext& context, QObject* parent)
    : QObject(parent), context_(context) {
    context.viewport()->setAcceptDrops(true);
    context.viewport()->installEventFilter(this);
}

bool ViewportDropTarget::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto* drag = static_cast<QDragEnterEvent*>(event);
        paths_.clear();
        if (const QMimeData* data = drag->mimeData();
            !context_.playSession().isPlaying() && data->hasFormat(editor::AssetPathsMimeType)) {
            paths_ = QString::fromUtf8(data->data(editor::AssetPathsMimeType)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            const engine::AssetCatalog& catalog = context_.engine().GetSceneAssets().GetCatalog();
            if (std::ranges::none_of(paths_, [&](const QString& path) { return placeableMesh(catalog, path) != nullptr; })) {
                paths_.clear();
            }
        }
        [[fallthrough]];
    }
    case QEvent::DragMove: {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        // A copy: the browser would delete the files after a move.
        drag->setDropAction(Qt::CopyAction);
        drag->setAccepted(!paths_.isEmpty());
        return true;
    }
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        if (paths_.isEmpty()) return false;
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        addToScene(context_, paths_, dropPoint(drop->position()));
        context_.viewport()->setFocus(Qt::OtherFocusReason);
        paths_.clear();
        return true;
    }
    default:
        return QObject::eventFilter(watched, event);
    }
}

std::optional<Vector3> ViewportDropTarget::dropPoint(const QPointF position) const {
    const auto view = context_.viewportView();
    if (!view) return std::nullopt;
    const auto ray = view->ray({static_cast<float>(position.x()), static_cast<float>(position.y())});
    // World bounds, not triangles: a model lands on the box of what's under the cursor.
    std::optional<float> nearest;
    for (const engine::RayHit& hit : engine::RaycastAll(context_.engine().GetScene(), context_.engine().GetSceneAssets(),
                                                         ray.origin, ray.direction, ray.length)) {
        if (hit.distance > 0.0f && (!nearest || hit.distance < *nearest)) nearest = hit.distance;
    }
    return nearest ? ray.origin + ray.direction * *nearest
                   : editor::EditorCameraSubsystem::SpawnPoint(ray.origin, ray.direction);
}

}  // namespace content
