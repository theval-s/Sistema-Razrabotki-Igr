#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
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
#include <engine/physics/PhysicsComponents.hpp>
#include <engine/scripting/ScriptComponent.hpp>
#include <engine/scene/Controller.hpp>
#include <engine/scene/SceneQueries.hpp>
#include <engine/rendering/RenderComponents.hpp>

namespace engine {
struct Camera;
struct DirectionalLight;
struct MeshRenderer;

/// An EnTT registry plus the structure the engine and editor rely on: every
/// scene entity has a Name, Tags, Transform and Hierarchy component.
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
    Scene(Scene&&) = delete; // controller instances retain their owning Scene address
    Scene& operator=(Scene&&) = delete;

    // --- Entities --------------------------------------------------------------
    /// Returns entt::null if `parent` is given but invalid.
    entt::entity CreateEntity(std::string_view name = "Entity", entt::entity parent = entt::null);
    /// Used when loading saved entities. A duplicate or malformed UUID is rejected.
    entt::entity CreateEntityWithId(std::string_view id, std::string_view name = "Entity",
                                    entt::entity parent = entt::null);
    entt::entity CreateEntityWithUuid(EntityUuid id, std::string_view name = "Entity",
                                      entt::entity parent = entt::null);
    /// Destroys the entity and all of its descendants.
    /// Destruction, Clear, controller attachment/removal and activation requested
    /// from a controller callback run FIFO after the outermost callback/teardown.
    /// Controllers attached during UpdateControllers first update on the next call.
    void DestroyEntity(entt::entity entity);
    void Clear();

    [[nodiscard]] bool IsValid(entt::entity entity) const;
    /// Returns the default (zero) UUID for an invalid entity; check IsValid(entity)
    /// when distinguishing it from a scene entity whose ID is the zero UUID.
    [[nodiscard]] EntityUuid GetId(entt::entity entity) const;
    [[nodiscard]] entt::entity FindByUuid(EntityUuid id) const;
    [[nodiscard]] entt::entity FindById(std::string_view id) const;
    [[nodiscard]] entt::entity FindFirstByName(std::string_view name) const;
    [[nodiscard]] std::vector<entt::entity> FindAllByName(std::string_view name) const;
    [[nodiscard]] std::vector<entt::entity> FindWithTag(std::string_view tag) const;
    [[nodiscard]] std::span<const std::string> GetTags(entt::entity entity) const;
    void AddTag(entt::entity entity, std::string_view tag);
    void RemoveTag(entt::entity entity, std::string_view tag);

    [[nodiscard]] bool RegisterController(std::string_view type, ControllerRegistration registration);
    [[nodiscard]] bool ValidateController(const ControllerDefinition& definition, std::string& error) const;
    void AttachController(entt::entity entity, ControllerDefinition definition);
    void RemoveController(entt::entity entity);
    void SetControllerEnabled(entt::entity entity, bool enabled);
    [[nodiscard]] const ControllerDefinition* GetControllerDefinition(entt::entity entity) const;
    // Explicit authoring operation; controller runtime mutations do not change saved data.
    void SetControllerAuthoredData(entt::entity entity, StateValue data);
    [[nodiscard]] bool CommitControllerAuthored(entt::entity entity, std::string& error);
    [[nodiscard]] std::optional<StateValue> CaptureControllerGameState(entt::entity entity, std::string& error) const;
    [[nodiscard]] bool RestoreControllerGameState(entt::entity entity, const StateValue& state, std::string& error);
    void SetControllersActive(bool active);
    void SetControllerInstantiationEnabled(bool enabled);
    void UpdateControllers(double deltaSeconds);
    [[nodiscard]] bool IsInControllerCallback() const noexcept { return inControllerCallback_; }
    enum class LifecycleEvent { ComponentRemoving, EntityDestroying };
    using LifecycleHandler = std::function<void(LifecycleEvent, entt::entity, entt::id_type)>;
    /// Called before the component/entity is removed, while its data is still accessible.
    /// Returns a subscription token; remove observers before their captured state dies.
    [[nodiscard]] std::size_t AddLifecycleObserver(LifecycleHandler handler);
    void RemoveLifecycleObserver(std::size_t token);
    /// Returns a token that expires when this Scene is destroyed.
    /// Does not extend the lifetime of Scene.
    [[nodiscard]] std::weak_ptr<void> GetLifetimeToken() const noexcept { return lifetime_;}

