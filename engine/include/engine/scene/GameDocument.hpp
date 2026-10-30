#pragma once
#include <engine/scene/SceneDocument.hpp>
#include <unordered_map>

namespace engine {
struct GameControllerState {
    std::string type;
    int version = 1;
    StateValue data;
};
struct GameEntityState {
    EntityUuid id;
    std::optional<Transform> transform;
    std::optional<GameControllerState> controller;
};
struct GameDocument {
    AssetReference scene;
    SceneDocument spawned; // authored snapshots, roots normalized (parents listed below)
    std::unordered_map<EntityUuid, EntityUuid, EntityUuidHash> spawnParents;
    std::vector<EntityUuid> deleted;
    std::vector<GameEntityState> entities;
};
[[nodiscard]] ENGINE_API bool ParseGameDocument(std::string_view json, GameDocument& result, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeGameDocument(const GameDocument& document, std::string& json, std::string& error);
} // namespace engine
