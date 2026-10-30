#include "component_inspectors.hpp"

#include <QLineEdit>
#include <QObject>
#include <QSignalBlocker>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <entt/core/type_info.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/editor_context.hpp>

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

template <class Asset>
QString assetText(const engine::AssetSystem& assets, const engine::AssetReference& authored,
                  const engine::AssetHandle<Asset> handle, const engine::CatalogAssetKind kind) {
    if (authored.id.IsValid()) {
        if (const engine::CatalogEntry* entry = assets.GetCatalog().Find(authored.id, kind)) {
            return QString::fromStdString(entry->path);
        }
        const std::string hint = authored.path.empty() ? authored.id.ToString() : authored.path;
        return QObject::tr("%1 (missing)").arg(QString::fromStdString(hint));
    }
    if (const std::optional<engine::AssetReference> loaded = assets.ReferenceFor(handle)) {
        return QString::fromStdString(loaded->path);
    }
    return QObject::tr("None");
}

void buildMeshRenderer(PropertyForm& form, editor::EditorContext& context, const entt::entity entity) {
    engine::Scene& scene = context.scene();
    engine::Engine& engine = context.engine();
    form.addText(QObject::tr("Mesh"), [&scene, &engine, entity] {
        const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity);
        return renderer != nullptr ? assetText(engine.GetSceneAssets(), renderer->meshReference, renderer->mesh,
                                               engine::CatalogAssetKind::Mesh)
                                   : QString();
    });
    form.addText(QObject::tr("Material"), [&scene, &engine, entity] {
        const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity);
        return renderer != nullptr
                   ? assetText(engine.GetSceneAssets(), renderer->materialReference, renderer->materialInstance,
                               engine::CatalogAssetKind::MaterialInstance)
                   : QString();
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

}  // namespace

std::span<const ComponentInspector> componentInspectors() {
    // maybe not necessary? I doubt we're gonna be caring about translation soon
    static const std::vector<ComponentInspector> inspectors{
        {entt::type_hash<engine::Transform>::value(), QObject::tr("Transform"), buildTransform},
        {entt::type_hash<engine::Tags>::value(), QObject::tr("Tags"), buildTags},
        {entt::type_hash<engine::MeshRenderer>::value(), QObject::tr("Mesh Renderer"), buildMeshRenderer},
        {entt::type_hash<engine::Camera>::value(), QObject::tr("Camera"), buildCamera},
        {entt::type_hash<engine::DirectionalLight>::value(), QObject::tr("Directional Light"), buildDirectionalLight},
        {entt::type_hash<engine::PointLight>::value(), QObject::tr("Point Light"), buildPointLight},
        {entt::type_hash<engine::SpotLight>::value(), QObject::tr("Spot Light"), buildSpotLight},
        {entt::type_hash<engine::Bounds>::value(), QObject::tr("Bounds"), buildBounds},
        {entt::type_hash<engine::GamePersistence>::value(), QObject::tr("Game Persistence"), buildGamePersistence},
    };
    return inspectors;
}

bool isHiddenComponent(const entt::id_type type) {
    return type == entt::type_hash<engine::Name>::value() || type == entt::type_hash<engine::EntityId>::value() ||
           type == entt::type_hash<engine::Hierarchy>::value();
}