    // --- Change notifications --------------------------------------------------------
    // Every change made through Scene's API is reported, for editor.
    // Writes through the mutable TryGetComponent()/GetTransform() don't emit events,
    // runtime systems use those every frame a lot. Use Modify()/SetTransform() where it matters.

    enum class ChangeKind : std::uint8_t {
        /// A new entity. Its components may still be added before the call that created it returns.
        Created,
        /// The entity is gone and its handle stale. Children go first.
        Destroyed,
        /// Reparented. Also emits for changing which folder entity is in
        Hierarchy,
        /// Changing name, tags, transform, components, controller etc
        Modified,
        /// Not about an entity (entt::null): the editor folder list, the active camera etc
        Settings,
    };
    using ChangeHandler = std::function<void(ChangeKind, entt::entity)>;
    /// Called synchronously, right after the scene changes. Handlers must not change the scene.

    /// Returns a subscription token; remove observers before their captured state dies.
    [[nodiscard]] std::size_t AddChangeObserver(ChangeHandler handler);
    void RemoveChangeObserver(std::size_t token);

    void SetActiveCamera(entt::entity entity);
    [[nodiscard]] entt::entity GetActiveCamera() const;
    void SetDirectionalLight(entt::entity entity);
    [[nodiscard]] entt::entity GetDirectionalLight() const;

    // --- Editor folders ------------------------------------------------------------
    // Grouping for the editor's hierarchy, saved with the scene and ignored at runtime.

    /// "A/B": nonempty folder names separated by single slashes.
    [[nodiscard]] static bool IsValidEditorFolder(std::string_view path);
    /// Puts a entity into a folder
    bool SetEditorFolder(entt::entity entity, std::string_view path);
    /// Empty when the entity isn't in a folder.
    [[nodiscard]] const std::string& GetEditorFolder(entt::entity entity) const;
    /// Folders that exist even with no entity in them. Invalid paths and repeats are dropped.
    void SetEditorFolders(std::vector<std::string> folders);
    [[nodiscard]] std::span<const std::string> GetEditorFolders() const noexcept { return editorFolders_; }
    [[nodiscard]] bool HasUnsupportedComponents() const noexcept { return !unsupported_.empty(); }
    [[nodiscard]] bool HasUnsupportedComponents(entt::entity entity) const noexcept { return unsupported_.contains(entity); }

    /// A component type an entity has, for editor inspector
    struct ComponentType {
        entt::id_type id;       // entt::type_hash<T>::value()
        std::string_view name;  // EnTT's type name, for display only
    };
    /// All components for given entity
    [[nodiscard]] std::vector<ComponentType> GetComponentTypes(entt::entity entity) const;

    template <class T, class... Args>
    T* AddComponent(entt::entity entity, Args&&... args) {
        static_assert(!IsRequiredComponent<T>, "required components are managed by Scene");
        if (!IsValid(entity) || registry_.all_of<T>(entity)) return nullptr;
        T& result = registry_.emplace<T>(entity, std::forward<Args>(args)...);
        if constexpr (!IsSerializableComponent<T>) unsupported_[entity].insert(entt::type_hash<T>::value());
        ++revision_;
        NotifyChange(ChangeKind::Modified, entity);
        return &result;
    }

    template <class T>
    bool RemoveComponent(entt::entity entity) {
        static_assert(!IsRequiredComponent<T>, "required components are managed by Scene");
        if (!IsValid(entity) || !registry_.all_of<T>(entity)) return false;
        NotifyLifecycle(LifecycleEvent::ComponentRemoving, entity, entt::type_hash<T>::value());
        registry_.remove<T>(entity);
        if constexpr (!IsSerializableComponent<T>) {
            if (auto it = unsupported_.find(entity); it != unsupported_.end()) {
                it->second.erase(entt::type_hash<T>::value());
                if (it->second.empty()) unsupported_.erase(it);
            }
        }
        ++revision_;
        NotifyChange(ChangeKind::Modified, entity);
        return true;
    }

    /// Edits the entity's T through `edit(T&)` and reports it. False, without
    /// calling `edit`, if the entity has no T.
    template <class T, class Fn>
    bool Modify(entt::entity entity, Fn&& edit) {
        static_assert(!IsRequiredComponent<T>, "required components are managed by Scene");
        T* component = IsValid(entity) ? registry_.try_get<T>(entity) : nullptr;
        if (component == nullptr) return false;
        std::forward<Fn>(edit)(*component);
        NotifyChange(ChangeKind::Modified, entity);
        return true;
    }

