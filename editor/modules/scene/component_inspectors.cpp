#include <QMessageBox>
#include "component_inspectors.hpp"

#include <QAbstractItemView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QObject>
#include <QSignalBlocker>
#include <QSettings>
#include <QStringList>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QStandardItemModel>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QListView>
#include <QStyle>
#include <QVBoxLayout>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/core/type_info.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/RigAuthoring.hpp>
#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/audio/AudioSubsystem.hpp>
#include <engine/engine.hpp>
#include <engine/physics/PhysicsComponents.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/asset_picker.hpp>
#include <editor/editor_context.hpp>
#include <editor/scene_document.hpp>
#include <editor/scene_events.hpp>
#include <editor/scene_history.hpp>

#include "navigation_bake.hpp"
#include "property_form.hpp"
#include "../audio/audio_probe.hpp"

namespace {

using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector3;
template <class T>
using Binding = PropertyForm::Binding<T>;

// I hate templates but we've got to...
/// get from TryGetComponent as const, and set as scene.Modify
template <class T, class M>
Binding<M> field(engine::Scene& scene, const entt::entity entity, M T::*member) {
    Binding<M> binding;
    binding.get = [&scene, entity, member]() -> const M* {
        const T* component = std::as_const(scene).TryGetComponent<T>(entity);
        return component != nullptr ? &(component->*member) : nullptr;
    };
    binding.set = [&scene, entity, member](const M& value) {
        scene.Modify<T>(entity, [&](T& component) { component.*member = value; });
    };
    return binding;
}

/// same, one level deeper: `inner` of the component's `outer`, e.g. &Bounds::local then &Aabb::min.
/// but yes this shit looks awful
template <class T, class O, class I, class M>
Binding<M> field(engine::Scene& scene, const entt::entity entity, O T::*outer, M I::*inner) {
    Binding<M> binding;
    binding.get = [&scene, entity, outer, inner]() -> const M* {
        const T* component = std::as_const(scene).TryGetComponent<T>(entity);
        return component != nullptr ? &(component->*outer.*inner) : nullptr;
    };
    binding.set = [&scene, entity, outer, inner](const M& value) {
        scene.Modify<T>(entity, [&](T& component) { component.*outer.*inner = value; });
    };
    return binding;
}

/// Same for the transform because it's not done through Modify
template <class M>
Binding<M> transformField(engine::Scene& scene, const entt::entity entity, M engine::Transform::*member) {
    Binding<M> binding;
    binding.get = [&scene, entity, member]() -> const M* {
        return scene.IsValid(entity) ? &(std::as_const(scene).GetTransform(entity).*member) : nullptr;
    };
    binding.set = [&scene, entity, member](const M& value) {
        if (!scene.IsValid(entity)) return;
        engine::Transform transform = scene.GetTransform(entity);
        transform.*member = value;
        scene.SetTransform(entity, transform);
    };
    return binding;
}

/// The cube's mesh file, as the catalog lists it: BuiltinAssets::CubeMesh names its folder.
constexpr const char* BuiltinCubeFile = "engine/primitives/cube/model.obj";

constexpr PropertyForm::NumberFormat Intensity{.step = 0.1, .decimals = 2, .minimum = 0.0, .maximum = 1.0e4};
constexpr PropertyForm::NumberFormat Range{.step = 0.5, .decimals = 2, .minimum = 0.01, .maximum = 1.0e4};
/// Spot half angles, in degrees: under 90 or the cone isn't one.
constexpr PropertyForm::NumberFormat HalfAngle{.step = 1.0, .decimals = 1, .minimum = 0.0, .maximum = 89.9};

void buildTransform(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addVec3(QObject::tr("Position"), transformField(scene, entity, &engine::Transform::position));
    form.addRotation(QObject::tr("Rotation"), transformField(scene, entity, &engine::Transform::rotation));
    form.addScale(QObject::tr("Scale"), transformField(scene, entity, &engine::Transform::scale),
                  {.minimum = 0.0});
}

/// Comma-separated. Tags go through Scene, which counts them as structural changes.
void buildTags(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    auto* edit = new QLineEdit;
    edit->setPlaceholderText(QObject::tr("tag, another tag"));
    QObject::connect(edit, &QLineEdit::editingFinished, edit, [&context, &scene, entity, edit] {
        if (!scene.IsValid(entity)) {
            return;
        }
        std::vector<std::string> wanted;
        for (const QString& tag : edit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (const QString trimmed = tag.trimmed(); !trimmed.isEmpty()) {
                wanted.push_back(trimmed.toStdString());
            }
        }
        // A copy: removing tags changes what GetTags() points at.
        const auto current = scene.GetTags(entity);
        const std::vector<std::string> had(current.begin(), current.end());
        context.history().edit(QObject::tr("Change Tags"), [&] {
            for (const std::string& tag : had) {
                if (std::find(wanted.begin(), wanted.end(), tag) == wanted.end()) {
                    scene.RemoveTag(entity, tag);
                }
            }
            for (const std::string& tag : wanted) {
                scene.AddTag(entity, tag);  // ignores ones it has
            }
        });
    });
    form.addRow(QObject::tr("Tags"), edit, [&scene, entity, edit] {
        if (edit->hasFocus() || !scene.IsValid(entity)) {
            return;
        }
        QStringList tags;
        for (const std::string& tag : scene.GetTags(entity)) {
            tags.push_back(QString::fromStdString(tag));
        }
        if (const QString text = tags.join(QStringLiteral(", ")); text != edit->text()) {
            const QSignalBlocker blocker(edit);
            edit->setText(text);
        }
    });
}

/// The asset a renderer field uses: the authored one, which a save writes, or else the loaded one.
template <class Asset>
std::optional<engine::AssetReference> usedAsset(const engine::AssetSystem& assets,
                                                const engine::AssetReference& authored,
                                                const engine::AssetHandle<Asset> handle) {
    if (authored.id.IsValid()) {
        return authored;
    }
    return assets.ReferenceFor(handle);
}

/// A picker for one renderer asset field. `get` returns its authored reference and loaded handle,
/// `set` stores a choice, and `request` loads a chosen asset into the scene's registry. A non-empty
/// `none` names an entry that leaves the field empty; `change` is the undo step's text.
template <class Component, class Asset, class Get, class Set>
void addAssetPicker(PropertyForm& form, editor::EditorContext& context, const entt::entity entity, const QString& label,
                    const QString& change, const engine::CatalogAssetKind kind, Get get, Set set,
                    engine::AssetHandle<Asset> (engine::AssetSystem::*request)(const engine::AssetReference&),
                    const QString& none = {}) {
    engine::Scene& scene = context.scene();
    engine::Engine& engine = context.engine();
    auto* picker = new editor::AssetPicker(engine.GetSceneAssets().GetCatalog(), kind, none);
    picker->setMinimumContentsLength(12);  // the dock is narrow; the popup shows whole paths

    // Only the user's choices get here: refresh picks quietly.
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker,
                     [&context, &scene, &engine, entity, change, picker, kind, get, set, request, none] {
                         const auto* renderer = std::as_const(scene).TryGetComponent<Component>(entity);
                         const std::optional<engine::AssetReference> chosen = picker->reference();
                         const bool cleared = !chosen && !none.isEmpty() && picker->currentIndex() == 0;
                         if (renderer == nullptr || (!chosen && !cleared)) {
                             return;
                         }
                         const auto [reference, handle] = get(*renderer);
                         if (chosen ? chosen->id == reference.id : !reference.id.IsValid() && !handle) {
                             return;
                         }
                         context.history().edit(change, [&] {
                             // Asynchronous, like adding the component: the entity isn't drawn until it's loaded.
                             const auto loaded = chosen ? (engine.GetSceneAssets().*request)(*chosen)
                                                        : engine::AssetHandle<Asset>{};
                             scene.Modify<Component>(entity, [&](Component& changed) {
                                 set(changed, loaded, chosen.value_or(engine::AssetReference{}));
                             });
                             if (kind == engine::CatalogAssetKind::MaterialInstance) {
                                 engine.ClearEntityMaterialOverride(entity);
                             }
                         });
                     });

    // What the picker last showed, so it's only touched when the field changes elsewhere.
    auto shown = std::make_shared<std::string>();
    form.addRow(label, picker, [&scene, &engine, entity, picker, get, shown, none] {
        const auto* renderer = scene.TryGetComponent<Component>(entity);
        if (renderer == nullptr || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) {
            return;
        }
        const auto [reference, handle] = get(*renderer);
        const std::optional<engine::AssetReference> used = usedAsset(engine.GetSceneAssets(), reference, handle);
        std::string key = used ? used->id.ToString() + '\n' + used->path : std::string("none");
        if (key == *shown) {
            return;
        }
        *shown = std::move(key);
        const QSignalBlocker blocker(picker);
        if (used) {
            if (picker->selectId(used->id)) {
                return;
            }
            picker->reload();  // it may have been imported since the list was read
            if (picker->selectId(used->id)) {
                return;
            }
        }
        picker->setCurrentIndex(!used && !none.isEmpty() ? 0 : -1);
        picker->lineEdit()->setPlaceholderText(
            used ? QObject::tr("%1 (missing)").arg(QString::fromStdString(used->path.empty() ? used->id.ToString() : used->path))
                 : QObject::tr("None"));
    });
}

/// addAssetPicker() for an asset whose `reference` and `handle` are members of the component.
template <class Component, class Asset>
void addRendererAsset(PropertyForm& form, editor::EditorContext& context, const entt::entity entity,
                      const QString& label, const engine::CatalogAssetKind kind,
                      engine::AssetReference Component::*reference, engine::AssetHandle<Asset> Component::*handle,
                      engine::AssetHandle<Asset> (engine::AssetSystem::*request)(const engine::AssetReference&),
                      const QString& none = {}) {
    addAssetPicker<Component, Asset>(
        form, context, entity, label, QObject::tr("Change %1").arg(label), kind,
        [reference, handle](const Component& renderer) { return std::pair{renderer.*reference, renderer.*handle}; },
        [reference, handle](Component& renderer, const engine::AssetHandle<Asset> loaded,
                            const engine::AssetReference& chosen) {
            renderer.*handle = loaded;
            renderer.*reference = chosen;
            if constexpr (std::is_same_v<Component, engine::SkinnedMeshRenderer>) renderer.posedBounds.reset();
            if constexpr (std::is_same_v<Asset, engine::MeshAsset>) renderer.slotMaterials.clear();  // the old mesh's slots
        },
        request, none);
}

/// A material's file name without its extension, for short labels.
QString materialName(const std::optional<engine::AssetReference>& material) {
    if (!material) return QObject::tr("none");
    QString name = QString::fromStdString(material->path).section(QLatin1Char('/'), -1);
    if (const QLatin1String extension(".material-instance.json"); name.endsWith(extension)) name.chop(extension.size());
    return name.isEmpty() ? QString::fromStdString(material->id.ToString()) : name;
}

