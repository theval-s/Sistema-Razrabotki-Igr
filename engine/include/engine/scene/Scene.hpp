#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <SimpleMath.h>
#include <entt/entity/registry.hpp>

#include <engine/engine_export.hpp>
#include <engine/scene/Components.hpp>

namespace engine {
struct Camera;
struct DirectionalLight;
struct MeshRenderer;

/// An EnTT registry plus the structure the engine and editor rely on: every
/// scene entity has a Name, a Transform and a Hierarchy component.
///
/// Create, destroy, reparent and modify components through Scene, so required
/// components and the hierarchy cannot be bypassed by external code.
///
/// Passing an invalid entity (null, destroyed, or not created by the Scene)
/// logs an error and returns a neutral result: nothing happens, false,
/// entt::null, an empty name/span, or an identity matrix. The exception is
/// GetTransform(), which has nothing sane to return and exits the process.
class ENGINE_API Scene {
public:
    Scene();
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;

    // --- Entities --------------------------------------------------------------
    /// Returns entt::null if `parent` is given but invalid.
    entt::entity CreateEntity(std::string_view name = "Entity", entt::entity parent = entt::null);
    /// Used when loading saved entities. A duplicate or malformed UUID is rejected.
    entt::entity CreateEntityWithId(std::string_view id, std::string_view name = "Entity",
                                    entt::entity parent = entt::null);
    /// Destroys the entity and all of its descendants.
    void DestroyEntity(entt::entity entity);
    void Clear();

    [[nodiscard]] bool IsValid(entt::entity entity) const;
    [[nodiscard]] std::string_view GetId(entt::entity entity) const;
    [[nodiscard]] entt::entity FindById(std::string_view id) const;
    void SetActiveCamera(entt::entity entity);
    [[nodiscard]] entt::entity GetActiveCamera() const;
    void SetDirectionalLight(entt::entity entity);
    [[nodiscard]] entt::entity GetDirectionalLight() const;
    [[nodiscard]] bool HasUnsupportedComponents() const noexcept { return !unsupported_.empty(); }

    template <class T, class... Args>
    T* AddComponent(entt::entity entity, Args&&... args) {
        static_assert(!IsRequiredComponent<T>, "required components are managed by Scene");
        if (!IsValid(entity) || registry_.all_of<T>(entity)) return nullptr;
        T& result = registry_.emplace<T>(entity, std::forward<Args>(args)...);
        if constexpr (!IsSerializableComponent<T>) unsupported_[entity].insert(entt::type_hash<T>::value());
        ++revision_;
        return &result;
    }

    template <class T>
    bool RemoveComponent(entt::entity entity) {
        static_assert(!IsRequiredComponent<T>, "required components are managed by Scene");
        if (!IsValid(entity) || !registry_.all_of<T>(entity)) return false;
        registry_.remove<T>(entity);
        if constexpr (!IsSerializableComponent<T>) {
            if (auto it = unsupported_.find(entity); it != unsupported_.end()) {
                it->second.erase(entt::type_hash<T>::value());
                if (it->second.empty()) unsupported_.erase(it);
            }
        }
        ++revision_;
        return true;
    }

    template <class T>
    [[nodiscard]] T* TryGetComponent(entt::entity entity) {
        static_assert(!IsRequiredComponent<T>, "required components are managed by Scene");
        return IsValid(entity) ? registry_.try_get<T>(entity) : nullptr;
    }

    template <class T>
    [[nodiscard]] const T* TryGetComponent(entt::entity entity) const {
        return IsValid(entity) ? registry_.try_get<T>(entity) : nullptr;
    }

    template <class T>
    [[nodiscard]] bool HasComponent(entt::entity entity) const {
        return TryGetComponent<T>(entity) != nullptr;
    }

    template <class T, class Fn>
    void ForEach(Fn&& fn) const {
        for (auto [entity, component] : registry_.view<T>().each()) {
            if (IsValid(entity)) fn(entity, std::as_const(component));
        }
    }

    template <class T>
    [[nodiscard]] entt::entity FirstWith() const {
        for (auto [entity, component] : registry_.view<T>().each()) {
            if (IsValid(entity)) return entity;
        }
        return entt::null;
    }

    [[nodiscard]] const std::string& GetName(entt::entity entity) const;
    void SetName(entt::entity entity, std::string_view name);

    // --- Transform & hierarchy -----------------------------------------------------
    [[nodiscard]] Transform& GetTransform(entt::entity entity);
    [[nodiscard]] const Transform& GetTransform(entt::entity entity) const;
    /// Local transform combined with all ancestors.
    [[nodiscard]] DirectX::SimpleMath::Matrix GetWorldMatrix(entt::entity entity) const;

    /// entt::null for root entities.
    [[nodiscard]] entt::entity GetParent(entt::entity entity) const;
    /// entt::null as parent makes the entity a root. Returns false (and changes
    /// nothing) if that would create a cycle or an entity is invalid.
    bool SetParent(entt::entity entity, entt::entity parent);
    [[nodiscard]] std::span<const entt::entity> GetChildren(entt::entity entity) const;
    [[nodiscard]] std::span<const entt::entity> GetRootEntities() const noexcept;

    /// Incremented when entities are created, destroyed, renamed or reparented
    /// through Scene. Cheap way for the editor to know when to rebuild its
    /// hierarchy view. Component insertion and removal also increment it.
    [[nodiscard]] std::uint64_t GetRevision() const noexcept { return revision_; }

private:
    template <class T>
    static constexpr bool IsRequiredComponent = std::is_same_v<T, Name> || std::is_same_v<T, Transform> ||
                                                std::is_same_v<T, Hierarchy> || std::is_same_v<T, EntityId>;
    template <class T>
    static constexpr bool IsSerializableComponent = std::is_same_v<T, MeshRenderer> ||
        std::is_same_v<T, Camera> || std::is_same_v<T, DirectionalLight>;
    /// Logs and returns false for an invalid entity. `caller` names the public function.
    [[nodiscard]] bool CheckEntity(entt::entity entity, std::string_view caller) const;
    void Detach(entt::entity entity);
    void Attach(entt::entity entity, entt::entity parent);

    entt::registry registry_;
    std::unordered_map<std::string, entt::entity> ids_;
    std::unordered_map<entt::entity, std::unordered_set<entt::id_type>> unsupported_;
    std::string activeCameraId_;
    std::string directionalLightId_;
    std::vector<entt::entity> roots_;
    std::uint64_t revision_ = 0;
};

}  // namespace engine
