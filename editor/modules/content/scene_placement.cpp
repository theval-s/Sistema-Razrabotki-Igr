#include "scene_placement.hpp"

#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QFile>
#include <QMimeData>
#include <QString>
#include <QWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <SimpleMath.h>
#include <engine/assets/RigAuthoring.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/animation/AnimationComponents.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/SceneQueries.hpp>

#include <editor/editor_camera.hpp>
#include <editor/asset_picker.hpp>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>
#include <editor/viewport_view.hpp>

#include "asset_operations.hpp"
#include "rig_workflow.hpp"

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

engine::AssetReference skeletonFor(const engine::CatalogEntry& mesh, const engine::AssetCatalog& catalog) {
    QFile file(editor::toQString(mesh.source));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    engine::SkinnedMeshDefinition descriptor;
    std::string error;
    if (!engine::ParseSkinnedMeshDefinition(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), descriptor, error)) return {};
    engine::AssetReference reference = descriptor.skeleton;
    // The catalog's current path wins after an asset move; loading stays asynchronous.
    if (const auto* entry = catalog.Find(reference.id, engine::CatalogAssetKind::Skeleton))
        reference.path = entry->path;
    return reference;
}

/// Legacy static imports use their neighbouring instance. Skinned rigs never infer ownership this way.
const engine::CatalogEntry* materialFor(const engine::AssetCatalog& catalog, const engine::CatalogEntry& mesh) {
    // Generic skinned descriptors have no implied ownership of neighbouring materials.
    if (mesh.metadata.kind == engine::CatalogAssetKind::SkinnedMesh)
        return catalog.FindPath(engine::BuiltinAssets::DefaultMaterialInstance);
    const QDir folder = QFileInfo(editor::toQString(mesh.source)).absoluteDir();
    for (const QString& file : folder.entryList({QStringLiteral("*.material-instance.json")}, QDir::Files, QDir::Name)) {
        if (const auto* entry = findEntry(catalog, folder.filePath(file), engine::CatalogAssetKind::MaterialInstance)) {
            return entry;
        }
    }
    return catalog.FindPath(engine::BuiltinAssets::DefaultMaterialInstance);
}