/// The Mesh Renderer's material slots, a picker each, whose empty entry names what the slot falls
/// back to. Hidden for a mesh with one slot and no slot material: Material does the same there.
void addSlotMaterials(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    // A heading of its own across the form, with the rows below it, indented.
    auto* panel = new QWidget;
    auto* section = new QVBoxLayout(panel);
    section->setContentsMargins(0, 0, 0, 0);
    section->addWidget(new QLabel(QObject::tr("Material slots:")));
    auto* rows = new QVBoxLayout;
    rows->setContentsMargins(panel->style()->pixelMetric(QStyle::PM_LayoutLeftMargin), 0, 0, 0);
    section->addLayout(rows);
    // The rows' form and the labels it was built with; built again when they change.
    struct Shown {
        PropertyForm* form = nullptr;
        QStringList labels;
    };
    auto shown = std::make_shared<Shown>();
    form.addRow(panel, [&context, entity, panel, rows, shown] {
        const auto* renderer = context.scene().TryGetComponent<engine::MeshRenderer>(entity);
        if (renderer == nullptr) {
            return;
        }
        engine::AssetSystem& assets = context.engine().GetSceneAssets();
        const auto mesh = assets.Peek(renderer->mesh);
        const std::size_t named = mesh ? mesh->materialSlots.size() : 0;
        std::size_t count = named;  // and the materials of slots the mesh doesn't have, so they can be cleared
        for (std::size_t slot = named; slot < renderer->slotMaterials.size(); ++slot) {
            if (engine::HasOwnMaterial(renderer->slotMaterials[slot])) count = slot + 1;
        }
        const bool any = std::ranges::any_of(renderer->slotMaterials,
                                             [](const engine::SlotMaterial& slot) { return engine::HasOwnMaterial(slot); });
        panel->setVisible(named > 1 || any);

        QStringList labels;  // each slot's name, then each slot's empty entry
        for (std::size_t slot = 0; slot < count; ++slot) {
            const QString name = slot < named ? QString::fromStdString(mesh->materialSlots[slot]) : QString();
            labels << (!name.isEmpty() ? name
                       : slot < named || !mesh ? QObject::tr("Slot %1").arg(slot)
                                               : QObject::tr("Slot %1 (not in the mesh)").arg(slot));
        }
        for (std::size_t slot = 0; slot < count; ++slot) {
            if (engine::HasOwnMaterial(*renderer)) {
                labels << QObject::tr("(Material: %1)")
                              .arg(materialName(usedAsset(assets, renderer->materialReference, renderer->materialInstance)));
            } else if (mesh && slot < mesh->materials.size()) {
                labels << QObject::tr("(Default: %1)").arg(materialName(assets.ReferenceFor(mesh->materials[slot])));
            } else {
                labels << QObject::tr("(None)");
            }
        }
        if (shown->form == nullptr || labels != shown->labels) {
            if (shown->form != nullptr) {
                shown->form->hide();
                shown->form->deleteLater();
            }
            auto* slot_form = new PropertyForm;
            for (std::size_t slot = 0; slot < count; ++slot) {
                const QString& name = labels[static_cast<qsizetype>(slot)];
                addAssetPicker<engine::MeshRenderer, engine::MaterialInstanceAsset>(
                    *slot_form, context, entity, name, QObject::tr("Change %1 Material").arg(name),
                    engine::CatalogAssetKind::MaterialInstance,
                    [slot](const engine::MeshRenderer& component) {
                        return slot < component.slotMaterials.size()
                                   ? std::pair{component.slotMaterials[slot].materialReference,
                                               component.slotMaterials[slot].materialInstance}
                                   : std::pair<engine::AssetReference, engine::AssetHandle<engine::MaterialInstanceAsset>>{};
                    },
                    [slot](engine::MeshRenderer& changed, const engine::AssetHandle<engine::MaterialInstanceAsset> loaded,
                           const engine::AssetReference& chosen) {
                        if (changed.slotMaterials.size() <= slot) changed.slotMaterials.resize(slot + 1);
                        changed.slotMaterials[slot] = {loaded, chosen};
                        while (!changed.slotMaterials.empty() && !engine::HasOwnMaterial(changed.slotMaterials.back())) {
                            changed.slotMaterials.pop_back();
                        }
                    },
                    &engine::AssetSystem::RequestMaterialInstance, labels[static_cast<qsizetype>(count + slot)]);
            }
            rows->addWidget(slot_form);
            *shown = {slot_form, std::move(labels)};
        }
        shown->form->refresh();
    });
}

void buildNavigationRegion(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    auto& scene = context.scene();
    using Region = engine::NavigationRegion;
    using Settings = engine::NavigationSettings;
    form.addVec3(QObject::tr("Half extents (world axes)"), field(scene, entity, &Region::halfExtents));
    form.addFloat(QObject::tr("Cell size"), field(scene, entity, &Region::settings, &Settings::cellSize));
    form.addFloat(QObject::tr("Cell height"), field(scene, entity, &Region::settings, &Settings::cellHeight));
    form.addFloat(QObject::tr("Agent height"), field(scene, entity, &Region::settings, &Settings::agentHeight));
    form.addFloat(QObject::tr("Agent radius"), field(scene, entity, &Region::settings, &Settings::agentRadius));
    form.addFloat(QObject::tr("Maximum step"), field(scene, entity, &Region::settings, &Settings::agentClimb));
    form.addFloat(QObject::tr("Maximum slope (degrees)"), field(scene, entity, &Region::settings, &Settings::agentSlope));
    auto* status = new QLabel;
    status->setWordWrap(true);
    form.addRow(QObject::tr("Baked data"), status, [&context, entity, status] {
        if (const QString text = navigationBakeStatus(context.engine(), entity); text != status->text())
            status->setText(text);
    });
    auto* bake = new QPushButton(QObject::tr("Bake Navigation Mesh"));
    QObject::connect(bake, &QPushButton::clicked, bake, [&context, entity, bake] {
        auto& document = context.sceneDocument();
        if (document.filePath().empty()) {
            const auto answer = QMessageBox::question(bake, QObject::tr("Navigation Region"),
                QObject::tr("The NavMesh is saved next to the scene. Save the scene first?"),
                QMessageBox::Save | QMessageBox::Cancel);
            if (answer != QMessageBox::Save || !document.saveInteractive(bake, false)) return;
        }
        std::string error;
        if (!bakeNavigation(context.engine(), context.history(), document.filePath(), entity, error))
            QMessageBox::warning(bake, QObject::tr("Navigation Region"), QString::fromStdString(error));
    });
    form.addRow({}, bake, [&context, bake] { bake->setEnabled(!context.engine().IsPlaying()); });
}

void buildNavigationSource(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    form.addBool(QObject::tr("Use Collider (off: Mesh Renderer)"),
                 field(context.scene(), entity, &engine::NavigationSource::useCollider));
}

void buildMeshRenderer(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    addRendererAsset(form, context, entity, QObject::tr("Mesh"), engine::CatalogAssetKind::Mesh,
                     &engine::MeshRenderer::meshReference, &engine::MeshRenderer::mesh, &engine::AssetSystem::RequestMesh);
    addRendererAsset(form, context, entity, QObject::tr("Material"), engine::CatalogAssetKind::MaterialInstance,
                     &engine::MeshRenderer::materialReference, &engine::MeshRenderer::materialInstance,
                     &engine::AssetSystem::RequestMaterialInstance, QObject::tr("(Mesh materials)"));
    addSlotMaterials(form, context, entity);
}

template<class Definition, class Parse>
bool readRigDefinition(const engine::AssetCatalog& catalog, const engine::AssetReference& reference,
                       engine::CatalogAssetKind kind, Definition& definition, Parse parse) {
    const auto* entry = catalog.Find(reference.id, kind);
    std::string text, error;
    return entry && engine::ReadTextFile(entry->source, text, error) && parse(text, definition, error);
}

bool compatibleClip(const engine::AssetCatalog& catalog, const engine::AssetReference& clip,
                    const engine::AssetReference& skeleton) {
    engine::AnimationClipDefinition definition;
    if (!skeleton.id.IsValid() || !readRigDefinition(catalog, clip, engine::CatalogAssetKind::AnimationClip,
            definition, engine::ParseAnimationClipDefinition)) return false;
    if (definition.skeleton.id == skeleton.id) return true;
    engine::SkeletonDefinition a, b;
    return readRigDefinition(catalog, skeleton, engine::CatalogAssetKind::Skeleton, a, engine::ParseSkeletonDefinition) &&
        readRigDefinition(catalog, definition.skeleton, engine::CatalogAssetKind::Skeleton, b, engine::ParseSkeletonDefinition) &&
        a.source.id == b.source.id;
}

// Keep filtering local to the animation inspector, including each popup reload.
class RigClipPicker final : public editor::AssetPicker {
public:
    RigClipPicker(editor::EditorContext& context, entt::entity entity, const QString& none)
        : AssetPicker(context.engine().GetSceneAssets().GetCatalog(), engine::CatalogAssetKind::AnimationClip, none),
          context_(context), entity_(entity) {}
    void showPopup() override {
        reload();
        const auto* animator = context_.scene().TryGetComponent<engine::Animator>(entity_);
        auto* items = qobject_cast<QStandardItemModel*>(model());
        if (items) for (int i = 0; i < count(); ++i) {
            const engine::AssetReference clip{engine::AssetUuid(itemData(i).toString().toStdString()), {}};
            const bool enabled = !clip.id.IsValid() || (animator && compatibleClip(
                context_.engine().GetSceneAssets().GetCatalog(), clip, animator->skeleton));
            items->item(i)->setEnabled(enabled);
            if (auto* list = qobject_cast<QListView*>(view())) list->setRowHidden(i, !enabled);
            setItemData(i, enabled ? QString{} : QObject::tr("Clip is incompatible with the assigned skeleton or its descriptor is unavailable."), Qt::ToolTipRole);
        }
        QComboBox::showPopup();
    }
private:
    editor::EditorContext& context_;
    entt::entity entity_;
};

void inferRendererSkeleton(editor::EditorContext& context, entt::entity entity, const engine::AssetReference& mesh) {
    engine::SkinnedMeshDefinition definition;
    if (!readRigDefinition(context.engine().GetSceneAssets().GetCatalog(), mesh, engine::CatalogAssetKind::SkinnedMesh,
            definition, engine::ParseSkinnedMeshDefinition)) return;
    auto& scene = context.scene();
    if (scene.HasComponent<engine::Animator>(entity)) {
        scene.Modify<engine::Animator>(entity, [&](auto& animator) { animator.skeleton = definition.skeleton; });
    } else {
        engine::Animator animator;
        animator.skeleton = definition.skeleton;
        scene.AddComponent<engine::Animator>(entity, animator);
    }
}

