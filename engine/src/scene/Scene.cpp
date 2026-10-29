#include <engine/scene/Scene.hpp>

#include <cstdlib>
#include <algorithm>
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
    static_cast<void>(registry_.storage<Bounds>());
}
Scene::~Scene() { Clear(); }

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
    registry_.emplace<Tags>(entity);
    registry_.emplace<Transform>(entity);
    registry_.emplace<Hierarchy>(entity);
    Attach(entity, parent);
    ids_.emplace(id, entity);
    ++revision_;
    return entity;
}

void Scene::DestroyEntity(const entt::entity entity) {
    if (inControllerCallback_) {
        pendingControllerOperations_.push_back([this, entity] { if (IsValid(entity)) DestroyEntity(entity); });
        return;
    }
    if (!CheckEntity(entity, "DestroyEntity")) {
        return;
    }
    ++destructionDepth_;

    // kill all children yes yes
    const std::vector<entt::entity> children = registry_.get<Hierarchy>(entity).children;
    for (const entt::entity child : children) {
        DestroyEntity(child);
    }

    DestroyController(entity);
    ReleaseMaterialOverride(entity);

    Detach(entity);
    const auto& id = registry_.get<EntityId>(entity).value;
    if (id == activeCameraId_) activeCameraId_.clear();
    if (id == directionalLightId_) directionalLightId_.clear();
    ids_.erase(registry_.get<EntityId>(entity).value);
    unsupported_.erase(entity);
    registry_.destroy(entity);
    ++revision_;
    if (--destructionDepth_ == 0) DrainControllerOperations();
}

