#include "component_inspectors.hpp"

#include <QAbstractItemView>
#include <QLineEdit>
#include <QObject>
#include <QSignalBlocker>
#include <QStringList>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QDoubleSpinBox>
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
#include <utility>
#include <vector>

#include <entt/core/type_info.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/asset_picker.hpp>
#include <editor/editor_context.hpp>
#include <editor/scene_document.hpp>

#include "property_form.hpp"

namespace {

using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector3;
template <class T>
using Getter = PropertyForm::Getter<T>; // std::function<T*()>

// I hate templates but we've got to...
/// `member` of the entity's T, or nullptr once the entity or the component is gone.
template <class T, class M>
Getter<M> field(engine::Scene& scene, const entt::entity entity, M T::*member) {
    return [&scene, entity, member]() -> M* {
        T* component = scene.TryGetComponent<T>(entity);
        return component != nullptr ? &(component->*member) : nullptr;
    };
}

/// Same for the transform, which every entity has but TryGetComponent() doesn't hand out.
template <class M>
Getter<M> transformField(engine::Scene& scene, const entt::entity entity, M engine::Transform::*member) {
    return [&scene, entity, member]() -> M* {
        return scene.IsValid(entity) ? &(scene.GetTransform(entity).*member) : nullptr;
    };
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
    form.addVec3(QObject::tr("Scale"), transformField(scene, entity, &engine::Transform::scale));
}

/// Comma-separated. Tags go through Scene, which counts them as structural changes.
void buildTags(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    auto* edit = new QLineEdit;
    edit->setPlaceholderText(QObject::tr("tag, another tag"));
    QObject::connect(edit, &QLineEdit::editingFinished, edit, [&scene, entity, edit] {
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
        for (const std::string& tag : had) {
            if (std::find(wanted.begin(), wanted.end(), tag) == wanted.end()) {
                scene.RemoveTag(entity, tag);
            }
        }
        for (const std::string& tag : wanted) {
            scene.AddTag(entity, tag);  // ignores ones it has
        }
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

/// The asset a MeshRenderer slot uses: the authored one, which a save writes, or else the loaded one.
template <class Asset>
std::optional<engine::AssetReference> usedAsset(const engine::AssetSystem& assets,
                                                const engine::AssetReference& authored,
                                                const engine::AssetHandle<Asset> handle) {
    if (authored.id.IsValid()) {
        return authored;
    }
    return assets.ReferenceFor(handle);
}

/// A picker for one asset slot of the MeshRenderer: `reference` and `handle` are its members,
/// `request` what loads a chosen asset into the scene's registry.
template <class Asset>
void addAssetSlot(PropertyForm& form, editor::EditorContext& context, const entt::entity entity, const QString& label,
                  const engine::CatalogAssetKind kind, engine::AssetReference engine::MeshRenderer::*reference,
                  engine::AssetHandle<Asset> engine::MeshRenderer::*handle,
                  engine::AssetHandle<Asset> (engine::AssetSystem::*request)(const engine::AssetReference&)) {
    engine::Scene& scene = context.scene();
    engine::Engine& engine = context.engine();
    auto* picker = new editor::AssetPicker(engine.GetSceneAssets().GetCatalog(), kind);
    picker->setMinimumContentsLength(12);  // the dock is narrow; the popup shows whole paths

    // Only the user's choices get here: refresh picks quietly.
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker,
                      [&context, &scene, &engine, entity, picker, kind, reference, handle, request] {
                         auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity);
                         const std::optional<engine::AssetReference> chosen = picker->reference();
                         if (renderer == nullptr || !chosen || chosen->id == (renderer->*reference).id) {
                             return;
                         }
                          // Asynchronous, like adding the component: the entity isn't drawn until it's loaded.
                          renderer->*handle = (engine.GetSceneAssets().*request)(*chosen);
                          renderer->*reference = *chosen;
                          if (kind == engine::CatalogAssetKind::MaterialInstance) {
                              engine.ClearEntityMaterialOverride(entity);
                          }
                         context.sceneDocument().markDirty();
                     });

    // What the picker last showed, so it's only touched when the slot changes elsewhere.
    auto shown = std::make_shared<std::string>();
    form.addRow(label, picker, [&scene, &engine, entity, picker, reference, handle, shown] {
        const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity);
        if (renderer == nullptr || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) {
            return;
        }
        const std::optional<engine::AssetReference> used =
            usedAsset(engine.GetSceneAssets(), renderer->*reference, renderer->*handle);
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
        picker->setCurrentIndex(-1);
        picker->lineEdit()->setPlaceholderText(
            used ? QObject::tr("%1 (missing)").arg(QString::fromStdString(used->path.empty() ? used->id.ToString() : used->path))
                 : QObject::tr("None"));
    });
}

void buildMeshRenderer(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    addAssetSlot(form, context, entity, QObject::tr("Mesh"), engine::CatalogAssetKind::Mesh,
                 &engine::MeshRenderer::meshReference, &engine::MeshRenderer::mesh, &engine::AssetSystem::RequestMesh);
    addAssetSlot(form, context, entity, QObject::tr("Material"), engine::CatalogAssetKind::MaterialInstance,
                 &engine::MeshRenderer::materialReference, &engine::MeshRenderer::materialInstance,
                 &engine::AssetSystem::RequestMaterialInstance);
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
    const Getter<engine::Aabb> local = field(context.scene(), entity, &engine::Bounds::local);
    form.addVec3(QObject::tr("Min"), [local]() -> Vector3* {
        engine::Aabb* aabb = local();
        return aabb != nullptr ? &aabb->min : nullptr;
    });
    form.addVec3(QObject::tr("Max"), [local]() -> Vector3* {
        engine::Aabb* aabb = local();
        return aabb != nullptr ? &aabb->max : nullptr;
    });
}

void buildGamePersistence(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    form.addBool(QObject::tr("Save transform"), field(scene, entity, &engine::GamePersistence::transform));
    form.addBool(QObject::tr("Save controller state"),
                 field(scene, entity, &engine::GamePersistence::controllerState));
}

/// One row of a Script's Properties
void addScriptProperty(QFormLayout& layout, editor::EditorContext& context, const entt::entity entity,
                       const std::string& name, const engine::StateValue* value, const bool overridden,
                       const std::function<void()>& invalidate) {
    const auto component = [&context, entity] {
        return context.scene().TryGetComponent<engine::ScriptComponent>(entity);
    };
    auto* row = new QWidget;
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    auto* reset = new QPushButton(QObject::tr("Reset"));
    reset->setEnabled(overridden);
    QObject::connect(reset, &QPushButton::clicked, reset, [&context, component, name, invalidate] {
        if (engine::ScriptComponent* current = component()) {
            current->properties.erase(name);
            context.sceneDocument().markDirty();
        }
        invalidate();
    });
    const auto store = [&context, component, name, reset](engine::StateValue changed) {
        if (engine::ScriptComponent* current = component()) {
            current->properties[name] = std::move(changed);
            context.sceneDocument().markDirty();
            reset->setEnabled(true);
        }
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

void buildScript(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    auto& scene = context.scene();
    auto& catalog = context.engine().GetAssets().GetCatalog();
    form.addBool(QObject::tr("Enabled"), field(scene, entity, &engine::ScriptComponent::enabled));
    auto* picker = new editor::AssetPicker(catalog, engine::CatalogAssetKind::Script, QObject::tr("None"));
    picker->setMinimumContentsLength(12);  // as in addAssetSlot: a wider row widens every component's form
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker, [&context, &scene, entity, picker](const int index) {
        auto* component = scene.TryGetComponent<engine::ScriptComponent>(entity);
        // -1 means nothing is chosen
        if (component == nullptr || index < 0) {
            return;
        }
        // "None" is chosen, so no reference.
        const engine::AssetReference chosen = picker->reference().value_or(engine::AssetReference{});
        if (chosen.id == component->script.id) {
            return;
        }
        component->script = chosen;
        component->properties.clear();
        context.sceneDocument().markDirty();
    });
    form.addRow(QObject::tr("Script"), picker, [&scene, entity, picker] {
        const auto* component = scene.TryGetComponent<engine::ScriptComponent>(entity);
        if (!component || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) return;
        const QSignalBlocker blocker(picker);
        if (!component->script.id.IsValid()) { picker->setCurrentIndex(0); return; }
        if (!picker->selectId(component->script.id)) {
            picker->setCurrentIndex(-1);
            picker->lineEdit()->setPlaceholderText(QObject::tr("%1 (missing)").arg(
                QString::fromStdString(component->script.path.empty() ? component->script.id.ToString() : component->script.path)));
        }
    });
    auto* open = new QPushButton(QObject::tr("Open script"));
    QObject::connect(open, &QPushButton::clicked, open, [&scene, &catalog, entity] {
        if (const auto* component = scene.TryGetComponent<engine::ScriptComponent>(entity))
            if (const auto* entry = catalog.Find(component->script.id, engine::CatalogAssetKind::Script))
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(engine::ToUtf8(entry->source))));
    });
    form.addRow({}, open, [&scene, &catalog, entity, open] {
        const auto* component = scene.TryGetComponent<engine::ScriptComponent>(entity);
        open->setEnabled(component && catalog.Find(component->script.id, engine::CatalogAssetKind::Script));
    });
    addScriptProperties(form, context, entity);
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

/// A built-in cube with the default material, as Add Cube makes, but with authored references so
/// it saves.
void addMeshRenderer(editor::EditorContext& context, const entt::entity entity) {
    engine::AssetSystem& assets = context.engine().GetSceneAssets();
    const engine::AssetCatalog& catalog = assets.GetCatalog();
    engine::MeshRenderer renderer;
    if (const engine::CatalogEntry* mesh = catalog.FindPath(BuiltinCubeFile)) {
        renderer.meshReference = {mesh->metadata.id, mesh->path};
        renderer.mesh = assets.RequestMesh(renderer.meshReference);
    } else {
        renderer.mesh = assets.RequestMesh(engine::BuiltinAssets::CubeMesh);
    }
    if (const engine::CatalogEntry* material = catalog.FindPath(engine::BuiltinAssets::DefaultMaterialInstance)) {
        renderer.materialReference = {material->metadata.id, material->path};
        renderer.materialInstance = assets.RequestMaterialInstance(renderer.materialReference);
    } else {
        renderer.materialInstance = assets.RequestMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance);
    }
    context.scene().AddComponent<engine::MeshRenderer>(entity, std::move(renderer));
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
    scene.AddComponent<engine::Bounds>(entity, bounds);
}

}  // namespace