void buildSkinnedMeshRenderer(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    auto* meshPicker = new editor::AssetPicker(context.engine().GetSceneAssets().GetCatalog(), engine::CatalogAssetKind::SkinnedMesh);
    QObject::connect(meshPicker, &QComboBox::currentIndexChanged, meshPicker, [&context, entity, meshPicker] {
        const auto reference = meshPicker->reference();
        const auto* renderer = context.scene().TryGetComponent<engine::SkinnedMeshRenderer>(entity);
        if (!reference || !renderer || renderer->meshReference.id == reference->id) return;
        context.history().edit(QObject::tr("Change skinned mesh and skeleton"), [&] {
            const auto handle = context.engine().GetSceneAssets().RequestSkinnedMesh(*reference);
            context.scene().Modify<engine::SkinnedMeshRenderer>(entity, [&](auto& changed) {
                changed.meshReference = *reference;
                changed.mesh = handle;
                changed.posedBounds.reset();
            });
            inferRendererSkeleton(context, entity, *reference);
        });
    });
    form.addRow(QObject::tr("Mesh"), meshPicker, [&context, entity, meshPicker] {
        const auto* renderer = context.scene().TryGetComponent<engine::SkinnedMeshRenderer>(entity);
        if (!renderer || meshPicker->hasFocus() || meshPicker->lineEdit()->hasFocus() || meshPicker->view()->isVisible()) return;
        const QSignalBlocker blocker(meshPicker);
        if (!meshPicker->selectId(renderer->meshReference.id)) {
            meshPicker->reload();
            if (!meshPicker->selectId(renderer->meshReference.id)) {
                meshPicker->setCurrentIndex(-1);
                meshPicker->lineEdit()->setPlaceholderText(QObject::tr("%1 (missing)").arg(QString::fromStdString(renderer->meshReference.path)));
            }
        }
    });
    addRendererAsset(form, context, entity, QObject::tr("Material"), engine::CatalogAssetKind::MaterialInstance,
                     &engine::SkinnedMeshRenderer::materialReference, &engine::SkinnedMeshRenderer::materialInstance,
                     &engine::AssetSystem::RequestMaterialInstance);
    auto* diagnostic = new QLabel;
    diagnostic->setWordWrap(true);
    form.addRow(QObject::tr("Rig compatibility"), diagnostic, [&context, entity, diagnostic] {
        const auto* renderer = context.scene().TryGetComponent<engine::SkinnedMeshRenderer>(entity);
        const auto* animator = context.scene().TryGetComponent<engine::Animator>(entity);
        if (!renderer) return;
        const auto mesh = context.engine().GetSceneAssets().Peek(renderer->mesh);
        engine::SkinnedMeshDefinition definition;
        const bool known = readRigDefinition(context.engine().GetSceneAssets().GetCatalog(), renderer->meshReference,
            engine::CatalogAssetKind::SkinnedMesh, definition, engine::ParseSkinnedMeshDefinition);
        QString text;
        if (!animator) text = QObject::tr("No Animator: mesh uses its rest pose.");
        else if (known && definition.skeleton.id != animator->skeleton.id)
            text = QObject::tr("Renderer and Animator reference different skeletons; incompatible poses fall back to rest.");
        if (mesh && animator) {
            auto& assets = context.engine().GetSceneAssets();
            if (const auto* entry = assets.GetCatalog().Find(animator->skeleton.id, engine::CatalogAssetKind::Skeleton)) {
                const auto skeleton = assets.Peek<engine::SkeletonAsset>(entry->path);
                if (skeleton && skeleton->signature != mesh->skeletonSignature)
                    text = QObject::tr("Skeleton/source revision mismatch: rendering uses the mesh rest pose.");
            }
        }
        diagnostic->setText(text);
    });
}

void addAnimatorReference(PropertyForm& form, editor::EditorContext& context, entt::entity entity,
                          const QString& label, engine::CatalogAssetKind kind,
                          engine::AssetReference engine::Animator::*member) {
    editor::AssetPicker* picker = kind == engine::CatalogAssetKind::AnimationClip
        ? static_cast<editor::AssetPicker*>(new RigClipPicker(context, entity, QObject::tr("None")))
        : new editor::AssetPicker(context.engine().GetSceneAssets().GetCatalog(), kind, QObject::tr("None"));
    picker->setMinimumContentsLength(12);
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker, [&context, entity, picker, member, label] {
        const auto* animator = context.scene().TryGetComponent<engine::Animator>(entity);
        if (!animator || picker->currentIndex() < 0) return;
        const auto chosen = picker->reference().value_or(engine::AssetReference{});
        if (member == &engine::Animator::clip && chosen.id.IsValid() &&
            !compatibleClip(context.engine().GetSceneAssets().GetCatalog(), chosen, animator->skeleton)) {
            context.showStatus(QObject::tr("Clip is incompatible with the assigned skeleton."), 5000);
            return;
        }
        if ((animator->*member).id == chosen.id && (animator->*member).path == chosen.path) return;
        context.history().edit(QObject::tr("Change %1").arg(label), [&] {
            context.scene().Modify<engine::Animator>(entity, [&](auto& changed) { changed.*member = chosen; });
        });
    });
    form.addRow(label, picker, [&context, entity, picker, member] {
        const auto* animator = context.scene().TryGetComponent<engine::Animator>(entity);
        if (!animator || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) return;
        const QSignalBlocker blocker(picker);
        const auto& reference = animator->*member;
        if (!reference.id.IsValid()) {
            picker->setCurrentIndex(0);
        } else if (!picker->selectId(reference.id)) {
            picker->reload();
            if (!picker->selectId(reference.id)) {
                picker->setCurrentIndex(-1);
                picker->lineEdit()->setPlaceholderText(QObject::tr("%1 (missing)").arg(
                    QString::fromStdString(reference.path.empty() ? reference.id.ToString() : reference.path)));
            }
        }
    });
}

void buildAnimator(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    auto& scene = context.scene();
    addAnimatorReference(form, context, entity, QObject::tr("Skeleton"), engine::CatalogAssetKind::Skeleton,
                         &engine::Animator::skeleton);
    addAnimatorReference(form, context, entity, QObject::tr("Clip"), engine::CatalogAssetKind::AnimationClip,
                         &engine::Animator::clip);
    form.addFloat(QObject::tr("Starting speed"), field(scene, entity, &engine::Animator::speed), {.step = 0.1, .decimals = 3, .minimum = 0.0});
    form.addBool(QObject::tr("Loop"), field(scene, entity, &engine::Animator::loop));
    form.addBool(QObject::tr("Play on start"), field(scene, entity, &engine::Animator::playOnStart));

    if (!context.engine().GetSubsystem<engine::AnimationSubsystem>()) return;
    auto* controls = new QWidget;
    auto* layout = new QVBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* mode = new QLabel;
    layout->addWidget(mode);
    auto* playbackClip = new RigClipPicker(context, entity, QObject::tr("Choose playback clip"));
    if (const auto* animator = scene.TryGetComponent<engine::Animator>(entity))
        playbackClip->selectId(animator->clip.id);
    playbackClip->setToolTip(QObject::tr("Transient playback target; does not change the scene's starting clip."));
    layout->addWidget(playbackClip);
    auto* fade = new QDoubleSpinBox;
    fade->setRange(0.0, 3600.0);
    fade->setSingleStep(0.1);
    fade->setSuffix(QObject::tr(" s fade"));
    PropertyForm::ignoreWheelUnlessFocused(fade);
    layout->addWidget(fade);
    auto* previewSpeed = new QDoubleSpinBox;
    previewSpeed->setRange(0.0, 1000.0);
    previewSpeed->setSingleStep(0.1);
    previewSpeed->setPrefix(QObject::tr("Transport speed: "));
    previewSpeed->setValue(1.0);
    PropertyForm::ignoreWheelUnlessFocused(previewSpeed);
    layout->addWidget(previewSpeed);
    auto* previewLoop = new QCheckBox(QObject::tr("Transport loop (restart clip to apply)"));
    previewLoop->setChecked(true);
    layout->addWidget(previewLoop);
    auto* buttons = new QHBoxLayout;
    auto* play = new QPushButton(QObject::tr("Play"));
    auto* pause = new QPushButton(QObject::tr("Pause"));
    auto* stop = new QPushButton(QObject::tr("Stop"));
    buttons->addWidget(play);
    buttons->addWidget(pause);
    buttons->addWidget(stop);
    layout->addLayout(buttons);
    auto* seek = new QDoubleSpinBox;
    seek->setRange(0.0, 1.0e9);
    seek->setDecimals(3);
    seek->setSingleStep(0.1);
    seek->setSuffix(QObject::tr(" s"));
    PropertyForm::ignoreWheelUnlessFocused(seek);
    layout->addWidget(seek);
    auto* timeline = new QSlider(Qt::Horizontal);
    timeline->setRange(0, 10000);
    timeline->setToolTip(QObject::tr("Seek within the loaded clip"));
    layout->addWidget(timeline);
    auto* status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);
    auto error = std::make_shared<QString>();
    const auto start = [&context, entity, fade, playbackClip, previewSpeed, previewLoop, error] {
        auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>();
        const auto* animator = context.scene().TryGetComponent<engine::Animator>(entity);
        const auto clip = playbackClip->reference();
        if (!animation || !animator || !clip) return;
        std::string message;
        if (!compatibleClip(context.engine().GetSceneAssets().GetCatalog(), *clip, animator->skeleton)) {
            *error = QObject::tr("Clip is missing, malformed or incompatible with the assigned skeleton.");
            return;
        }
        const bool started = context.engine().IsPlaying()
            ? animation->Play(entity, *clip, static_cast<float>(fade->value()), previewLoop->isChecked(), message)
            : animation->Preview(entity, *clip, static_cast<float>(fade->value()), previewLoop->isChecked(), message);
        if (started) animation->SetSpeed(entity, static_cast<float>(previewSpeed->value()), message);
        *error = QString::fromStdString(message);
    };
    QObject::connect(play, &QPushButton::clicked, controls, start);
    QObject::connect(playbackClip, &QComboBox::currentIndexChanged, controls, [&context, entity, start] {
        auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>();
        if (animation && animation->GetPreviewEntity() == entity) start();
    });
    QObject::connect(previewSpeed, &QDoubleSpinBox::valueChanged, controls, [&context, entity, error](double value) {
        auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>();
        if (!animation || (!context.engine().IsPlaying() && animation->GetPreviewEntity() != entity)) return;
        std::string message;
        animation->SetSpeed(entity, static_cast<float>(value), message);
        *error = QString::fromStdString(message);
    });
    QObject::connect(pause, &QPushButton::clicked, controls, [&context, entity] {
        if (auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>()) {
            if (const auto state = animation->GetPlaybackState(entity)) animation->SetPaused(entity, state->playing);
        }
    });
    QObject::connect(stop, &QPushButton::clicked, controls, [&context, entity, error] {
        if (auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>()) {
            if (context.engine().IsPlaying()) animation->Stop(entity);
            else animation->EndPreview();
        }
        error->clear();
    });
    QObject::connect(seek, &QDoubleSpinBox::editingFinished, controls, [&context, entity, seek, error] {
        if (auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>()) {
            std::string message;
            animation->Seek(entity, seek->value(), message);
            *error = QString::fromStdString(message);
        }
    });
    QObject::connect(timeline, &QSlider::valueChanged, controls, [&context, entity, timeline, seek, error](int position) {
        if (auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>()) {
            const auto state = animation->GetPlaybackState(entity);
            if (!state || state->duration <= 0 || state->transitioning) return;
            const double seconds = state->duration * static_cast<double>(position) / timeline->maximum();
            std::string message;
            animation->Seek(entity, seconds, message);
            seek->setValue(seconds);
            *error = QString::fromStdString(message);
        }
    });
    // Transport is transient subsystem state, deliberately outside authored-property history.
    form.addRow(QObject::tr("Playback"), controls, [&context, entity, controls, playbackClip, previewSpeed, play, pause, stop, seek, timeline, mode, status, error] {
        auto* animation = context.engine().GetSubsystem<engine::AnimationSubsystem>();
        const auto* animator = context.scene().TryGetComponent<engine::Animator>(entity);
        controls->setEnabled(animation && animator);
        if (!animation || !animator) return;
        const auto state = animation->GetPlaybackState(entity);
        const bool runtime = context.engine().IsPlaying();
        const bool active = runtime || animation->GetPreviewEntity() == entity;
        if (active && state && !previewSpeed->hasFocus()) {
            const QSignalBlocker blocker(previewSpeed);
            previewSpeed->setValue(state->speed);
        }
        mode->setText(runtime ? QObject::tr("Play mode — runtime transport") : QObject::tr("Edit mode — selected entity preview"));
        play->setText(runtime ? QObject::tr("Play clip") : QObject::tr("Preview clip"));
        stop->setText(runtime ? QObject::tr("Stop") : QObject::tr("End preview"));
        const auto chosen = playbackClip->reference();
        playbackClip->setEnabled(!(active && state && state->transitioning));
        play->setEnabled(chosen && compatibleClip(context.engine().GetSceneAssets().GetCatalog(), *chosen, animator->skeleton) &&
            !(active && state && state->transitioning));
        pause->setEnabled(active && state && state->ready && state->duration > 0);
        stop->setEnabled(active);
        seek->setEnabled(active && state && state->ready && state->duration > 0.0f && !state->transitioning);
        timeline->setEnabled(seek->isEnabled());
        pause->setText(state && !state->playing ? QObject::tr("Resume") : QObject::tr("Pause"));
        if (state && !seek->hasFocus()) {
            const QSignalBlocker blocker(seek);
            seek->setMaximum(state->duration);
            seek->setValue(state->timeSeconds);
        }
        if (state && !timeline->isSliderDown()) {
            const QSignalBlocker blocker(timeline);
            timeline->setValue(state->duration > 0 ? static_cast<int>(state->timeSeconds / state->duration * timeline->maximum()) : 0);
        }
        QString diagnostic;
        const auto& catalog = context.engine().GetSceneAssets().GetCatalog();
        if (animator->skeleton.id.IsValid() && !catalog.Find(animator->skeleton.id, engine::CatalogAssetKind::Skeleton))
            diagnostic = QObject::tr("Authored skeleton is missing (reference preserved). ");
        if (animator->clip.id.IsValid() && !compatibleClip(catalog, animator->clip, animator->skeleton))
            diagnostic += QObject::tr("Authored starting clip is missing or incompatible (reference preserved). ");
        const QString transport = !active ? QObject::tr("Stopped — normal Edit pose") : !state ? QObject::tr("Loading") :
            state->transitioning ? QObject::tr("Loading / transitioning") : !state->ready ? QObject::tr("Loading") :
            state->duration <= 0 ? QObject::tr("Stopped") : state->playing ? QObject::tr("Playing") : QObject::tr("Paused / finished");
        const auto clipName = [&catalog](const engine::AssetReference& reference) {
            if (const auto* entry = catalog.Find(reference.id, engine::CatalogAssetKind::AnimationClip))
                return QString::fromStdString(entry->path);
            return QString::fromStdString(reference.path);
        };
        QString playingClip;
        if (active && state && state->clip.id.IsValid()) {
            playingClip = QObject::tr("Current: %1 (%2x, %3)\n").arg(clipName(state->clip))
                .arg(state->speed).arg(state->loop ? QObject::tr("loop") : QObject::tr("once"));
            if (state->targetClip.id.IsValid()) playingClip += QObject::tr("Target: %1\n").arg(clipName(state->targetClip));
        }
        status->setText(diagnostic + playingClip + (!error->isEmpty() ? *error : state && !state->error.empty()
            ? QObject::tr("Error: %1").arg(QString::fromStdString(state->error))
            : QObject::tr("%1 — %2 / %3 s").arg(transport).arg(state ? state->timeSeconds : 0, 0, 'f', 3)
                .arg(state ? state->duration : 0, 0, 'f', 3)));
    });
}

