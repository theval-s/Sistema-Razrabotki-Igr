#include <engine/rendering/RenderExtractionSubsystem.hpp>

#include <algorithm>
#include <cmath>

#include <engine/core/Log.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>

namespace engine {

entt::resource<MaterialInstanceAsset> SectionMaterial(AssetSystem& assets, const MeshRenderer& renderer,
                                                      const MeshAsset& mesh, const MeshSection& section) {
    // A renderer whose own material is missing stays undrawn rather than silently changing look.
    if (HasOwnMaterial(renderer)) return assets.Peek(renderer.materialInstance);
    if (section.materialSlot >= mesh.materials.size()) return {};
    return assets.Peek(mesh.materials[section.materialSlot]);
}

entt::resource<MaterialInstanceAsset> PrimaryMaterial(AssetSystem& assets, const MeshRenderer& renderer) {
    if (HasOwnMaterial(renderer)) return assets.Peek(renderer.materialInstance);
    const auto mesh = assets.Peek(renderer.mesh);
    if (!mesh || mesh->sections.empty()) return {};
    return SectionMaterial(assets, renderer, *mesh, mesh->sections.front());
}

std::string_view RenderExtractionSubsystem::GetName() const { return "RenderExtractionSubsystem"; }

bool RenderExtractionSubsystem::Initialize() {
    rendering_ = GetEngine().GetSubsystem<RenderingSubsystem>();
    if (!rendering_) log::Render().error("RenderExtractionSubsystem needs RenderingSubsystem");
    return rendering_ != nullptr;
}

void RenderExtractionSubsystem::Shutdown() {
    if (rendering_) {
        rendering_->GetWorld().sceneItems.clear();
        rendering_->GetWorld().lights.clear();
    }
    rendering_ = nullptr;
}

void RenderExtractionSubsystem::LateUpdate(const FrameContext&) {
    using DirectX::SimpleMath::Vector3;
    const Scene& scene = GetEngine().GetScene();
    AssetSystem& assets = GetEngine().GetSceneAssets();
    RenderWorld& world = rendering_->GetWorld();
    world.sceneItems.clear();
    world.lights.clear();
    scene.ForEach<MeshRenderer>([&](entt::entity entity, const MeshRenderer& renderer) {
        const auto mesh = assets.Peek(renderer.mesh);
        if (!mesh) return;
        const auto override = GetEngine().GetEntityMaterialOverride(entity);
        for (const auto& section : mesh->sections) {
            if (const auto instance = SectionMaterial(assets, renderer, *mesh, section))
                world.sceneItems.push_back({scene.GetWorldMatrix(entity), section.mesh,
                    override ? override : instance->instance});
        }
    });

    const auto* animation = GetEngine().GetSubsystem<AnimationSubsystem>();
    scene.ForEach<SkinnedMeshRenderer>([&](entt::entity entity, const SkinnedMeshRenderer& renderer) {
        const auto mesh = assets.Peek(renderer.mesh);
        const auto material = assets.Peek(renderer.materialInstance);
        if (!mesh || !material) return;
        const auto pose = animation ? animation->GetPose(entity) : nullptr;
        const auto override = GetEngine().GetEntityMaterialOverride(entity);
        for (const auto& section : mesh->sections) {
            if (!section.binding) continue;
            auto palette = BuildSkinningPalette(*section.binding, mesh->skeletonSignature, pose.get());
            if (palette) world.sceneItems.push_back({scene.GetWorldMatrix(entity), section.mesh,
                override ? override : material->instance, std::move(palette)});
        }
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
    // A scene without a sun still runs the shadow/view passes. Keep their view
    // basis valid while the zero intensity disables directional lighting.
    world.directionalLight.direction = Vector3::Forward;
    if (const entt::entity entity = scene.GetDirectionalLight(); entity != entt::null) {
        const auto& light = *scene.TryGetComponent<DirectionalLight>(entity);
        Vector3 direction = Vector3::TransformNormal(Vector3::Forward, scene.GetWorldMatrix(entity));
        if (direction.LengthSquared() > 0) direction.Normalize();
        else direction = Vector3::Forward;
        world.directionalLight.direction = direction;
        world.directionalLight.color = light.color;
        world.directionalLight.intensity = light.intensity;
    }
    scene.ForEach<PointLight>([&](entt::entity entity, const PointLight& light) {
        LightInfo info{};
        info.type = LightInfo::Point;
        info.position = scene.GetWorldMatrix(entity).Translation();
        info.color = light.color;
        info.intensity = light.intensity;
        info.range = light.range;
        world.lights.push_back(info);
    });
    scene.ForEach<SpotLight>([&](entt::entity entity, const SpotLight& light) {
        const auto matrix = scene.GetWorldMatrix(entity);
        Vector3 direction = Vector3::TransformNormal(Vector3::Forward, matrix);
        if (direction.LengthSquared() > 0) direction.Normalize();
        LightInfo info{};
        info.type = LightInfo::Spot;
        info.position = matrix.Translation();
        info.direction = direction;
        info.color = light.color;
        info.intensity = light.intensity;
        info.range = light.range;
        info.spotInnerAngle = std::cos(light.innerHalfAngle);
        info.spotOuterAngle = std::cos(light.outerHalfAngle);
        world.lights.push_back(info);
    });
}
} // namespace engine
