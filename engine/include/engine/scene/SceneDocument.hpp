#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Transform.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controller.hpp>
#include <engine/scene/SceneQueries.hpp>

namespace engine {
class Scene;
class AssetSystem;

// Authored, asset-free scene representation. All references survive a parse
// without requesting GPU resources; switching scenes can be committed afterwards.
struct SceneEntityDefinition {
    std::string id;
    std::string name;
    std::vector<std::string> tags;
    std::optional<std::string> parent;
    Transform transform;
    struct Mesh {
        AssetReference mesh;
        AssetReference materialInstance;
    };
    std::optional<Mesh> renderer;
    std::optional<Camera> camera;
    std::optional<DirectionalLight> directionalLight;
    std::optional<ControllerDefinition> controller;
    std::optional<Bounds> bounds;
    std::optional<GamePersistence> persistence;
};

struct SceneDocument {
    std::vector<SceneEntityDefinition> entities;
    std::optional<std::string> activeCamera;
    std::optional<std::string> directionalLight;
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
} // namespace engine