void buildCamera(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addAngle(QObject::tr("Field of view"), field(scene, entity, &engine::Camera::fovY),
                  {.step = 1.0, .decimals = 1, .minimum = 1.0, .maximum = 179.0});
    form.addFloat(QObject::tr("Near plane"), field(scene, entity, &engine::Camera::nearPlane),
                  {.step = 0.01, .decimals = 3, .minimum = 0.001, .maximum = 1.0e4});
    form.addFloat(QObject::tr("Far plane"), field(scene, entity, &engine::Camera::farPlane),
                  {.step = 1.0, .decimals = 2, .minimum = 0.01, .maximum = 1.0e6});
}

QString audioListenerDiagnostic(editor::EditorContext& context) {
    const auto& scene = context.scene();
    if (context.engine().IsPlaying()) {
        if (const auto* audio = context.engine().GetSubsystem<engine::AudioSubsystem>()) {
            const auto diagnostic = audio->GetListenerDiagnostic();
            if (!diagnostic.empty()) return QString::fromStdString(diagnostic);
            const auto listener = audio->GetEffectiveListener();
            if (scene.IsValid(listener))
                return QObject::tr("Effective listener: %1").arg(QString::fromStdString(scene.GetName(listener)));
        }
    }
    int count = 0;
    entt::entity listener = entt::null;
    scene.ForEach<engine::AudioListener>([&](entt::entity entity, const engine::AudioListener& value) {
        if (value.enabled) { ++count; listener = entity; }
    });
    if (count > 1) return QObject::tr("Conflicting enabled listeners (%1). Disable all but one; 3D output is suppressed.").arg(count);
    if (count == 1) return QObject::tr("Explicit listener: %1").arg(QString::fromStdString(scene.GetName(listener)));
    const auto camera = scene.GetActiveCamera();
    if (scene.IsValid(camera) && scene.HasComponent<engine::Camera>(camera))
        return QObject::tr("Fallback listener: active scene camera (%1).").arg(QString::fromStdString(scene.GetName(camera)));
    return QObject::tr("No enabled listener or active scene camera. 3D output is suppressed; 2D audio remains available.");
}

void buildAudioListener(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    form.addBool(QObject::tr("Enabled"), field(context.scene(), entity, &engine::AudioListener::enabled));
    form.addText(QObject::tr("Listener"), [&context] { return audioListenerDiagnostic(context); });
    form.addText({}, [] { return QObject::tr("One enabled explicit listener per scene. Local -Z is forward; +Y is up."); });
}

// Validate a copy before notifying Scene. Rejected edits produce neither authored changes nor undo steps.
template<class T>
Binding<T> audioField(engine::Scene& scene, entt::entity entity, T engine::AudioSource::*member,
                      const std::shared_ptr<std::string>& error) {
    auto binding = field(scene, entity, member);
    binding.set = [&scene, entity, member, error](const T& value) {
        const auto* source = std::as_const(scene).TryGetComponent<engine::AudioSource>(entity);
        if (!source) return;
        auto candidate = *source;
        candidate.*member = value;
        if (!engine::ValidateAudioSource(candidate, *error)) return;
        error->clear();
        if (candidate != *source)
            scene.Modify<engine::AudioSource>(entity, [&](auto& changed) { changed = candidate; });
    };
    return binding;
}

template<class T>
void addAudioChoice(PropertyForm& form, editor::EditorContext& context, entt::entity entity,
                    const QString& label, T engine::AudioSource::*member,
                    std::initializer_list<std::pair<const char*, T>> choices,
                    const std::shared_ptr<std::string>& error) {
    auto binding = audioField(context.scene(), entity, member, error);
    auto* combo = new QComboBox;
    for (const auto& [text, value] : choices) combo->addItem(QObject::tr(text), static_cast<int>(value));
    QObject::connect(combo, &QComboBox::activated, combo, [&context, combo, binding, label](int index) {
        const auto value = static_cast<T>(combo->itemData(index).toInt());
        context.history().edit(QObject::tr("Change %1").arg(label), [&] { binding.set(value); });
    });
    form.addRow(label, combo, [combo, binding] {
        if (const auto* value = binding.get(); value && !combo->view()->isVisible()) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(combo->findData(static_cast<int>(*value)));
        }
    });
}

