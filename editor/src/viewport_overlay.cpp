#include <editor/viewport_overlay.hpp>

#include <QWidget>

#include <algorithm>
#include <cmath>
#include <string>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/rendering/DebugDraw.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/selection.hpp>

namespace editor {

namespace {

using DirectX::SimpleMath::Color;
using DirectX::SimpleMath::Vector3;

const Color ColliderColor{0.35f, 0.95f, 0.45f};
const Color LightColor{1.0f, 0.8f, 0.25f};
/// The orientation axes' length and their center's distance from the corner, in logical pixels.
constexpr float AxesLength = 34.0f;
constexpr float AxesMargin = 60.0f;

/// The reach of an entity's lights, or for a directional light where it shines.
void drawLights(engine::DebugDraw& draw, const engine::Scene& scene, const entt::entity entity) {
    const auto matrix = scene.GetWorldMatrix(entity);
    const Vector3 origin = matrix.Translation();
    if (const auto* point = scene.TryGetComponent<engine::PointLight>(entity);
        point && std::isfinite(point->range) && point->range > 0.0f) {
        // The range is in world units, whatever the entity's scale.
        draw.Sphere(origin, point->range, LightColor);
    }
    Vector3 forward = Vector3::TransformNormal(Vector3::Forward, matrix);
    Vector3 right = Vector3::TransformNormal(Vector3::Right, matrix);
    if (forward.LengthSquared() <= 0.0f) forward = Vector3::Forward;
    forward.Normalize();
    right -= forward * right.Dot(forward);
    if (right.LengthSquared() <= 0.0f) right = forward.Cross(Vector3::Up);
    if (right.LengthSquared() <= 0.0f) right = forward.Cross(Vector3::Right);
    right.Normalize();
    Vector3 up = right.Cross(forward);
    up.Normalize();
    if (const auto* spot = scene.TryGetComponent<engine::SpotLight>(entity);
        spot && std::isfinite(spot->range) && spot->range > 0.0f && std::isfinite(spot->outerHalfAngle)) {
        // Cone clipped by the light's spherical range, as in the lighting shader.
        const float angle = std::clamp(spot->outerHalfAngle, 0.0f, 1.5707963f);
        const float depth = spot->range * std::cos(angle);
        const float radius = spot->range * std::sin(angle);
        const Vector3 end = origin + forward * depth;
        draw.Circle(end, right, up, radius, LightColor);
        for (const Vector3 rim : {right, -right, up, -up}) draw.Line(origin, end + rim * radius, LightColor);
        draw.Line(origin, end, LightColor);
    }
    if (scene.HasComponent<engine::DirectionalLight>(entity)) {
        draw.Arrow(origin - forward * 0.5f, origin + forward * 1.5f, 0.35f, LightColor);
    }
}

}  // namespace

ViewportOverlaySubsystem::ViewportOverlaySubsystem(const Selection& selection, const QWidget& viewport)
    : selection_(selection), viewport_(viewport) {}

std::string_view ViewportOverlaySubsystem::GetName() const {
    return "ViewportOverlaySubsystem";
}

void ViewportOverlaySubsystem::LateUpdate(const engine::FrameContext&) {
    auto& world = GetEngine().GetSubsystem<engine::RenderingSubsystem>()->GetWorld();
    world.selectedSceneItems.clear();
    // In the bottom-left corner.
    const auto scale = static_cast<float>(viewport_.devicePixelRatioF());
    const auto height = static_cast<float>(GetEngine().GetSurface().height);
    world.debugDraw.ScreenAxes(world.camera.GetViewMatrix(), {AxesMargin * scale, height - AxesMargin * scale},
                               AxesLength * scale);
    auto& assets = GetEngine().GetSceneAssets();
    const auto& scene = GetEngine().GetScene();
    if (auto* physics = GetEngine().GetSubsystem<engine::PhysicsSubsystem>()) {
        std::string ignored;
        // The inspector edits the primary entity. Preview also works in Edit,
        // without registering simulation bodies or advancing the fixed clock.
        (void)physics->DrawCollider(selection_.primary(), world.debugDraw, ColliderColor, ignored);
    }
    for (const auto entity : selection_.entities()) {
        if (!scene.IsValid(entity)) continue;
        drawLights(world.debugDraw, scene, entity);
        if (const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity)) {
            const auto mesh = assets.Peek(renderer->mesh);
            const auto material = assets.Peek(renderer->materialInstance);
            if (mesh && material) {
                const auto override = GetEngine().GetEntityMaterialOverride(entity);
                world.selectedSceneItems.push_back({scene.GetWorldMatrix(entity), mesh->mesh,
                                                    override ? override : material->instance});
            }
        }
    }
}

void ViewportOverlaySubsystem::Shutdown() {
    GetEngine().GetSubsystem<engine::RenderingSubsystem>()->GetWorld().selectedSceneItems.clear();
}

}  // namespace editor
