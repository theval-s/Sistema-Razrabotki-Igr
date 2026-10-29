#include <engine/scene/Scene.hpp>

#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/rendering/RenderComponents.hpp>

namespace engine {

Scene::Scene() {
    // Pools must be instantiated in Engine.dll before editor modules add components.
    static_cast<void>(registry_.storage<MeshRenderer>());
    static_cast<void>(registry_.storage<Camera>());
    static_cast<void>(registry_.storage<DirectionalLight>());
}
Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

// --- Entities -------------------------------------------------------------------

entt::entity Scene::CreateEntity(const std::string_view name, const entt::entity parent) {
    return CreateEntityWithId(NewUuid(), name, parent);
}

entt::entity Scene::CreateEntityWithId(const std::string_view id, const std::string_view name,
                                       const entt::entity parent) {
    if (!IsUuid(id) || ids_.contains(std::string(id))) {
        spdlog::error("Scene::CreateEntityWithId: invalid or duplicate UUID '{}'", id);
        return entt::null;
    }
    if (parent != entt::null && !CheckEntity(parent, "CreateEntity")) {
        return entt::null;
    }

    // we can also just use plain vectors but entt registry looks good
    const entt::entity entity = registry_.create();
    registry_.emplace<EntityId>(entity, std::string(id));
    registry_.emplace<Name>(entity, std::string(name));
    registry_.emplace<Transform>(entity);
    registry_.emplace<Hierarchy>(entity);
    Attach(entity, parent);
    ids_.emplace(id, entity);
    ++revision_;
    return entity;
}

void Scene::DestroyEntity(const entt::entity entity) {
    if (!CheckEntity(entity, "DestroyEntity")) {
        return;
    }

    // kill all children yes yes
    const std::vector<entt::entity> children = registry_.get<Hierarchy>(entity).children;
    for (const entt::entity child : children) {
        DestroyEntity(child);
    }

    Detach(entity);
    const auto& id = registry_.get<EntityId>(entity).value;
    if (id == activeCameraId_) activeCameraId_.clear();
    if (id == directionalLightId_) directionalLightId_.clear();
    ids_.erase(registry_.get<EntityId>(entity).value);
    unsupported_.erase(entity);
    registry_.destroy(entity);
    ++revision_;
}

void Scene::Clear() {
    const std::vector<entt::entity> roots = roots_;
    for (const entt::entity root : roots) {
        DestroyEntity(root);
    }
    registry_.clear();  // anything created behind Scene's back
    ids_.clear();
    unsupported_.clear();
    activeCameraId_.clear();
    directionalLightId_.clear();
    ++revision_;
}

bool Scene::IsValid(const entt::entity entity) const {
    return registry_.valid(entity) && registry_.all_of<Name, Transform, Hierarchy, EntityId>(entity);
}

std::string_view Scene::GetId(const entt::entity entity) const {
    return IsValid(entity) ? registry_.get<EntityId>(entity).value : std::string_view{};
}

entt::entity Scene::FindById(const std::string_view id) const {
    const auto it = ids_.find(std::string(id));
    return it == ids_.end() ? entt::null : it->second;
}

void Scene::SetActiveCamera(entt::entity entity) {
    if (entity != entt::null && !HasComponent<Camera>(entity)) return;
    activeCameraId_ = entity == entt::null ? "" : std::string(GetId(entity));
    ++revision_;
}

entt::entity Scene::GetActiveCamera() const {
    const auto entity = FindById(activeCameraId_);
    return HasComponent<Camera>(entity) ? entity : FirstWith<Camera>();
}

void Scene::SetDirectionalLight(entt::entity entity) {
    if (entity != entt::null && !HasComponent<DirectionalLight>(entity)) return;
    directionalLightId_ = entity == entt::null ? "" : std::string(GetId(entity));
    ++revision_;
}

entt::entity Scene::GetDirectionalLight() const {
    const auto entity = FindById(directionalLightId_);
    return HasComponent<DirectionalLight>(entity) ? entity : FirstWith<DirectionalLight>();
}

const std::string& Scene::GetName(const entt::entity entity) const {
    if (!CheckEntity(entity, "GetName")) {
        static const std::string empty;
        return empty;
    }
    return registry_.get<Name>(entity).value;
}

void Scene::SetName(const entt::entity entity, const std::string_view name) {
    if (!CheckEntity(entity, "SetName")) {
        return;
    }
    registry_.get<Name>(entity).value = name;
    ++revision_;
}

// --- Transform & hierarchy -----------------------------------------------------

Transform& Scene::GetTransform(const entt::entity entity) {
    return const_cast<Transform&>(std::as_const(*this).GetTransform(entity));
}

const Transform& Scene::GetTransform(const entt::entity entity) const {
    if (!CheckEntity(entity, "GetTransform")) {
        //TODO: handle it somehow, maybe throw error event system
        spdlog::critical("Scene::GetTransform on an invalid entity, exiting");
        std::exit(EXIT_FAILURE);
    }
    return registry_.get<Transform>(entity);
}

DirectX::SimpleMath::Matrix Scene::GetWorldMatrix(const entt::entity entity) const {
    if (!CheckEntity(entity, "GetWorldMatrix")) {
        return DirectX::SimpleMath::Matrix::Identity;
    }
    DirectX::SimpleMath::Matrix world = registry_.get<Transform>(entity).ToMatrix();
    if (const entt::entity parent = registry_.get<Hierarchy>(entity).parent; parent != entt::null) {
        world *= GetWorldMatrix(parent);
    }
    return world;
}

entt::entity Scene::GetParent(const entt::entity entity) const {
    if (!CheckEntity(entity, "GetParent")) {
        return entt::null;
    }
    return registry_.get<Hierarchy>(entity).parent;
}

bool Scene::SetParent(const entt::entity entity, const entt::entity parent) {
    if (!CheckEntity(entity, "SetParent")) {
        return false;
    }
    if (parent != entt::null) {
        if (!CheckEntity(parent, "SetParent")) {
            return false;
        }
        for (entt::entity ancestor = parent; ancestor != entt::null;
             ancestor = registry_.get<Hierarchy>(ancestor).parent) {
            if (ancestor == entity) {
                return false;
            }
        }
    }
    if (registry_.get<Hierarchy>(entity).parent == parent) {
        return true;
    }

    Detach(entity);
    Attach(entity, parent);
    ++revision_;
    return true;
}

std::span<const entt::entity> Scene::GetChildren(const entt::entity entity) const {
    if (!CheckEntity(entity, "GetChildren")) {
        return {};
    }
    return registry_.get<Hierarchy>(entity).children;
}

std::span<const entt::entity> Scene::GetRootEntities() const noexcept {
    return roots_;
}

// --- Internals -------------------------------------------------------------------

bool Scene::CheckEntity(const entt::entity entity, const std::string_view caller) const {
    if (IsValid(entity)) {
        return true;
    }
    spdlog::error("Scene::{}: invalid or destroyed entity {}", caller, entt::to_integral(entity));
    return false;
}

void Scene::Detach(const entt::entity entity) {
    auto& hierarchy = registry_.get<Hierarchy>(entity);
    auto& siblings = hierarchy.parent != entt::null ? registry_.get<Hierarchy>(hierarchy.parent).children : roots_;
    std::erase(siblings, entity);
    hierarchy.parent = entt::null;
}

void Scene::Attach(const entt::entity entity, const entt::entity parent) {
    registry_.get<Hierarchy>(entity).parent = parent;
    auto& siblings = parent != entt::null ? registry_.get<Hierarchy>(parent).children : roots_;
    siblings.push_back(entity);
}

}  // namespace engine