void buildAudioSource(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    using Source = engine::AudioSource;
    auto& scene = context.scene();
    auto error = std::make_shared<std::string>();
    auto previewVoice = std::make_shared<engine::AudioVoiceHandle>();
    auto previewClip = std::make_shared<std::optional<engine::AssetReference>>();
    const auto audio = [&context] { return context.engine().GetSubsystem<engine::AudioSubsystem>(); };
    form.addBool(QObject::tr("Enabled"), audioField(scene, entity, &Source::enabled, error));

    auto* picker = new editor::AssetPicker(context.engine().GetSceneAssets().GetCatalog(),
                                          engine::CatalogAssetKind::AudioClip, QObject::tr("None"));
    picker->setMinimumContentsLength(12);
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker,
                     [&context, &scene, entity, picker, error, audio, previewVoice](int index) {
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        if (!source || index < 0 || source->clip == picker->reference()) return;
        if (auto* subsystem = audio()) subsystem->Stop(*previewVoice);
        *previewVoice = {};
        auto binding = audioField(scene, entity, &Source::clip, error);
        context.history().edit(QObject::tr("Change Audio Clip"), [&] { binding.set(picker->reference()); });
    });
    form.addRow(QObject::tr("Clip"), picker, [&scene, entity, picker] {
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        if (!source || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) return;
        const QSignalBlocker blocker(picker);
        if (!source->clip) { picker->setCurrentIndex(0); return; }
        if (!picker->selectId(source->clip->id)) {
            picker->reload();
            if (picker->selectId(source->clip->id)) return;
            picker->setCurrentIndex(-1);
            picker->lineEdit()->setPlaceholderText(QObject::tr("%1 (missing)").arg(QString::fromStdString(
                source->clip->path.empty() ? source->clip->id.ToString() : source->clip->path)));
        }
    });
    form.addBool(QObject::tr("Play on start"), audioField(scene, entity, &Source::playOnStart, error));
    form.addBool(QObject::tr("Looping"), audioField(scene, entity, &Source::looping, error));
    addAudioChoice(form, context, entity, QObject::tr("Loading"), &Source::loading,
                   {{"Buffered", engine::AudioLoadingMode::Buffered}, {"Streamed", engine::AudioLoadingMode::Streamed}}, error);
    addAudioChoice(form, context, entity, QObject::tr("Bus"), &Source::bus,
                   {{"Music", engine::AudioBus::Music}, {"SFX", engine::AudioBus::SFX}, {"UI", engine::AudioBus::UI}}, error);
    form.addFloat(QObject::tr("Volume (0–1)"), audioField(scene, entity, &Source::volume, error),
                   {.step = 0.05, .decimals = 3, .minimum = 0, .maximum = 1});
    constexpr PropertyForm::NumberFormat positive{.step = 0.1, .decimals = 3, .minimum = 0.001, .maximum = 1.0e6};
    form.addFloat(QObject::tr("Pitch (ratio)"), audioField(scene, entity, &Source::pitch, error), positive);
    addAudioChoice(form, context, entity, QObject::tr("Spatial mode"), &Source::spatial,
                   {{"2D", engine::AudioSpatialMode::TwoD}, {"3D (mono clip)", engine::AudioSpatialMode::ThreeD}}, error);
    auto* spatial = new PropertyForm;
    form.addRow(spatial, [&scene, entity, spatial] {
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        spatial->setVisible(source && source->spatial == engine::AudioSpatialMode::ThreeD);
        spatial->refresh();
    });
    addAudioChoice(*spatial, context, entity, QObject::tr("Attenuation"), &Source::attenuation,
                   {{"None", engine::AudioAttenuation::None}, {"Inverse", engine::AudioAttenuation::Inverse},
                    {"Linear", engine::AudioAttenuation::Linear}, {"Exponential", engine::AudioAttenuation::Exponential}}, error);
    spatial->addFloat(QObject::tr("Minimum distance (m)"), audioField(scene, entity, &Source::minDistance, error), positive);
    spatial->addFloat(QObject::tr("Maximum distance (m)"), audioField(scene, entity, &Source::maxDistance, error), positive);
    spatial->addFloat(QObject::tr("Rolloff (factor)"), audioField(scene, entity, &Source::rolloff, error),
                      {.step = 0.1, .decimals = 3, .minimum = 0, .maximum = 1.0e6});
    spatial->addText({}, [] { return QObject::tr("1 scene unit = 1 metre. Maximum distance is an attenuation bound, not a hard silence radius. 3D point emitters require mono clips."); });
    spatial->addText(QObject::tr("Listener"), [&context] { return audioListenerDiagnostic(context); });

    struct Metadata {
        std::optional<engine::AssetReference> clip;
        std::filesystem::path path;
        audio_editor::FileStamp stamp;
        std::shared_ptr<audio_editor::Probe> probe;
    };
    auto metadata = std::make_shared<Metadata>();
    form.addText(QObject::tr("Clip metadata"), [&context, &scene, entity, metadata, audio, previewVoice] {
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        if (!source || !source->clip) {
            if (auto* subsystem = audio()) subsystem->Stop(*previewVoice);
            *previewVoice = {}; *metadata = {};
            return QObject::tr("No clip assigned");
        }
        const auto* entry = context.engine().GetSceneAssets().GetCatalog().Find(source->clip->id, engine::CatalogAssetKind::AudioClip);
        if (!entry) {
            if (auto* subsystem = audio()) subsystem->Stop(*previewVoice);
            *previewVoice = {}; *metadata = {};
            return QObject::tr("Clip is missing from the catalog");
        }
        const auto stamp = audio_editor::FileStamp::read(entry->source);
        if (metadata->clip != source->clip || metadata->path != entry->source || metadata->stamp != stamp) {
            if (auto* subsystem = audio()) subsystem->Stop(*previewVoice);
            *previewVoice = {};
            metadata->clip = source->clip; metadata->path = entry->source;
            metadata->stamp = stamp;
            metadata->probe = audio_editor::Probe::start(entry->source);
        }
        const auto& probe = metadata->probe;
        if (!probe || !probe->complete()) return QObject::tr("Inspecting audio…");
        if (!probe->error.empty()) return QObject::tr("Invalid audio: %1").arg(QString::fromStdString(probe->error));
        if (source->spatial == engine::AudioSpatialMode::ThreeD && probe->info.channels != 1)
            return QObject::tr("Invalid 3D source: clip has %1 channels. Choose 2D or assign a mono clip.").arg(probe->info.channels);
        return QObject::tr("%1 s · %2 channels · %3 Hz · %4 bytes")
            .arg(probe->info.durationSeconds, 0, 'f', 3).arg(probe->info.channels).arg(probe->info.sampleRate).arg(qulonglong(probe->info.fileBytes));
    });

    auto* buttons = new QWidget;
    auto* row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    auto* preview = new QPushButton(QObject::tr("Audition raw clip (2D)"));
    auto* stop = new QPushButton(QObject::tr("Stop"));
    preview->setToolTip(QObject::tr("Audition the clip in 2D, independently of Edit pause. Does not start the scene source."));
    row->addWidget(preview);
    row->addWidget(stop);
    QObject::connect(preview, &QPushButton::clicked, preview, [&context, &scene, entity, audio, error, previewVoice, previewClip] {
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        if (context.engine().IsPlaying() || !source || !source->clip) return;
        if (auto* subsystem = audio()) {
            *previewClip = source->clip;
            engine::AudioPreviewOptions options;
            options.volume = qBound(0, QSettings().value("audio/auditionVolume", 100).toInt(), 100) / 100.f;
            *previewVoice = subsystem->Preview(*source->clip, options, *error);
        }
    });
    QObject::connect(stop, &QPushButton::clicked, stop, [audio, previewVoice, error] {
        if (auto* subsystem = audio()) subsystem->Stop(*previewVoice);
        *previewVoice = {};
        error->clear();
    });
    // Stop only this form's voice: deleteLater() may run after another inspector has started auditioning.
    QObject::connect(&form, &QObject::destroyed, &context.sceneEvents(), [audio, previewVoice] {
        if (auto* subsystem = audio()) subsystem->Stop(*previewVoice);
    });
    form.addRow(buttons, [&context, &scene, entity, audio, previewVoice, previewClip, preview, stop] {
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        auto* subsystem = audio();
        if (subsystem && *previewVoice && (!source || source->clip != *previewClip)) {
            subsystem->Stop(*previewVoice);
            *previewVoice = {};
        }
        preview->setEnabled(!context.engine().IsPlaying() && subsystem && subsystem->IsAvailable() && source && source->clip &&
            context.engine().GetSceneAssets().GetCatalog().Find(source->clip->id, engine::CatalogAssetKind::AudioClip));
        const auto status = subsystem ? subsystem->GetStatus(*previewVoice) : engine::AudioVoiceStatus::Invalid;
        stop->setEnabled(status == engine::AudioVoiceStatus::Pending || status == engine::AudioVoiceStatus::Playing ||
                         status == engine::AudioVoiceStatus::Paused);
    });
    auto* sourceButtons = new QWidget;
    auto* sourceRow = new QHBoxLayout(sourceButtons);
    sourceRow->setContentsMargins(0, 0, 0, 0);
    auto* playSource = new QPushButton(QObject::tr("Play source"));
    auto* pauseSource = new QPushButton(QObject::tr("Pause"));
    auto* resumeSource = new QPushButton(QObject::tr("Resume"));
    auto* stopSource = new QPushButton(QObject::tr("Stop source"));
    for (auto* button : {playSource, pauseSource, resumeSource, stopSource}) sourceRow->addWidget(button);
    QObject::connect(playSource, &QPushButton::clicked, playSource, [audio, entity, error] {
        if (auto* subsystem = audio()) (void)subsystem->PlaySource(entity, *error);
    });
    QObject::connect(pauseSource, &QPushButton::clicked, pauseSource, [audio, entity] { if (auto* a = audio()) a->PauseSource(entity); });
    QObject::connect(resumeSource, &QPushButton::clicked, resumeSource, [audio, entity] { if (auto* a = audio()) a->ResumeSource(entity); });
    QObject::connect(stopSource, &QPushButton::clicked, stopSource, [audio, entity] { if (auto* a = audio()) a->StopSource(entity); });
    form.addRow(sourceButtons, [&context, &scene, entity, audio, metadata, sourceButtons, buttons, playSource, pauseSource, resumeSource, stopSource] {
        const bool playing = context.engine().IsPlaying();
        sourceButtons->setVisible(playing); buttons->setVisible(!playing);
        auto* a = audio();
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        const auto probe = metadata->probe;
        const bool valid = source && source->clip && metadata->clip == source->clip && probe && probe->complete() && probe->error.empty() &&
            (source->spatial == engine::AudioSpatialMode::TwoD || probe->info.channels == 1);
        playSource->setEnabled(playing && a && a->IsAvailable() && valid && source->enabled);
        const auto status = a ? a->GetStatus(a->GetSourceVoice(entity)) : engine::AudioVoiceStatus::Invalid;
        pauseSource->setEnabled(playing && status == engine::AudioVoiceStatus::Playing);
        resumeSource->setEnabled(playing && status == engine::AudioVoiceStatus::Paused);
        stopSource->setEnabled(playing && (status == engine::AudioVoiceStatus::Pending || status == engine::AudioVoiceStatus::Playing || status == engine::AudioVoiceStatus::Paused));
    });
    form.addText(QObject::tr("Audio status"), [&context, &scene, entity, audio, previewVoice, error] {
        if (!error->empty()) return QString::fromStdString(*error);
        const auto* source = std::as_const(scene).TryGetComponent<Source>(entity);
        if (source && source->clip && !context.engine().GetSceneAssets().GetCatalog().Find(source->clip->id, engine::CatalogAssetKind::AudioClip))
            return QObject::tr("Audio clip is missing. The authored reference is preserved.");
        auto* subsystem = audio();
        if (!subsystem) return QObject::tr("Audio subsystem is unavailable.");
        if (!subsystem->IsAvailable()) return QString::fromStdString(subsystem->GetDiagnostic());
        const auto voice = context.engine().IsPlaying() ? subsystem->GetSourceVoice(entity) : *previewVoice;
        switch (subsystem->GetStatus(voice)) {
        case engine::AudioVoiceStatus::Pending: return QObject::tr("Loading…");
        case engine::AudioVoiceStatus::Playing: return QObject::tr("Playing");
        case engine::AudioVoiceStatus::Paused: return QObject::tr("Paused");
        case engine::AudioVoiceStatus::Failed: return QString::fromStdString(subsystem->GetVoiceError(voice));
        case engine::AudioVoiceStatus::Finished: return QObject::tr("Finished");
        default: return QObject::tr("Stopped");
        }
    });
}

void buildDirectionalLight(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addColor(QObject::tr("Color"), field(scene, entity, &engine::DirectionalLight::color));
    form.addFloat(QObject::tr("Intensity"), field(scene, entity, &engine::DirectionalLight::intensity), Intensity);
}

