#pragma once
#include <engine/scripting/ScriptComponent.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Transform.hpp>
#include <engine/physics/PhysicsComponents.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controller.hpp>
#include <engine/scene/SceneQueries.hpp>

namespace engine {
class Scene;
class AssetSystem;

// Authored, asset-free scene representation. All references survive a parse
// without requesting GPU resources; switching scenes can be committed afterwards.
struct SceneEntityDefinition {
    EntityUuid id;
    std::string name;
    std::vector<std::string> tags;
    std::optional<EntityUuid> parent;
    Transform transform;
    struct Mesh {
        AssetReference mesh;
        AssetReference materialInstance;
    };
    std::optional<Mesh> renderer;
    std::optional<Camera> camera;
    std::optional<DirectionalLight> directionalLight;
    std::optional<PointLight> pointLight;
    std::optional<SpotLight> spotLight;
    std::optional<ControllerDefinition> controller;
    std::optional<Bounds> bounds;
    std::optional<RigidBody> rigidBody;
    std::optional<Collider> collider;
    std::optional<GamePersistence> persistence;
    std::optional<ScriptComponent> script;
};

struct SceneDocument {
    std::vector<SceneEntityDefinition> entities;
    std::optional<EntityUuid> activeCamera;
    std::optional<EntityUuid> directionalLight;
};

// Structural validation includes UUID syntax, unique entity IDs, hierarchy
// cycles, component data and active camera/light selection. Asset existence is
// intentionally checked only when assets are requested after scene commitment.
[[nodiscard]] ENGINE_API bool ParseSceneDocument(std::string_view json, SceneDocument& result, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeSceneDocument(const SceneDocument& document, std::string& json, std::string& error);
[[nodiscard]] ENGINE_API bool CaptureSceneDocument(const Scene& scene, const AssetSystem& assets,
                                                   SceneDocument& result, std::string& error,
                                                   bool ignoreTransientComponents = false);
// Called only after ParseSceneDocument has validated the document, on an empty
// destination scene. Missing assets yield invalid handles but do not abort loading.
ENGINE_API void PopulateScene(const SceneDocument& document, AssetSystem& assets, Scene& destination);

// The subtrees under `entities`, parents before children, for copying them within a live
// scene (duplicate, copy/paste, undo). An entity inside another one's subtree is taken once.
// The top entity of each subtree keeps its parent's UUID, which is outside the result.
// Fails, leaving `result` unchanged, if one of them has components without a codec.
[[nodiscard]] ENGINE_API bool CaptureEntities(const Scene& scene, const AssetSystem& assets,
                                              std::span<const entt::entity> entities,
                                              std::vector<SceneEntityDefinition>& result, std::string& error);
// Creates entities from CaptureEntities() in a live scene. With `freshIds` each one gets a new
// UUID and parents inside the set follow along; otherwise the UUIDs are kept, and an entity
// whose UUID is already in use is skipped. A parent outside the set is looked up in
// `destination`; the entity becomes a root if it isn't there. Returns the created entities in
// the order of `definitions` (entt::null for skipped ones).
ENGINE_API std::vector<entt::entity> InstantiateEntities(std::span<const SceneEntityDefinition> definitions,
                                                         AssetSystem& assets, Scene& destination, bool freshIds);
} // namespace engine
