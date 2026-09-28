#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SimpleMath.h>
#include <entt/entity/registry.hpp>

#include <engine/engine_export.hpp>
#include <engine/scene/Components.hpp>

namespace engine {

/// An EnTT registry plus the structure the engine and editor rely on: every
/// scene entity has a Name, a Transform and a Hierarchy component.
///
/// Gameplay components go straight to the registry:
///
///     auto& registry = scene.GetRegistry();
///     registry.emplace<MeshRenderer>(entity, mesh, texture);
///     for (auto [entity, renderer] : registry.view<MeshRenderer>().each()) { ... }
///
/// Create, destroy and reparent entities through Scene rather than
/// registry.create()/destroy(), so the hierarchy stays consistent.
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

    [[nodiscard]] entt::registry& GetRegistry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& GetRegistry() const noexcept { return registry_; }

    // --- Entities --------------------------------------------------------------
    /// Returns entt::null if `parent` is given but invalid.
    entt::entity CreateEntity(std::string_view name = "Entity", entt::entity parent = entt::null);
    /// Destroys the entity and all of its descendants.
    void DestroyEntity(entt::entity entity);
    void Clear();

    [[nodiscard]] bool IsValid(entt::entity entity) const;

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
    /// hierarchy view. For component changes use registry.on_construct<T>() etc.
    [[nodiscard]] std::uint64_t GetRevision() const noexcept { return revision_; }

private:
    /// Logs and returns false for an invalid entity. `caller` names the public function.
    [[nodiscard]] bool CheckEntity(entt::entity entity, std::string_view caller) const;
    void Detach(entt::entity entity);
    void Attach(entt::entity entity, entt::entity parent);

    entt::registry registry_;
    std::vector<entt::entity> roots_;
    std::uint64_t revision_ = 0;
};

}  // namespace engine