void buildPointLight(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addColor(QObject::tr("Color"), field(scene, entity, &engine::PointLight::color));
    form.addFloat(QObject::tr("Intensity"), field(scene, entity, &engine::PointLight::intensity), Intensity);
    form.addFloat(QObject::tr("Range"), field(scene, entity, &engine::PointLight::range), Range);
}

void buildSpotLight(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addColor(QObject::tr("Color"), field(scene, entity, &engine::SpotLight::color));
    form.addFloat(QObject::tr("Intensity"), field(scene, entity, &engine::SpotLight::intensity), Intensity);
    form.addFloat(QObject::tr("Range"), field(scene, entity, &engine::SpotLight::range), Range);
    form.addAngle(QObject::tr("Inner angle"), field(scene, entity, &engine::SpotLight::innerHalfAngle), HalfAngle);
    form.addAngle(QObject::tr("Outer angle"), field(scene, entity, &engine::SpotLight::outerHalfAngle), HalfAngle);
}

void buildBounds(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addVec3(QObject::tr("Min"), field(scene, entity, &engine::Bounds::local, &engine::Aabb::min));
    form.addVec3(QObject::tr("Max"), field(scene, entity, &engine::Bounds::local, &engine::Aabb::max));
}

void buildRigidBody(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    auto& scene = context.scene();
    auto* motion = new QComboBox;
    motion->addItem(QObject::tr("Static"), static_cast<int>(engine::MotionType::Static));
    motion->addItem(QObject::tr("Dynamic"), static_cast<int>(engine::MotionType::Dynamic));
    motion->addItem(QObject::tr("Kinematic"), static_cast<int>(engine::MotionType::Kinematic));
    motion->setToolTip(QObject::tr("Dynamic bodies need no parent. Moving mesh bodies always use a convex hull."));
    QObject::connect(motion, &QComboBox::activated, motion, [&context, &scene, entity, motion](int index) {
        const auto* body = std::as_const(scene).TryGetComponent<engine::RigidBody>(entity);
        if (!body) return;
        const auto type = static_cast<engine::MotionType>(motion->itemData(index).toInt());
        if (type == engine::MotionType::Dynamic && scene.GetParent(entity) != entt::null) return;
        const auto* collider = std::as_const(scene).TryGetComponent<engine::Collider>(entity);
        const bool convex = type != engine::MotionType::Static && collider && collider->shape == engine::ColliderShape::TriangleMesh;
        const bool changed = body->motionType != type;
        context.history().edit(QObject::tr("Change Motion type"), [&] {
            if (convex)
                scene.Modify<engine::Collider>(entity, [](engine::Collider& edited) { edited.shape = engine::ColliderShape::ConvexMesh; });
            if (changed)
                scene.Modify<engine::RigidBody>(entity, [type](engine::RigidBody& edited) { edited.motionType = type; });
        });
    });
    form.addRow(QObject::tr("Motion type"), motion, [&scene, entity, motion] {
        const auto* body = scene.TryGetComponent<engine::RigidBody>(entity);
        if (!body) return;
        if (auto* model = qobject_cast<QStandardItemModel*>(motion->model()))
            model->item(motion->findData(static_cast<int>(engine::MotionType::Dynamic)))->setEnabled(scene.GetParent(entity) == entt::null);
        if (!motion->view()->isVisible()) {
            const QSignalBlocker blocker(motion);
            motion->setCurrentIndex(motion->findData(static_cast<int>(body->motionType)));
        }
    });
    form.addFloat(QObject::tr("Mass (kg)"), field(scene, entity, &engine::RigidBody::mass),
                  {.step = 0.1, .decimals = 3, .minimum = 0.001, .maximum = 1.0e6});
    form.addFloat(QObject::tr("Friction"), field(scene, entity, &engine::RigidBody::friction),
                  {.step = 0.05, .decimals = 3, .minimum = 0, .maximum = 1.0e4});
    form.addFloat(QObject::tr("Restitution"), field(scene, entity, &engine::RigidBody::restitution),
                  {.step = 0.05, .decimals = 3, .minimum = 0, .maximum = 1});
}

void buildCollider(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    using Shape = engine::ColliderShape;
    auto& scene = context.scene();
    const auto component = [&scene, entity] { return std::as_const(scene).TryGetComponent<engine::Collider>(entity); };
    const auto modify = [&context, entity](const QString& text, auto&& edit) {
        context.history().edit(text, [&] { context.scene().Modify<engine::Collider>(entity, edit); });
    };
    const auto mesh = [&context, &scene, entity] {
        const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity);
        return renderer ? context.engine().GetSceneAssets().Peek(renderer->mesh) : entt::resource<engine::MeshAsset>{};
    };
    auto* shape = new QComboBox;
    for (const auto& [label, value] : std::initializer_list<std::pair<const char*, Shape>>{
             {"Cube", Shape::Cube}, {"Box (parallelepiped)", Shape::Box}, {"Sphere", Shape::Sphere},
             {"Capsule", Shape::Capsule}, {"Mesh", Shape::ConvexMesh}})
        shape->addItem(QObject::tr(label), static_cast<int>(value));
    shape->setToolTip(QObject::tr("Mesh uses Mesh Renderer geometry. Enable Convex to generate a convex hull."));
    QObject::connect(shape, &QComboBox::activated, shape, [&scene, entity, component, modify, shape](int index) {
        const auto* collider = component();
        const auto* body = std::as_const(scene).TryGetComponent<engine::RigidBody>(entity);
        if (!collider || !body) return;
        auto chosen = static_cast<Shape>(shape->itemData(index).toInt());
        if (chosen == Shape::ConvexMesh) {
            if (!scene.HasComponent<engine::MeshRenderer>(entity)) return;
            if (body->motionType == engine::MotionType::Static && collider->shape != Shape::ConvexMesh) chosen = Shape::TriangleMesh;
        }
        modify(QObject::tr("Change Shape"), [chosen](engine::Collider& changed) { changed.shape = chosen; });
    });
    form.addRow(QObject::tr("Shape"), shape, [&scene, entity, component, shape] {
        const auto* collider = component();
        const auto* body = scene.TryGetComponent<engine::RigidBody>(entity);
        if (!collider || !body) return;
        if (auto* model = qobject_cast<QStandardItemModel*>(shape->model())) {
            const bool hasMesh = scene.HasComponent<engine::MeshRenderer>(entity);
            model->item(shape->findData(static_cast<int>(Shape::ConvexMesh)))->setEnabled(hasMesh);
        }
        if (!shape->view()->isVisible()) {
            const QSignalBlocker blocker(shape);
            shape->setCurrentIndex(shape->findData(static_cast<int>(collider->shape == Shape::TriangleMesh ? Shape::ConvexMesh : collider->shape)));
        }
    });
    auto* convexPanel = new QWidget;
    auto* convexLayout = new QHBoxLayout(convexPanel);
    convexLayout->setContentsMargins(0, 0, 0, 0);
    auto* convex = new QCheckBox(QObject::tr("Convex (convex hull)"));
    convex->setToolTip(QObject::tr("Generate a convex hull from mesh vertices. Fills holes and concavities. Required for Dynamic/Kinematic."));
    convexLayout->addWidget(convex);
    QObject::connect(convex, &QCheckBox::clicked, convex, [&scene, entity, component, modify](bool checked) {
        const auto* collider = component();
        const auto* body = std::as_const(scene).TryGetComponent<engine::RigidBody>(entity);
        if (!collider || !body || collider->shape < Shape::ConvexMesh) return;
        if (!checked && body->motionType != engine::MotionType::Static) return;
        modify(QObject::tr("Change Convex"),
               [checked](engine::Collider& changed) { changed.shape = checked ? Shape::ConvexMesh : Shape::TriangleMesh; });
    });
    form.addRow(convexPanel, [&scene, entity, component, convex, convexPanel] {
        const auto* collider = component();
        const auto* body = scene.TryGetComponent<engine::RigidBody>(entity);
        convexPanel->setVisible(collider && collider->shape >= Shape::ConvexMesh);
        convex->setEnabled(body && body->motionType == engine::MotionType::Static);
        const QSignalBlocker blocker(convex);
        convex->setChecked(collider && collider->shape == Shape::ConvexMesh);
    });
    form.addBool(QObject::tr("Is trigger"), field(scene, entity, &engine::Collider::isTrigger));
    form.addBool(QObject::tr("Show collider"), field(scene, entity, &engine::Collider::showDebug));
    form.addVec3(QObject::tr("Center"), field(scene, entity, &engine::Collider::center));
    // Separate forms show only the dimensions that the selected shape uses.
    const auto dimensions = [&](Shape kind) {
        auto* fields = new PropertyForm;
        form.addRow(fields, [component, fields, kind] {
            const auto* collider = component();
            fields->setVisible(collider && collider->shape == kind);
            fields->refresh();
        });
        return fields;
    };
    constexpr PropertyForm::NumberFormat positive{.step = 0.1, .decimals = 3, .minimum = 0.001, .maximum = 1.0e6};
    dimensions(Shape::Cube)->addFloat(QObject::tr("Half side"),
                                      field(scene, entity, &engine::Collider::halfExtents, &Vector3::x), positive);
    dimensions(Shape::Box)->addVec3(QObject::tr("Half extents"), field(scene, entity, &engine::Collider::halfExtents), positive);
    dimensions(Shape::Sphere)->addFloat(QObject::tr("Radius"), field(scene, entity, &engine::Collider::radius), positive);
    auto* capsule = dimensions(Shape::Capsule);
    capsule->addFloat(QObject::tr("Radius"), field(scene, entity, &engine::Collider::radius), positive);
    capsule->addFloat(QObject::tr("Cylinder half height"), field(scene, entity, &engine::Collider::halfHeight),
                      {.step = 0.1, .decimals = 3, .minimum = 0, .maximum = 1.0e6});
    auto* buttons = new QWidget;
    auto* row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    auto* fit = new QPushButton(QObject::tr("Fit to mesh"));
    auto* fromMesh = new QPushButton(QObject::tr("From mesh"));
    fit->setToolTip(QObject::tr("Fit this primitive to the loaded mesh bounds. Capsule axis is local Y."));
    fromMesh->setToolTip(QObject::tr("Use Mesh Renderer geometry. Moving bodies require Convex; Static can toggle it. Resets Center."));
    row->addWidget(fit);
    row->addWidget(fromMesh);
    QObject::connect(fit, &QPushButton::clicked, fit, [modify, mesh] {
        const auto asset = mesh();
        if (!asset) return;
        modify(QObject::tr("Fit Collider to Mesh"), [&asset](engine::Collider& collider) {
            const Vector3 half = (asset->bounds.max - asset->bounds.min) * 0.5f;
            collider.center = (asset->bounds.min + asset->bounds.max) * 0.5f;
            collider.halfExtents = Vector3::Max(half, Vector3(0.001f));
            if (collider.shape == Shape::Cube) collider.halfExtents = Vector3((std::max)({half.x, half.y, half.z, 0.001f}));
            collider.radius = (std::max)(0.001f, collider.shape == Shape::Sphere ? half.Length() : (std::max)(half.x, half.z));
            collider.halfHeight = (std::max)(0.0f, half.y - collider.radius);
        });
    });
    QObject::connect(fromMesh, &QPushButton::clicked, fromMesh, [&scene, entity, modify, mesh] {
        const auto* body = std::as_const(scene).TryGetComponent<engine::RigidBody>(entity);
        const auto asset = mesh();
        if (!body || !asset || !asset->geometry) return;
        const bool moving = body->motionType != engine::MotionType::Static;
        modify(QObject::tr("Collider from Mesh"), [moving](engine::Collider& collider) {
            collider.shape = moving || collider.shape == Shape::ConvexMesh ? Shape::ConvexMesh : Shape::TriangleMesh;
            collider.center = {};
        });
    });
    form.addRow(buttons, [component, mesh, fit, fromMesh] {
        const auto* collider = component();
        const auto asset = mesh();
        fit->setEnabled(collider && asset && collider->shape <= Shape::Capsule);
        fromMesh->setEnabled(collider && asset && asset->geometry);
    });
    form.addText({}, [component, mesh] {
        const auto* collider = component();
        if (!mesh()) return QObject::tr("Assign a mesh in Mesh Renderer and wait for it to load to use mesh tools.");
        return collider && collider->shape == Shape::ConvexMesh ? QObject::tr("Convex hull fills holes and concavities.")
             : collider && collider->shape == Shape::TriangleMesh ? QObject::tr("Triangle mesh: Static only, one-sided surfaces.") : QString{};
    });
}