void Scene::Clear() {
    if (inControllerCallback_) {
        pendingControllerOperations_.push_back([this] { Clear(); });
        return;
    }
    const std::vector<entt::entity> roots = roots_;
    for (const entt::entity root : roots) {
        DestroyEntity(root);
    }
    registry_.clear();  // anything created behind Scene's back
    ids_.clear();
    unsupported_.clear();
    controllers_.clear();
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

void Scene::Visit(const std::function<void(entt::entity)>& callback) const {
    auto walk = [&](auto&& self, entt::entity entity) -> void {
        callback(entity);
        for (const auto child : registry_.get<Hierarchy>(entity).children) self(self, child);
    };
    for (const auto root : roots_) walk(walk, root);
}

entt::entity Scene::FindFirstByName(std::string_view name) const {
    entt::entity found = entt::null;
    Visit([&](entt::entity entity) { if (found == entt::null && GetName(entity) == name) found = entity; });
    return found;
}

std::vector<entt::entity> Scene::FindAllByName(std::string_view name) const {
    std::vector<entt::entity> found;
    Visit([&](entt::entity entity) { if (GetName(entity) == name) found.push_back(entity); });
    return found;
}

std::vector<entt::entity> Scene::FindWithTag(std::string_view tag) const {
    std::vector<entt::entity> found;
    Visit([&](entt::entity entity) {
        const auto values = GetTags(entity);
        if (std::find(values.begin(), values.end(), tag) != values.end()) found.push_back(entity);
    });
    return found;
}

std::span<const std::string> Scene::GetTags(entt::entity entity) const {
    return IsValid(entity) ? std::span<const std::string>(registry_.get<Tags>(entity).values) : std::span<const std::string>{};
}

void Scene::AddTag(entt::entity entity, std::string_view tag) {
    if (!IsValid(entity) || tag.empty()) return;
    auto& values = registry_.get<Tags>(entity).values;
    if (std::find(values.begin(), values.end(), tag) == values.end()) { values.emplace_back(tag); ++revision_; }
}

void Scene::RemoveTag(entt::entity entity, std::string_view tag) {
    if (!IsValid(entity)) return;
    auto& values = registry_.get<Tags>(entity).values;
    const auto size = values.size();
    std::erase(values, tag);
    if (values.size() != size) ++revision_;
}

bool Scene::RegisterController(std::string_view type, ControllerRegistration registration) {
    if (type.empty() || registration.version < 1 || !registration.create || controllerTypes_.contains(std::string(type)))
        return false;
    auto [it, inserted] = controllerTypes_.emplace(type, std::move(registration));
    (void)inserted;
    // Previously unavailable definitions become usable after registration.
    for (auto& [entity, slot] : controllers_)
        if (slot.definition.type == type && !slot.instance) {
            const auto definition = slot.definition;
            pendingControllerOperations_.push_back([this, entity, definition] { AttachController(entity, definition); });
        }
    DrainControllerOperations();
    return true;
}

bool Scene::ValidateController(const ControllerDefinition& definition, std::string& error) const {
    auto it = controllerTypes_.find(definition.type);
    if (it == controllerTypes_.end() || it->second.version != definition.version) {
        error = "unavailable controller '" + definition.type + "' version " + std::to_string(definition.version);
        return false;
    }
    if (!it->second.validate) { error.clear(); return true; }
    try {
        if (it->second.validate(definition.data, error)) { error.clear(); return true; }
        if (error.empty()) error = "controller rejected its authored data";
    } catch (const std::exception& e) { error = e.what(); }
    return false;
}

void Scene::AttachController(entt::entity entity, ControllerDefinition definition) {
    if (inControllerCallback_) {
        pendingControllerOperations_.push_back([this, entity, definition = std::move(definition)]() mutable {
            AttachController(entity, std::move(definition));
        });
        return;
    }
    if (!IsValid(entity)) return;
    DestroyController(entity);
    auto [it, inserted] = controllers_.emplace(entity, ControllerSlot{std::move(definition)});
    (void)inserted;
    auto& slot = it->second;
    std::string error;
    if (ValidateController(slot.definition, error)) {
        try {
            slot.instance = controllerTypes_.at(slot.definition.type).create(slot.definition.data);
            if (slot.instance) {
                slot.instance->scene_ = this;
                slot.instance->entity_ = entity;
                inControllerCallback_ = true;
                slot.instance->OnCreate();
                if (controllersActive_ && slot.definition.enabled) {
                    slot.active = true;
                    slot.instance->OnEnable();
                }
                inControllerCallback_ = false;
            } else error = "controller factory returned null";
        } catch (const std::exception& e) {
            error = e.what();
            if (slot.instance) {
                try { if (slot.active) slot.instance->OnDisable(); } catch (const std::exception&) {}
                try { slot.instance->OnDestroy(); } catch (const std::exception&) {}
                slot.instance.reset();
                slot.active = false;
            }
            inControllerCallback_ = false;
        }
    }
    if (!error.empty()) spdlog::error("Scene controller on {}: {}", GetId(entity), error);
    ++revision_;
    DrainControllerOperations();
}

void Scene::DestroyController(entt::entity entity) {
    auto it = controllers_.find(entity);
    if (it == controllers_.end()) return;
    if (it->second.instance) {
        inControllerCallback_ = true;
        if (it->second.active) {
            try { it->second.instance->OnDisable(); }
            catch (const std::exception& e) { spdlog::error("Controller disable: {}", e.what()); }
        }
        try { it->second.instance->OnDestroy(); }
        catch (const std::exception& e) { spdlog::error("Controller destroy: {}", e.what()); }
        inControllerCallback_ = false;
    }
    controllers_.erase(it);
    ++revision_;
}

void Scene::RemoveController(entt::entity entity) {
    if (inControllerCallback_) {
        pendingControllerOperations_.push_back([this, entity] { RemoveController(entity); });
        return;
    }
    DestroyController(entity);
    DrainControllerOperations();
}

void Scene::SetControllerEnabled(entt::entity entity, bool enabled) {
    if (inControllerCallback_) {
        pendingControllerOperations_.push_back([this, entity, enabled] { SetControllerEnabled(entity, enabled); });
        return;
    }
    auto it = controllers_.find(entity);
    if (it == controllers_.end() || it->second.definition.enabled == enabled) return;
    it->second.definition.enabled = enabled;
    auto& slot = it->second;
    if (slot.instance && controllersActive_) {
        inControllerCallback_ = true;
        try {
            if (enabled) { slot.active = true; slot.instance->OnEnable(); }
            else { slot.active = false; slot.instance->OnDisable(); }
        } catch (const std::exception& e) { spdlog::error("Controller enable: {}", e.what()); }
        inControllerCallback_ = false;
    }
    ++revision_;
    DrainControllerOperations();
}

const ControllerDefinition* Scene::GetControllerDefinition(entt::entity entity) const {
    const auto it = controllers_.find(entity);
    return it == controllers_.end() ? nullptr : &it->second.definition;
}

void Scene::SetControllerAuthoredData(entt::entity entity, std::string data) {
    if (auto it = controllers_.find(entity); it != controllers_.end()) {
        it->second.definition.data = std::move(data);
        ++revision_;
    }
}

void Scene::SetControllersActive(bool active) {
    if (inControllerCallback_) {
        pendingControllerOperations_.push_back([this, active] { SetControllersActive(active); });
        return;
    }
    controllersActive_ = active;
    Visit([&](entt::entity entity) {
        auto it = controllers_.find(entity);
        if (it == controllers_.end() || !it->second.instance || !it->second.definition.enabled || it->second.active == active) return;
        inControllerCallback_ = true;
        try {
            it->second.active = active;
            if (active) it->second.instance->OnEnable();
            else it->second.instance->OnDisable();
        } catch (const std::exception& e) { spdlog::error("Controller activation: {}", e.what()); }
        inControllerCallback_ = false;
    });
    DrainControllerOperations();
}

void Scene::UpdateControllers(double deltaSeconds) {
    if (!controllersActive_ || deltaSeconds <= 0) return;
    std::vector<entt::entity> order;
    Visit([&](entt::entity entity) { order.push_back(entity); });
    inControllerCallback_ = true;
    for (const auto entity : order) {
        auto it = controllers_.find(entity);
        if (it != controllers_.end() && it->second.active) {
            try { it->second.instance->Update(deltaSeconds); }
            catch (const std::exception& e) { spdlog::error("Controller update: {}", e.what()); }
        }
    }
    inControllerCallback_ = false;
    DrainControllerOperations();
}

void Scene::DrainControllerOperations() {
    if (inControllerCallback_ || destructionDepth_ != 0) return;
    while (!pendingControllerOperations_.empty()) {
        auto operations = std::move(pendingControllerOperations_);
        pendingControllerOperations_.clear();
        for (auto& operation : operations) operation();
    }
}

void Scene::SetMaterialOverrideReleaser(std::function<void(MaterialInstanceHandle)> releaser) {
    materialOverrideReleaser_ = std::move(releaser);
}

void Scene::ReleaseMaterialOverride(entt::entity entity) {
    if (auto* renderer = registry_.try_get<MeshRenderer>(entity); renderer && renderer->materialOverride) {
        if (materialOverrideReleaser_) materialOverrideReleaser_(renderer->materialOverride);
        renderer->materialOverride = {};
    }
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
