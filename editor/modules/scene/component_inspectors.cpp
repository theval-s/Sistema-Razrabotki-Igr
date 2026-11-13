#include <QMessageBox>
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
#include <QComboBox>
#include <QStandardItemModel>
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
#include <engine/physics/PhysicsComponents.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/asset_picker.hpp>
#include <editor/editor_context.hpp>
#include <editor/scene_document.hpp>
#include <editor/scene_history.hpp>

#include "property_form.hpp"

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
                     [&context, &scene, &engine, entity, label, picker, kind, reference, handle, request] {
                         const auto* renderer = std::as_const(scene).TryGetComponent<engine::MeshRenderer>(entity);
                         const std::optional<engine::AssetReference> chosen = picker->reference();
                         if (renderer == nullptr || !chosen || chosen->id == (renderer->*reference).id) {
                             return;
                         }
                         context.history().edit(QObject::tr("Change %1").arg(label), [&] {
                             // Asynchronous, like adding the component: the entity isn't drawn until it's loaded.
                             const auto loaded = (engine.GetSceneAssets().*request)(*chosen);
                             scene.Modify<engine::MeshRenderer>(entity, [&](engine::MeshRenderer& changed) {
                                 changed.*handle = loaded;
                                 changed.*reference = *chosen;
                             });
                             if (kind == engine::CatalogAssetKind::MaterialInstance) {
                                 engine.ClearEntityMaterialOverride(entity);
                             }
                         });
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
    auto* bake = new QPushButton(QObject::tr("Bake Navigation Mesh"));
    QObject::connect(bake, &QPushButton::clicked, bake, [&context, entity, bake] {
        std::string error;
        if (!context.engine().BuildNavigation(entity, error))
            QMessageBox::warning(bake, QObject::tr("Navigation Region"), QString::fromStdString(error));
    });
    form.addRow({}, bake, [&context, bake] { bake->setEnabled(!context.engine().IsPlaying()); });
}

void buildNavigationSource(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    form.addBool(QObject::tr("Use Collider (off: Mesh Renderer)"),
                 field(context.scene(), entity, &engine::NavigationSource::useCollider));
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

void buildScript(PropertyForm& form, editor::EditorContext& context, entt::entity entity) {
    auto& scene = context.scene();
    auto& catalog = context.engine().GetAssets().GetCatalog();
    form.addBool(QObject::tr("Enabled"), field(scene, entity, &engine::ScriptComponent::enabled));
    auto* picker = new editor::AssetPicker(catalog, engine::CatalogAssetKind::Script, QObject::tr("None"));
    picker->setMinimumContentsLength(12);  // as in addAssetSlot: a wider row widens every component's form
    QObject::connect(picker, &QComboBox::currentIndexChanged, picker, [&context, &scene, entity, picker](const int index) {
        const auto* component = std::as_const(scene).TryGetComponent<engine::ScriptComponent>(entity);
        // -1 means nothing is chosen
        if (component == nullptr || index < 0) {
            return;
        }
        // "None" is chosen, so no reference.
        const engine::AssetReference chosen = picker->reference().value_or(engine::AssetReference{});
        if (chosen.id == component->script.id) {
            return;
        }
        context.history().edit(QObject::tr("Change Script"), [&] {
            scene.Modify<engine::ScriptComponent>(entity, [&chosen](engine::ScriptComponent& changed) {
                changed.script = chosen;
                changed.properties.clear();
            });
        });
    });
    form.addRow(QObject::tr("Script"), picker, [&scene, entity, picker] {
        const auto* component = scene.TryGetComponent<engine::ScriptComponent>(entity);
        if (!component || picker->hasFocus() || picker->lineEdit()->hasFocus() || picker->view()->isVisible()) {
            return;
        }
        const QSignalBlocker blocker(picker);
        if (!component->script.id.IsValid()) {
            picker->setCurrentIndex(0);  // "None"
            return;
        }
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

}  // namespace

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

std::span<const ComponentInspector> componentInspectors() {
    // maybe not necessary? I doubt we're gonna be caring about translation soon
    static const std::vector<ComponentInspector> inspectors{
        {entt::type_hash<engine::ScriptComponent>::value(), QObject::tr("Script"), buildScript,
         addComponent<engine::ScriptComponent>, removeComponent<engine::ScriptComponent>},
        {entt::type_hash<engine::Transform>::value(), QObject::tr("Transform"), buildTransform},
        {entt::type_hash<engine::Tags>::value(), QObject::tr("Tags"), buildTags},
        {entt::type_hash<engine::MeshRenderer>::value(), QObject::tr("Mesh Renderer"), buildMeshRenderer,
         addMeshRenderer, removeComponent<engine::MeshRenderer>},
        {entt::type_hash<engine::NavigationRegion>::value(), QObject::tr("Navigation Region"), buildNavigationRegion,
         addComponent<engine::NavigationRegion>, removeComponent<engine::NavigationRegion>},
        {entt::type_hash<engine::NavigationSource>::value(), QObject::tr("Navigation Source"), buildNavigationSource,
         addComponent<engine::NavigationSource>, removeComponent<engine::NavigationSource>},
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