void buildGamePersistence(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addBool(QObject::tr("Save transform"), field(scene, entity, &engine::GamePersistence::transform));
    form.addBool(QObject::tr("Save controller state"),
                 field(scene, entity, &engine::GamePersistence::controllerState));
    form.addBool(QObject::tr("Save script state"),
                 field(scene, entity, &engine::GamePersistence::scriptState));
}

/// One row of a Script's Properties
void addScriptProperty(QFormLayout& layout, editor::EditorContext& context, const entt::entity entity,
                       const std::string& name, const engine::StateValue* value, const bool overridden,
                       const std::function<void()>& invalidate) {
    engine::Scene& scene = context.scene();
    auto* row = new QWidget;
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    auto* reset = new QPushButton(QObject::tr("Reset"));
    reset->setEnabled(overridden);
    const QString label = QString::fromStdString(name);
    QObject::connect(reset, &QPushButton::clicked, reset, [&context, &scene, entity, name, label, invalidate] {
        context.history().edit(QObject::tr("Reset %1").arg(label), [&] {
            scene.Modify<engine::ScriptComponent>(entity, [&name](engine::ScriptComponent& component) {
                component.properties.erase(name);
            });
        });
        invalidate();
    });
    // Text applies on each key: those merge into one step.
    const QString key = QString::fromStdString("script/" + scene.GetId(entity).ToString() + "/" + name);
    const auto store = [&context, &scene, entity, name, label, key, reset](engine::StateValue changed) {
        bool stored = false;
        context.history().edit(QObject::tr("Change %1").arg(label), [&] {
            stored = scene.Modify<engine::ScriptComponent>(entity, [&](engine::ScriptComponent& component) {
                component.properties[name] = std::move(changed);
            });
        }, key);
        if (stored) reset->setEnabled(true);
    };

    if (value == nullptr) {
        line->addWidget(new QLabel(QObject::tr("Not declared by the script")));
    } else if (const auto* text = std::get_if<std::string>(&value->value)) {
        auto* edit = new QLineEdit(QString::fromStdString(*text));
        QObject::connect(edit, &QLineEdit::textEdited, edit,
                         [store](const QString& changed) { store(changed.toStdString()); });
        line->addWidget(edit);
    } else if (const auto* boolean = std::get_if<bool>(&value->value)) {
        auto* box = new QCheckBox;
        box->setChecked(*boolean);
        QObject::connect(box, &QCheckBox::toggled, box, [store](const bool checked) { store(checked); });
        line->addWidget(box);
    } else {
        // The inspector's usual number format, widened only to fit the value: a spin box is as wide
        // as the text of its range.
        const PropertyForm::NumberFormat format;
        auto* box = new QDoubleSpinBox;
        box->setRange(std::min(format.minimum, value->Number()), std::max(format.maximum, value->Number()));
        box->setDecimals(format.decimals);
        box->setSingleStep(format.step);
        box->setValue(value->Number());
        box->setKeyboardTracking(false);
        PropertyForm::ignoreWheelUnlessFocused(box);
        QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [store](const double changed) { store(changed); });
        line->addWidget(box);
    }
    line->addWidget(reset);
    layout.addRow(QString::fromStdString(name), row);
}

/// Replaces the rows in `layout` with the properties `component`'s script `entry` declares, or a
/// note on why there are none.
void fillScriptProperties(QFormLayout& layout, editor::EditorContext& context, const entt::entity entity,
                          const engine::ScriptComponent& component, const engine::CatalogEntry* entry,
                          const std::function<void()>& invalidate) {
    while (QLayoutItem* item = layout.takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const auto note = [&layout](const QString& text) {
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        layout.addRow(label);
    };

    if (entry == nullptr) {
        note(component.script.id.IsValid() ? QObject::tr("The script is missing.")
                                           : QObject::tr("Choose a script to edit its properties."));
        return;
    }
    std::string source;
    std::string error;
    engine::ScriptPropertyMap declared;
    if (!engine::ReadTextFile(entry->source, source, error) || !engine::ReadScriptProperties(source, declared, error)) {
        note(QString::fromStdString(error));
        return;
    }
    for (const auto& [name, value] : declared) {
        const auto saved = component.properties.find(name);
        if (saved == component.properties.end()) {
            addScriptProperty(layout, context, entity, name, &value, false, invalidate);
        } else if (engine::CompatibleScriptProperty(value, saved->second)) {
            addScriptProperty(layout, context, entity, name, &saved->second, true, invalidate);
        } else {
            addScriptProperty(layout, context, entity, name, nullptr, true, invalidate);
        }
    }
    for (const auto& [name, value] : component.properties) {
        if (!declared.contains(name)) {
            addScriptProperty(layout, context, entity, name, nullptr, true, invalidate);
        }
    }
}

/// The Script component's Properties. The rows are only rebuilt when the script, or its file on
/// disk, changes, or after a Reset: the editors show their own edits.
void addScriptProperties(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    // A heading of its own across the form, with the rows below it, indented.
    auto* panel = new QWidget;
    auto* section = new QVBoxLayout(panel);
    section->setContentsMargins(0, 0, 0, 0);
    section->addWidget(new QLabel(QObject::tr("Properties:")));
    auto* layout = new QFormLayout;
    layout->setContentsMargins(panel->style()->pixelMetric(QStyle::PM_LayoutLeftMargin), 0, 0, 0);
    section->addLayout(layout);
    // What the rows were built from. Not valid to begin with, so the first refresh builds them.
    struct Shown {
        bool valid = false;
        engine::AssetUuid script;
        std::filesystem::file_time_type modified;
    };
    auto shown = std::make_shared<Shown>();
    form.addRow(panel, [&context, entity, layout, shown] {
        const auto* component = context.scene().TryGetComponent<engine::ScriptComponent>(entity);
        if (component == nullptr) {
            return;
        }
        const engine::CatalogEntry* entry =
            context.engine().GetAssets().GetCatalog().Find(component->script.id, engine::CatalogAssetKind::Script);
        std::error_code ignored;  // an unreadable file shows its error in the rows
        const auto modified =
            entry != nullptr ? std::filesystem::last_write_time(entry->source, ignored) : std::filesystem::file_time_type{};
        if (shown->valid && shown->script == component->script.id && shown->modified == modified) {
            return;
        }
        *shown = {true, component->script.id, modified};
        fillScriptProperties(*layout, context, entity, *component, entry, [shown] { shown->valid = false; });
    });
}

template<class Component>
void addLuaAssetPicker(PropertyForm& form, editor::EditorContext& context, entt::entity entity,
                       engine::AssetReference Component::* reference, engine::CatalogAssetKind kind,
                       const QString& label, const QString& editLabel, const QString& openLabel) {
    auto& scene = context.scene();
    auto& catalog = context.engine().GetAssets().GetCatalog();
    auto* picker = new editor::AssetPicker(catalog, kind, QObject::tr("None"));
    picker->setMinimumContentsLength(12);  // as in addAssetPicker: a wider row widens every component's form
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker,
                     [&context, &scene, entity, picker, reference, editLabel](const int index) {
        const auto* component = std::as_const(scene).TryGetComponent<Component>(entity);
        // -1 means nothing is chosen
        if (component == nullptr || index < 0) {
            return;
        }
        // "None" is chosen, so no reference.
        const engine::AssetReference chosen = picker->reference().value_or(engine::AssetReference{});
        if (chosen.id == (component->*reference).id) {
            return;
        }
        context.history().edit(editLabel, [&] {
            scene.Modify<Component>(entity, [&](Component& changed) {
                changed.*reference = chosen;
                if constexpr (std::is_same_v<Component, engine::ScriptComponent>) changed.properties.clear();
            });
        });
    });
    form.addRow(label, picker, [&scene, entity, picker, reference] {
        const auto* component = scene.TryGetComponent<Component>(entity);
        if (!component || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) {
            return;
        }
        const QSignalBlocker blocker(picker);
        const auto& asset = component->*reference;
        if (!asset.id.IsValid()) {
            picker->setCurrentIndex(0);  // "None"
            return;
        }
        if (!picker->selectId(asset.id)) {
            picker->setCurrentIndex(-1);
            picker->lineEdit()->setPlaceholderText(QObject::tr("%1 (missing)").arg(
                QString::fromStdString(asset.path.empty() ? asset.id.ToString() : asset.path)));
        }
    });
    auto* open = new QPushButton(openLabel);
    QObject::connect(open, &QPushButton::clicked, open, [&scene, &catalog, entity, reference, kind] {
        if (const auto* component = scene.TryGetComponent<Component>(entity))
            if (const auto* entry = catalog.Find((component->*reference).id, kind))
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(engine::ToUtf8(entry->source))));
    });
    form.addRow({}, open, [&scene, &catalog, entity, open, reference, kind] {
        const auto* component = scene.TryGetComponent<Component>(entity);
        open->setEnabled(component && catalog.Find((component->*reference).id, kind));
    });
}

void buildScript(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    form.addBool(QObject::tr("Enabled"), field(context.scene(), entity, &engine::ScriptComponent::enabled));
    addLuaAssetPicker(form, context, entity, &engine::ScriptComponent::script, engine::CatalogAssetKind::Script,
                      QObject::tr("Script"), QObject::tr("Change Script"), QObject::tr("Open script"));
    addScriptProperties(form, context, entity);
}