std::optional<engine::AssetReference> chooseSkinnedMaterial(editor::EditorContext& context,
                                                          const engine::AssetCatalog& catalog) {
    QDialog dialog(context.mainWindow());
    dialog.setWindowTitle(QObject::tr("Place Animated Rig"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(QObject::tr(
        "This rig has no owned material instance. Choose an existing material for this entity. "
        "The engine default is preselected when available; neighbouring files are not assigned automatically."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* picker = new editor::AssetPicker(catalog, engine::CatalogAssetKind::MaterialInstance,
                                           QObject::tr("Choose material…"));
    picker->selectPath(QString::fromUtf8(engine::BuiltinAssets::DefaultMaterialInstance));
    layout->addWidget(picker);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(picker->reference().has_value());
    QObject::connect(picker, &QComboBox::currentIndexChanged, &dialog, [picker, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(picker->reference().has_value());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || context.playSession().isPlaying()) return std::nullopt;
    return picker->reference();
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
        if (const auto* entry = findEntry(catalog, path, engine::CatalogAssetKind::SkinnedMesh)) return entry;
        return findEntry(catalog, path, engine::CatalogAssetKind::Mesh);
    }
    const QDir folder(path);
    for (const QString& file : folder.entryList({QStringLiteral("*.skinned-mesh.json")}, QDir::Files, QDir::Name)) {
        if (const auto* entry = findEntry(catalog, folder.filePath(file), engine::CatalogAssetKind::SkinnedMesh)) return entry;
    }
    // The name AssetPathResolver::ResolveMesh looks for in a static model folder.
    for (const QString& file : folder.entryList({QStringLiteral("model.*")}, QDir::Files, QDir::Name)) {
        if (const auto* entry = findEntry(catalog, folder.filePath(file), engine::CatalogAssetKind::Mesh)) {
            return entry;
        }
    }
    return nullptr;
}

int addToScene(editor::EditorContext& context, const QStringList& paths, const std::optional<Vector3> at) {
    if (context.playSession().isPlaying()) return 0;
    engine::AssetSystem& assets = context.engine().GetSceneAssets();
    const engine::AssetCatalog& catalog = assets.GetCatalog();
    // Copy entries: material selection opens a modal loop where a catalog rescan may occur.
    std::vector<engine::CatalogEntry> meshes;
    for (const QString& path : paths) {
        if (const auto* mesh = placeableMesh(catalog, path)) {
            meshes.push_back(*mesh);
        }
    }
    if (meshes.empty()) {
        return 0;
    }

    struct Placement {
        engine::CatalogEntry mesh;
        std::optional<engine::RigAssetGroup> group;
        engine::AssetReference material;
    };
    std::vector<Placement> placements;
    for (const auto& mesh : meshes) {
        Placement placement{mesh, std::nullopt, {}};
        if (mesh.metadata.kind == engine::CatalogAssetKind::SkinnedMesh) {
            engine::RigAssetGroup group;
            std::string error;
            if (selectedRig(context, editor::toQString(mesh.source), group, error)) placement.group = std::move(group);
            if (placement.group && placement.group->materialInstance) {
                if (const auto* owned = catalog.Find(placement.group->materialInstance->id, engine::CatalogAssetKind::MaterialInstance))
                    placement.material = {owned->metadata.id, owned->path};
            }
            if (!placement.material.id.IsValid()) {
                const auto chosen = chooseSkinnedMaterial(context, catalog);
                if (!chosen) return 0; // Cancel before any scene/history mutation.
                placement.material = *chosen;
            }
        } else if (const auto* material = materialFor(catalog, mesh)) {
            placement.material = {material->metadata.id, material->path};
        } else continue;
        placements.push_back(std::move(placement));
    }
    if (placements.empty() || context.playSession().isPlaying()) return 0;
    const editor::EditorCameraSubsystem* camera = context.camera();
    const Vector3 center = at ? *at : camera != nullptr ? camera->GetSpawnPoint() : Vector3::Zero;
    const Vector3 right = camera != nullptr ? camera->GetRight() : Vector3::Right;
    engine::Scene& scene = context.scene();
    std::vector<entt::entity> added;
    context.history().begin(placements.size() == 1 ? QObject::tr("Place Mesh") : QObject::tr("Place Meshes"));
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const engine::CatalogEntry& mesh = placements[i].mesh;
        const auto& group = placements[i].group;
        const engine::AssetReference mesh_reference{mesh.metadata.id, mesh.path};
        const auto& material_reference = placements[i].material;

        const entt::entity entity = scene.CreateEntity(entityName(mesh).toStdString());
        const float offset = (static_cast<float>(i) - static_cast<float>(placements.size() - 1) / 2.0f) * Spacing;
        engine::Transform transform = scene.GetTransform(entity);
        transform.position = center + right * offset;
        if (group) transform.scale = Vector3(group->placementScale);
        scene.SetTransform(entity, transform);
        // Asynchronous: the entity shows up once the mesh is loaded. The references are what gets saved.
        if (mesh.metadata.kind == engine::CatalogAssetKind::SkinnedMesh) {
            engine::SkinnedMeshRenderer renderer;
            renderer.mesh = assets.RequestSkinnedMesh(mesh_reference);
            renderer.materialInstance = assets.RequestMaterialInstance(material_reference);
            renderer.meshReference = mesh_reference;
            renderer.materialReference = material_reference;
            scene.AddComponent<engine::SkinnedMeshRenderer>(entity, std::move(renderer));
            engine::Animator animator;
            animator.skeleton = skeletonFor(mesh, catalog);
            if (group) {
                for (const auto& binding : group->clips) {
                    if (!binding.removed && catalog.Find(binding.asset.id, engine::CatalogAssetKind::AnimationClip)) {
                        animator.clip = binding.asset;
                        break;
                    }
                }
            }
            // Prefer an explicitly selected compatible clip, then a deterministic catalog default.
            const auto matchingClip = [&](const engine::CatalogEntry& entry) {
                QFile file(editor::toQString(entry.source));
                if (!file.open(QIODevice::ReadOnly)) return false;
                const auto bytes = file.readAll();
                engine::AnimationClipDefinition definition;
                std::string error;
                return engine::ParseAnimationClipDefinition(
                    std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), definition, error) &&
                    definition.skeleton.id == animator.skeleton.id;
            };
            bool selected_clip = false;
            for (const auto& path : paths) {
                if (const auto* entry = findEntry(catalog, path, engine::CatalogAssetKind::AnimationClip); entry && matchingClip(*entry)) {
                    animator.clip = {entry->metadata.id, entry->path};
                    selected_clip = true;
                    break;
                }
            }
            if (!group && !selected_clip) {
                for (const auto* entry : catalog.List(engine::CatalogAssetKind::AnimationClip)) {
                    if (matchingClip(*entry)) { animator.clip = {entry->metadata.id, entry->path}; break; }
                }
            }
            scene.AddComponent<engine::Animator>(entity, std::move(animator));
        } else if (!mesh.metadata.materials.empty()) {
            // Imported with its materials: every slot draws with its own.
            scene.AddComponent<engine::MeshRenderer>(entity, assets.RequestMesh(mesh_reference),
                                                     engine::AssetHandle<engine::MaterialInstanceAsset>{},
                                                     mesh_reference, engine::AssetReference{});
        } else {
            scene.AddComponent<engine::MeshRenderer>(entity, assets.RequestMesh(mesh_reference),
                                                     assets.RequestMaterialInstance(material_reference), mesh_reference,
                                                     material_reference);
        }
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