    /// Writes through it aren't reported; see Modify().
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
    /// Writes through it aren't reported; see SetTransform().
    [[nodiscard]] Transform& GetTransform(entt::entity entity);
    [[nodiscard]] const Transform& GetTransform(entt::entity entity) const;
    /// The local transform, reported.
    void SetTransform(entt::entity entity, const Transform& transform);
    /// Local transform combined with all ancestors.
    [[nodiscard]] DirectX::SimpleMath::Matrix GetWorldMatrix(entt::entity entity) const;

    /// entt::null for root entities.
    [[nodiscard]] entt::entity GetParent(entt::entity entity) const;
    /// entt::null as parent makes the entity a root. Returns false (and changes
    /// nothing) if that would create a cycle or an entity is invalid.
    bool SetParent(entt::entity entity, entt::entity parent);
    /// SetParent() that changes the local transform too
    /// without this entity would teleport when reparenting
    bool SetParentKeepingWorld(entt::entity entity, entt::entity parent);
    [[nodiscard]] std::span<const entt::entity> GetChildren(entt::entity entity) const;
    [[nodiscard]] std::span<const entt::entity> GetRootEntities() const noexcept;

    /// Incremented when entities are created, destroyed, renamed or reparented
    /// through Scene, and when components are added or removed. Not by value
    /// changes such as Modify(): observe changes for those.
    [[nodiscard]] std::uint64_t GetRevision() const noexcept { return revision_; }

private:
    template <class T>
    static constexpr bool IsRequiredComponent = std::is_same_v<T, Name> || std::is_same_v<T, Transform> ||
                                                 std::is_same_v<T, Hierarchy> || std::is_same_v<T, EntityId> ||
                                                 std::is_same_v<T, Tags> || std::is_same_v<T, EditorFolder>;
    template <class T>
    static constexpr bool IsSerializableComponent = std::is_same_v<T, MeshRenderer> ||
        std::is_same_v<T, Camera> || std::is_same_v<T, DirectionalLight> ||
        std::is_same_v<T, PointLight> || std::is_same_v<T, SpotLight> || std::is_same_v<T, Bounds> ||
        std::is_same_v<T, GamePersistence> || std::is_same_v<T, RigidBody> || std::is_same_v<T, Collider> || std::is_same_v<T, ScriptComponent>;
    /// Logs and returns false for an invalid entity. `caller` names the public function.
    [[nodiscard]] bool CheckEntity(entt::entity entity, std::string_view caller) const;
    void Detach(entt::entity entity);
    void Attach(entt::entity entity, entt::entity parent);
    void Visit(const std::function<void(entt::entity)>& callback) const;
    void FlushCommands();
    void DestroyController(entt::entity entity);
    void NotifyLifecycle(LifecycleEvent event, entt::entity entity, entt::id_type component);
    void NotifyChange(ChangeKind kind, entt::entity entity = entt::null);

    struct ControllerSlot {
        ControllerDefinition definition;
        std::unique_ptr<Controller> instance;
        bool active = false;
    };

    struct Command {
        enum class Kind { DestroyEntity, Clear, AttachController, RemoveController, SetControllerEnabled, SetControllersActive };
        Kind kind;
        entt::entity entity = entt::null;
        ControllerDefinition definition;
        bool active = false;
    };

    entt::registry registry_;
    std::unordered_map<EntityUuid, entt::entity, EntityUuidHash> ids_;
    std::unordered_map<entt::entity, std::unordered_set<entt::id_type>> unsupported_;
    std::unordered_map<std::string, ControllerRegistration> controllerTypes_;
    std::unordered_map<entt::entity, ControllerSlot> controllers_;
    std::unordered_map<entt::entity, StateValue> pendingGameStates_;
    // FIFO. Callbacks enqueue; the outermost controller/destruction operation
    // flushes at its safe point. Commands appended while flushing run last.
    std::deque<Command> commands_;
    bool flushingCommands_ = false;
    std::unordered_map<std::size_t, LifecycleHandler> lifecycleObservers_;
    std::size_t nextLifecycleObserver_ = 0;
    std::unordered_map<std::size_t, ChangeHandler> changeObservers_;
    std::size_t nextChangeObserver_ = 0;
    bool controllersActive_ = false;
    bool controllerInstantiationEnabled_ = true;
    bool inControllerCallback_ = false;
    std::size_t destructionDepth_ = 0;
    std::optional<EntityUuid> activeCameraId_;
    std::optional<EntityUuid> directionalLightId_;
    std::vector<std::string> editorFolders_;
    std::vector<entt::entity> roots_;
    std::uint64_t revision_ = 0;
    // Lifetime tracking
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
};

}  // namespace engine