void buildBehaviorTree(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    using Component = engine::ai::BehaviorTreeComponent;
    auto& scene = context.scene();
    form.addBool(QObject::tr("Enabled"), field(scene, entity, &Component::enabled));
    addLuaAssetPicker(form, context, entity, &Component::tree, engine::CatalogAssetKind::BehaviorTree,
                      QObject::tr("Tree"), QObject::tr("Change Behavior Tree"), QObject::tr("Open Lua source"));
    const auto treeEntry = [&context, entity]() -> const engine::CatalogEntry* {
        const auto* component = context.scene().TryGetComponent<Component>(entity);
        return component ? context.engine().GetAssets().GetCatalog().Find(component->tree.id,
                                                                           engine::CatalogAssetKind::BehaviorTree)
                         : nullptr;
    };
    auto* graph = new QPushButton(QObject::tr("Show Tree Graph"));
    QObject::connect(graph, &QPushButton::clicked, graph, [&context, treeEntry] {
        if (const auto* entry = treeEntry()) context.openAsset(entry->source);
    });
    form.addRow({}, graph, [graph, treeEntry] { graph->setEnabled(treeEntry() != nullptr); });

    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* edit = new QPlainTextEdit;
    edit->setMaximumHeight(100);
    edit->setPlaceholderText(QStringLiteral(R"({"seconds": 1, "pulseEnabled": true})"));
    auto* apply = new QPushButton(QObject::tr("Apply Parameters"));
    auto* message = new QLabel;
    message->setWordWrap(true);
    layout->addWidget(edit);
    layout->addWidget(apply);
    layout->addWidget(message);
    auto shown = std::make_shared<std::optional<engine::ScriptPropertyMap>>();
    QObject::connect(apply, &QPushButton::clicked, panel, [&context, &scene, entity, edit, message, shown] {
        engine::StateValue value;
        std::string error;
        if (!engine::ParseStateValue(edit->toPlainText().toStdString(), value, error) ||
            !std::holds_alternative<engine::StateValue::Object>(value.value)) {
            message->setText(error.empty() ? QObject::tr("Enter a JSON object.") : QString::fromStdString(error));
            return;
        }
        if (!engine::ValidateScriptProperties(value.Fields(), error)) {
            message->setText(QString::fromStdString(error));
            return;
        }
        context.history().edit(QObject::tr("Change Behavior Tree Parameters"), [&] {
            scene.Modify<Component>(entity, [&](Component& component) { component.parameters = value.Fields(); });
        });
        message->clear();
        shown->reset();
        edit->document()->setModified(false);
    });
    form.addRow(QObject::tr("Parameters (JSON)"), panel, [&scene, entity, edit, message, shown] {
        const auto* component = scene.TryGetComponent<Component>(entity);
        if (!component || edit->document()->isModified() || *shown == component->parameters) return;
        std::string json, error;
        if (engine::SerializeStateValue(engine::StateValue(component->parameters), json, error)) {
            edit->setPlainText(QString::fromStdString(json));
            edit->document()->setModified(false);
            *shown = component->parameters;
            message->clear();
        }
    });
}

/// Default-constructed, through Scene: it counts as a structural change, so the scene is dirty.
template <class T>
void addComponent(editor::EditorContext& context, const entt::entity entity) {
    context.scene().AddComponent<T>(entity);
}

template <class T>
void removeComponent(editor::EditorContext& context, const entt::entity entity) {
    context.scene().RemoveComponent<T>(entity);
}

/// The mesh's own bounds if it has one loaded, a unit cube around the origin otherwise.
void addBounds(editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    engine::Bounds bounds{{Vector3{-0.5f, -0.5f, -0.5f}, Vector3{0.5f, 0.5f, 0.5f}}};
    if (const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity)) {
        if (const auto mesh = context.engine().GetSceneAssets().Peek(renderer->mesh)) {
            bounds.local = mesh->bounds;
        }
    }
    if (const auto* renderer = scene.TryGetComponent<engine::SkinnedMeshRenderer>(entity)) {
        if (renderer->posedBounds) bounds.local = *renderer->posedBounds;
        else if (const auto mesh = context.engine().GetSceneAssets().Peek(renderer->mesh)) bounds.local = mesh->bounds;
    }
    scene.AddComponent<engine::Bounds>(entity, bounds);
}

// The scene codec requires the pair. Either menu action produces a saveable
// default Static body; removing either component also removes its companion.
void addPhysics(editor::EditorContext& context, const entt::entity entity) {
    auto& scene = context.scene();
    if (!scene.HasComponent<engine::Collider>(entity)) scene.AddComponent<engine::Collider>(entity);
    if (!scene.HasComponent<engine::RigidBody>(entity)) scene.AddComponent<engine::RigidBody>(entity);
}
void removePhysics(editor::EditorContext& context, const entt::entity entity) {
    context.scene().RemoveComponent<engine::RigidBody>(entity);
    context.scene().RemoveComponent<engine::Collider>(entity);
}

void addSkinnedMeshRenderer(editor::EditorContext& context, const entt::entity entity) {
    auto& scene = context.scene();
    auto& assets = context.engine().GetSceneAssets();
    const auto meshes = assets.GetCatalog().List(engine::CatalogAssetKind::SkinnedMesh);
    if (meshes.empty()) {
        QMessageBox::information(nullptr, QObject::tr("Skinned Mesh Renderer"),
                                 QObject::tr("Add a skinned mesh descriptor to the content folder first."));
        return;
    }
    engine::SkinnedMeshRenderer renderer;
    // The scene codec requires file-backed renderer slots, including in undo snapshots.
    renderer.meshReference = {meshes.front()->metadata.id, meshes.front()->path};
    renderer.mesh = assets.RequestSkinnedMesh(renderer.meshReference);
    renderer.materialInstance = assets.RequestMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance);
    if (const auto* material = assets.GetCatalog().FindPath(engine::BuiltinAssets::DefaultMaterialInstance))
        renderer.materialReference = {material->metadata.id, material->path};
    scene.RemoveComponent<engine::MeshRenderer>(entity);
    scene.AddComponent<engine::SkinnedMeshRenderer>(entity, std::move(renderer));
    inferRendererSkeleton(context, entity, scene.TryGetComponent<engine::SkinnedMeshRenderer>(entity)->meshReference);
}

}  // namespace

void addMeshRenderer(editor::EditorContext& context, const entt::entity entity) {
    context.scene().RemoveComponent<engine::SkinnedMeshRenderer>(entity);
    engine::AssetSystem& assets = context.engine().GetSceneAssets();
    const engine::AssetCatalog& catalog = assets.GetCatalog();
    engine::MeshRenderer renderer;
    if (const engine::CatalogEntry* mesh = catalog.FindPath(BuiltinCubeFile)) {
        renderer.meshReference = {mesh->metadata.id, mesh->path};
        renderer.mesh = assets.RequestMesh(renderer.meshReference);
    } else {
        renderer.mesh = assets.RequestMesh(engine::BuiltinAssets::CubeMesh);
    }
    // No material of its own: the mesh's slots bring theirs.
    context.scene().AddComponent<engine::MeshRenderer>(entity, std::move(renderer));
}

std::span<const ComponentInspector> componentInspectors() {
    // maybe not necessary? I doubt we're gonna be caring about translation soon
    static const std::vector<ComponentInspector> inspectors{
        {entt::type_hash<engine::ScriptComponent>::value(), QObject::tr("Script"), buildScript,
         addComponent<engine::ScriptComponent>, removeComponent<engine::ScriptComponent>},
        {entt::type_hash<engine::ai::BehaviorTreeComponent>::value(), QObject::tr("Behavior Tree"), buildBehaviorTree,
         addComponent<engine::ai::BehaviorTreeComponent>, removeComponent<engine::ai::BehaviorTreeComponent>},
        {entt::type_hash<engine::Transform>::value(), QObject::tr("Transform"), buildTransform},
        {entt::type_hash<engine::Tags>::value(), QObject::tr("Tags"), buildTags},
        {entt::type_hash<engine::MeshRenderer>::value(), QObject::tr("Mesh Renderer"), buildMeshRenderer,
         addMeshRenderer, removeComponent<engine::MeshRenderer>},
        {entt::type_hash<engine::SkinnedMeshRenderer>::value(), QObject::tr("Skinned Mesh Renderer"), buildSkinnedMeshRenderer,
         addSkinnedMeshRenderer, removeComponent<engine::SkinnedMeshRenderer>},
        {entt::type_hash<engine::Animator>::value(), QObject::tr("Animator"), buildAnimator,
         addComponent<engine::Animator>, removeComponent<engine::Animator>},
        {entt::type_hash<engine::NavigationRegion>::value(), QObject::tr("NavMesh"), buildNavigationRegion},
        {entt::type_hash<engine::NavigationSource>::value(), QObject::tr("Navigation Source"), buildNavigationSource,
         addComponent<engine::NavigationSource>, removeComponent<engine::NavigationSource>},
        {entt::type_hash<engine::Camera>::value(), QObject::tr("Camera"), buildCamera, addComponent<engine::Camera>,
         removeComponent<engine::Camera>},
        {entt::type_hash<engine::AudioSource>::value(), QObject::tr("Audio Source"), buildAudioSource,
         addComponent<engine::AudioSource>, removeComponent<engine::AudioSource>},
        {entt::type_hash<engine::AudioListener>::value(), QObject::tr("Audio Listener"), buildAudioListener,
         addComponent<engine::AudioListener>, removeComponent<engine::AudioListener>},
        {entt::type_hash<engine::DirectionalLight>::value(), QObject::tr("Directional Light"), buildDirectionalLight,
         addComponent<engine::DirectionalLight>, removeComponent<engine::DirectionalLight>},
        {entt::type_hash<engine::PointLight>::value(), QObject::tr("Point Light"), buildPointLight,
         addComponent<engine::PointLight>, removeComponent<engine::PointLight>},
        {entt::type_hash<engine::SpotLight>::value(), QObject::tr("Spot Light"), buildSpotLight,
         addComponent<engine::SpotLight>, removeComponent<engine::SpotLight>},
        {entt::type_hash<engine::Bounds>::value(), QObject::tr("Bounds"), buildBounds, addBounds,
         removeComponent<engine::Bounds>},
        {entt::type_hash<engine::RigidBody>::value(), QObject::tr("Rigid Body"), buildRigidBody,
         addPhysics, removePhysics, QObject::tr("Remove Rigid Body and Collider")},
        {entt::type_hash<engine::Collider>::value(), QObject::tr("Collider"), buildCollider,
         addPhysics, removePhysics, QObject::tr("Remove Rigid Body and Collider")},
        {entt::type_hash<engine::GamePersistence>::value(), QObject::tr("Game Persistence"), buildGamePersistence,
         addComponent<engine::GamePersistence>, removeComponent<engine::GamePersistence>},
    };
    return inspectors;
}

bool isHiddenComponent(const entt::id_type type) {
    return type == entt::type_hash<engine::Name>::value() || type == entt::type_hash<engine::EntityId>::value() ||
           type == entt::type_hash<engine::Hierarchy>::value() || type == entt::type_hash<engine::EditorFolder>::value();
}
