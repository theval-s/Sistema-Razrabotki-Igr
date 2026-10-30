#include <engine/scene/Scene.hpp>

#include <cstdlib>
#include <algorithm>
#include <utility>

#include <spdlog/spdlog.h>
#include <engine/rendering/RenderComponents.hpp>

namespace engine {

Scene::Scene() {
    // Pools must be instantiated in Engine.dll before editor modules add components.
    static_cast<void>(registry_.storage<MeshRenderer>());
    static_cast<void>(registry_.storage<Camera>());
    static_cast<void>(registry_.storage<DirectionalLight>());
    static_cast<void>(registry_.storage<PointLight>());
    static_cast<void>(registry_.storage<SpotLight>());
    static_cast<void>(registry_.storage<Bounds>());
    static_cast<void>(registry_.storage<GamePersistence>());
}
Scene::~Scene() { Clear(); }

// --- Entities -------------------------------------------------------------------

entt::entity Scene::CreateEntity(const std::string_view name, const entt::entity parent) {
    return CreateEntityWithUuid(EntityUuid(NewUuid()), name, parent);
}

entt::entity Scene::CreateEntityWithId(const std::string_view id, const std::string_view name,
                                       const entt::entity parent) {
    const auto parsed = EntityUuid::Parse(id);
    if (!parsed) {
        spdlog::error("Scene::CreateEntityWithId: invalid UUID '{}'", id);
        return entt::null;
    }
    return CreateEntityWithUuid(*parsed, name, parent);
}

entt::entity Scene::CreateEntityWithUuid(EntityUuid id, const std::string_view name,
                                         const entt::entity parent) {
    if (ids_.contains(id)) {
        spdlog::error("Scene::CreateEntityWithUuid: duplicate UUID '{}'", id.ToString());
        return entt::null;
    }
    if (parent != entt::null && !CheckEntity(parent, "CreateEntity")) {
        return entt::null;
    }

    // we can also just use plain vectors but entt registry looks good
    const entt::entity entity = registry_.create();
    registry_.emplace<EntityId>(entity, id);
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
        commands_.push_back({Command::Kind::DestroyEntity, entity});
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
    pendingGameStates_.erase(entity);
    NotifyLifecycle(LifecycleEvent::EntityDestroying, entity, 0);

    Detach(entity);
    const auto& id = registry_.get<EntityId>(entity).value;
    if (activeCameraId_ == id) activeCameraId_.reset();
    if (directionalLightId_ == id) directionalLightId_.reset();
    ids_.erase(registry_.get<EntityId>(entity).value);
    unsupported_.erase(entity);
    registry_.destroy(entity);
    ++revision_;
    if (--destructionDepth_ == 0) FlushCommands();
}

void Scene::Clear() {
    if (inControllerCallback_) {
        commands_.push_back({Command::Kind::Clear});
        return;
    }
    ++destructionDepth_;
    const std::vector<entt::entity> roots = roots_;
    for (const entt::entity root : roots) {
        DestroyEntity(root);
    }
    registry_.clear();  // anything created behind Scene's back
    ids_.clear();
    unsupported_.clear();
    controllers_.clear();
    pendingGameStates_.clear();
    activeCameraId_.reset();
    directionalLightId_.reset();
    ++revision_;
    if (--destructionDepth_ == 0) FlushCommands();
}

bool Scene::IsValid(const entt::entity entity) const {
    return registry_.valid(entity) && registry_.all_of<Name, Transform, Hierarchy, EntityId>(entity);
}

EntityUuid Scene::GetId(const entt::entity entity) const {
    return IsValid(entity) ? registry_.get<EntityId>(entity).value : EntityUuid{};
}

entt::entity Scene::FindByUuid(EntityUuid id) const {
    const auto it = ids_.find(id);
    return it == ids_.end() ? entt::null : it->second;
}

entt::entity Scene::FindById(const std::string_view id) const {
    const auto parsed = EntityUuid::Parse(id);
    return parsed ? FindByUuid(*parsed) : entt::null;
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
            commands_.push_back({Command::Kind::AttachController, entity, definition});
        }
    FlushCommands();
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
        commands_.push_back({Command::Kind::AttachController, entity, std::move(definition)});
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
            slot.instance = controllerTypes_.at(slot.definition.type).create();
            if (slot.instance) {
                slot.instance->scene_ = this;
                slot.instance->entity_ = entity;
                inControllerCallback_ = true;
                slot.instance->LoadAuthored(slot.definition.data);
                slot.instance->OnCreate();
                if (const auto pending = pendingGameStates_.find(entity); pending != pendingGameStates_.end()) {
                    slot.instance->LoadGameState(pending->second);
                    pendingGameStates_.erase(pending);
                }
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
    if (!error.empty()) spdlog::error("Scene controller on {}: {}", GetId(entity).ToString(), error);
    ++revision_;
    FlushCommands();
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
        commands_.push_back({Command::Kind::RemoveController, entity});
        return;
    }
    DestroyController(entity);
    FlushCommands();
}

