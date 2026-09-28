#include <engine/scene/Scene.hpp>

#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>

namespace engine {

Scene::Scene() = default;
Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

// --- Entities -------------------------------------------------------------------

entt::entity Scene::CreateEntity(const std::string_view name, const entt::entity parent) {
    if (parent != entt::null && !CheckEntity(parent, "CreateEntity")) {
        return entt::null;
    }

    // we can also just use plain vectors but entt registry looks good
    const entt::entity entity = registry_.create();
    registry_.emplace<Name>(entity, std::string(name));
    registry_.emplace<Transform>(entity);
    registry_.emplace<Hierarchy>(entity);
    Attach(entity, parent);
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
    registry_.destroy(entity);
    ++revision_;
}

void Scene::Clear() {
    const std::vector<entt::entity> roots = roots_;
    for (const entt::entity root : roots) {
        DestroyEntity(root);
    }
    registry_.clear();  // anything created behind Scene's back
    ++revision_;
}

bool Scene::IsValid(const entt::entity entity) const {
    return registry_.valid(entity) && registry_.all_of<Hierarchy>(entity);
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
