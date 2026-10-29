#include <engine/rendering/RenderExtractionSubsystem.hpp>

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>

namespace engine {

std::string_view RenderExtractionSubsystem::GetName() const { return "RenderExtractionSubsystem"; }

bool RenderExtractionSubsystem::Initialize() {
    rendering_ = GetEngine().GetSubsystem<RenderingSubsystem>();
    if (!rendering_) spdlog::error("RenderExtractionSubsystem needs RenderingSubsystem");
    return rendering_ != nullptr;
}

void RenderExtractionSubsystem::Shutdown() {
    if (rendering_) rendering_->GetWorld().sceneItems.clear();
    rendering_ = nullptr;
}

void RenderExtractionSubsystem::Update(const FrameContext&) {
    using DirectX::SimpleMath::Vector3;
    const Scene& scene = GetEngine().GetScene();
    AssetSystem& assets = GetEngine().GetSceneAssets();
    RenderWorld& world = rendering_->GetWorld();
    world.sceneItems.clear();
    scene.ForEach<MeshRenderer>([&](entt::entity entity, const MeshRenderer& renderer) {
        const auto mesh = assets.Peek(renderer.mesh);
        const auto instance = assets.Peek(renderer.materialInstance);
        if (mesh && instance) world.sceneItems.push_back({scene.GetWorldMatrix(entity), mesh->mesh,
            renderer.materialOverride ? renderer.materialOverride : instance->instance});
    });

    if (const NativeSurface& surface = GetEngine().GetSurface(); surface.width && surface.height)
        world.camera.aspectRatio = static_cast<float>(surface.width) / static_cast<float>(surface.height);
    if (const entt::entity entity = scene.GetActiveCamera(); entity != entt::null) {
        const Camera& camera = *scene.TryGetComponent<Camera>(entity);
        const auto matrix = scene.GetWorldMatrix(entity);
        Vector3 forward = Vector3::TransformNormal(Vector3::Forward, matrix);
        if (forward.LengthSquared() > 0) forward.Normalize();
        world.camera.position = matrix.Translation();
        world.camera.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        world.camera.yaw = std::atan2(-forward.x, -forward.z);
        world.camera.fov = camera.fovY;
        world.camera.nearPlane = camera.nearPlane;
        world.camera.farPlane = camera.farPlane;
    }
    world.directionalLight = {};
    world.directionalLight.type = LightInfo::Directional;
    if (const entt::entity entity = scene.GetDirectionalLight(); entity != entt::null) {
        const auto& light = *scene.TryGetComponent<DirectionalLight>(entity);
        Vector3 direction = Vector3::TransformNormal(Vector3::Forward, scene.GetWorldMatrix(entity));
        if (direction.LengthSquared() > 0) direction.Normalize();
        world.directionalLight.direction = direction;
        world.directionalLight.color = light.color;
        world.directionalLight.intensity = light.intensity;
    }
}
} // namespace engine