void Scene::SetControllerEnabled(entt::entity entity, bool enabled) {
    if (inControllerCallback_) {
        commands_.push_back({Command::Kind::SetControllerEnabled, entity, {}, enabled});
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
    FlushCommands();
}

const ControllerDefinition* Scene::GetControllerDefinition(entt::entity entity) const {
    const auto it = controllers_.find(entity);
    return it == controllers_.end() ? nullptr : &it->second.definition;
}

void Scene::SetControllerAuthoredData(entt::entity entity, StateValue data) {
    if (auto it = controllers_.find(entity); it != controllers_.end()) {
        it->second.definition.data = std::move(data);
        ++revision_;
    }
}

bool Scene::CommitControllerAuthored(entt::entity entity, std::string& error) {
    auto it = controllers_.find(entity);
    if (it == controllers_.end() || !it->second.instance) { error = "controller is unavailable"; return false; }
    try {
        auto authored = it->second.definition.data;
        it->second.instance->SaveAuthored(authored);
        if (!ValidateController(ControllerDefinition{it->second.definition.type,
            it->second.definition.version, it->second.definition.enabled, authored}, error)) return false;
        it->second.definition.data = std::move(authored);
        ++revision_;
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

std::optional<StateValue> Scene::CaptureControllerGameState(entt::entity entity, std::string& error) const {
    auto it = controllers_.find(entity);
    if (it == controllers_.end() || !it->second.instance) {
        if (const auto pending = pendingGameStates_.find(entity); pending != pendingGameStates_.end())
            return pending->second;
        return std::nullopt;
    }
    try {
        StateValue state;
        it->second.instance->SaveGameState(state);
        error.clear();
        return state;
    } catch (const std::exception& e) { error = e.what(); return std::nullopt; }
}

bool Scene::RestoreControllerGameState(entt::entity entity, const StateValue& state, std::string& error) {
    auto it = controllers_.find(entity);
    if (it == controllers_.end()) { error = "controller is missing"; return false; }
    if (!it->second.instance) { pendingGameStates_[entity] = state; error = "controller is unavailable; state retained"; return true; }
    try { it->second.instance->LoadGameState(state); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}

void Scene::SetControllersActive(bool active) {
    if (inControllerCallback_) {
        commands_.push_back({Command::Kind::SetControllersActive, entt::null, {}, active});
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
    FlushCommands();
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
    FlushCommands();
}

void Scene::FlushCommands() {
    if (flushingCommands_ || inControllerCallback_ || destructionDepth_ != 0) return;
    flushingCommands_ = true;
    while (!commands_.empty()) {
        Command command = std::move(commands_.front());
        commands_.pop_front();
        switch (command.kind) {
        case Command::Kind::DestroyEntity:
            if (IsValid(command.entity)) DestroyEntity(command.entity);
            break;
        case Command::Kind::Clear: Clear(); break;
        case Command::Kind::AttachController:
            if (IsValid(command.entity)) AttachController(command.entity, std::move(command.definition));
            break;
        case Command::Kind::RemoveController: RemoveController(command.entity); break;
        case Command::Kind::SetControllerEnabled: SetControllerEnabled(command.entity, command.active); break;
        case Command::Kind::SetControllersActive: SetControllersActive(command.active); break;
        }
    }
    flushingCommands_ = false;
}

std::size_t Scene::AddLifecycleObserver(LifecycleHandler handler) {
    const auto token = ++nextLifecycleObserver_;
    lifecycleObservers_.emplace(token, std::move(handler));
    return token;
}

void Scene::RemoveLifecycleObserver(std::size_t token) {
    lifecycleObservers_.erase(token);
}

void Scene::NotifyLifecycle(LifecycleEvent event, entt::entity entity, entt::id_type component) {
    std::vector<std::size_t> tokens;
    tokens.reserve(lifecycleObservers_.size());
    for (const auto& [token, handler] : lifecycleObservers_) tokens.push_back(token);
    for (const auto token : tokens) {
        if (const auto it = lifecycleObservers_.find(token); it != lifecycleObservers_.end())
            it->second(event, entity, component);
    }
}

void Scene::SetActiveCamera(entt::entity entity) {
    if (entity != entt::null && !HasComponent<Camera>(entity)) return;
    activeCameraId_ = entity == entt::null ? std::nullopt : std::optional{GetId(entity)};
    ++revision_;
}

entt::entity Scene::GetActiveCamera() const {
    const auto entity = activeCameraId_ ? FindByUuid(*activeCameraId_) : entt::null;
    return HasComponent<Camera>(entity) ? entity : FirstWith<Camera>();
}

void Scene::SetDirectionalLight(entt::entity entity) {
    if (entity != entt::null && !HasComponent<DirectionalLight>(entity)) return;
    directionalLightId_ = entity == entt::null ? std::nullopt : std::optional{GetId(entity)};
    ++revision_;
}

entt::entity Scene::GetDirectionalLight() const {
    const auto entity = directionalLightId_ ? FindByUuid(*directionalLightId_) : entt::null;
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