std::span<const ComponentInspector> componentInspectors() {
    // maybe not necessary? I doubt we're gonna be caring about translation soon
    static const std::vector<ComponentInspector> inspectors{
        {entt::type_hash<engine::ScriptComponent>::value(), QObject::tr("Script"), buildScript,
         addComponent<engine::ScriptComponent>, removeComponent<engine::ScriptComponent>},
        {entt::type_hash<engine::Transform>::value(), QObject::tr("Transform"), buildTransform},
        {entt::type_hash<engine::Tags>::value(), QObject::tr("Tags"), buildTags},
        {entt::type_hash<engine::MeshRenderer>::value(), QObject::tr("Mesh Renderer"), buildMeshRenderer,
         addMeshRenderer, removeComponent<engine::MeshRenderer>},
        {entt::type_hash<engine::Camera>::value(), QObject::tr("Camera"), buildCamera, addComponent<engine::Camera>,
         removeComponent<engine::Camera>},
        {entt::type_hash<engine::DirectionalLight>::value(), QObject::tr("Directional Light"), buildDirectionalLight,
         addComponent<engine::DirectionalLight>, removeComponent<engine::DirectionalLight>},
        {entt::type_hash<engine::PointLight>::value(), QObject::tr("Point Light"), buildPointLight,
         addComponent<engine::PointLight>, removeComponent<engine::PointLight>},
        {entt::type_hash<engine::SpotLight>::value(), QObject::tr("Spot Light"), buildSpotLight,
         addComponent<engine::SpotLight>, removeComponent<engine::SpotLight>},
        {entt::type_hash<engine::Bounds>::value(), QObject::tr("Bounds"), buildBounds, addBounds,
         removeComponent<engine::Bounds>},
        {entt::type_hash<engine::GamePersistence>::value(), QObject::tr("Game Persistence"), buildGamePersistence,
         addComponent<engine::GamePersistence>, removeComponent<engine::GamePersistence>},
    };
    return inspectors;
}

bool isHiddenComponent(const entt::id_type type) {
    return type == entt::type_hash<engine::Name>::value() || type == entt::type_hash<engine::EntityId>::value() ||
           type == entt::type_hash<engine::Hierarchy>::value();
}
