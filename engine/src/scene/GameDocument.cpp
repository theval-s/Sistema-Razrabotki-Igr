#include <engine/scene/GameDocument.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_set>
#include <cmath>

namespace engine {
namespace {
using Json = nlohmann::json;
using Vector3 = DirectX::SimpleMath::Vector3;
using Quaternion = DirectX::SimpleMath::Quaternion;

void Fields(const Json& j, std::initializer_list<std::string_view> allowed) {
    if (!j.is_object()) throw std::invalid_argument("expected object in save game");
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool found = false;
        for (const auto field : allowed) found |= it.key() == field;
        if (!found) throw std::invalid_argument("unknown save-game field: " + it.key());
    }
}
StateValue State(const Json& j) {
    StateValue value;
    std::string error;
    if (!ParseStateValue(j.dump(), value, error)) throw std::invalid_argument(error);
    return value;
}
Json JsonState(const StateValue& state) {
    std::string text, error;
    if (!SerializeStateValue(state, text, error)) throw std::invalid_argument(error);
    return Json::parse(text);
}
Json EncodeTransform(const Transform& t) {
    return {{"position", {t.position.x, t.position.y, t.position.z}},
            {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}},
            {"scale", {t.scale.x, t.scale.y, t.scale.z}}};
}
Transform DecodeTransform(const Json& j) {
    Fields(j, {"position", "rotation", "scale"});
    auto numbers = [](const Json& array, std::size_t count) {
        if (!array.is_array() || array.size() != count) throw std::invalid_argument("invalid transform array");
        std::vector<float> result;
        for (const auto& item : array) {
            if (!item.is_number()) throw std::invalid_argument("invalid transform number");
            const float v = item.get<float>();
            if (!std::isfinite(v)) throw std::invalid_argument("non-finite transform");
            result.push_back(v);
        }
        return result;
    };
    const auto p = numbers(j.at("position"), 3), r = numbers(j.at("rotation"), 4), s = numbers(j.at("scale"), 3);
    Transform t;
    t.position = Vector3{p[0], p[1], p[2]};
    t.rotation = Quaternion{r[0], r[1], r[2], r[3]};
    t.scale = Vector3{s[0], s[1], s[2]};
    if (t.rotation.LengthSquared() < 1e-8f) throw std::invalid_argument("zero-length rotation");
    return t;
}
GameDocument Decode(const Json& j) {
    Fields(j, {"version", "scene", "spawned", "spawnParents", "deleted", "entities"});
    if (j.at("version") != 1) throw std::invalid_argument("unsupported save-game version");
    GameDocument doc;
    std::string error;
    if (!ParseAssetReference(j.at("scene").dump(), doc.scene, error)) throw std::invalid_argument(error);
    if (!ParseSceneDocument(j.at("spawned").dump(), doc.spawned, error)) throw std::invalid_argument("spawned: " + error);
    if (!j.at("spawnParents").is_object() || !j.at("deleted").is_array() || !j.at("entities").is_array())
        throw std::invalid_argument("invalid save-game collections");
    std::unordered_set<EntityUuid, EntityUuidHash> spawnedIds, deletedIds, stateIds;
    for (const auto& e : doc.spawned.entities) {
        if (e.parent) throw std::invalid_argument("spawned snapshot parents must be normalized");
        spawnedIds.insert(e.id);
    }
    for (auto it = j.at("spawnParents").begin(); it != j.at("spawnParents").end(); ++it) {
        const auto key = EntityUuid::Parse(it.key());
        const auto id = EntityUuid::Parse(it.value().get<std::string>());
        if (!key || !id || !spawnedIds.contains(*key) || *key == *id)
            throw std::invalid_argument("invalid spawn parent");
        doc.spawnParents.emplace(*key, *id);
    }
    for (const auto& item : j.at("deleted")) {
        const auto id = EntityUuid::Parse(item.get<std::string>());
        if (!id || !deletedIds.insert(*id).second || spawnedIds.contains(*id))
            throw std::invalid_argument("invalid or duplicate deleted entity UUID");
        doc.deleted.push_back(*id);
    }
    for (const auto& item : j.at("entities")) {
        Fields(item, {"id", "transform", "controller"});
        GameEntityState state;
        const auto id = EntityUuid::Parse(item.at("id").get<std::string>());
        if (!id || !stateIds.insert(*id).second || deletedIds.contains(*id))
            throw std::invalid_argument("invalid or duplicate save-game entity UUID");
        state.id = *id;
        if (item.contains("transform")) state.transform = DecodeTransform(item.at("transform"));
        if (item.contains("controller")) {
            const auto& c = item.at("controller");
            Fields(c, {"type", "version", "data"});
            GameControllerState saved{c.at("type").get<std::string>(), c.at("version").get<int>(), State(c.at("data"))};
            if (saved.type.empty() || saved.version < 1) throw std::invalid_argument("invalid saved controller");
            state.controller = std::move(saved);
        }
        if (!state.transform && !state.controller) throw std::invalid_argument("empty game state record");
        doc.entities.push_back(std::move(state));
    }
    return doc;
}
Json Encode(const GameDocument& doc) {
    std::string spawnedText, sceneText, error;
    if (!SerializeSceneDocument(doc.spawned, spawnedText, error)) throw std::invalid_argument(error);
    if (!SerializeAssetReference(doc.scene, sceneText, error)) throw std::invalid_argument(error);
    Json j = {{"version", 1}, {"scene", Json::parse(sceneText)}, {"spawned", Json::parse(spawnedText)},
              {"spawnParents", Json::object()}, {"deleted", Json::array()}, {"entities", Json::array()}};
    for (const auto& [id, parent] : doc.spawnParents) j["spawnParents"][id.ToString()] = parent.ToString();
    for (const auto& id : doc.deleted) j["deleted"].push_back(id.ToString());
    for (const auto& state : doc.entities) {
        Json entry = {{"id", state.id.ToString()}};
        if (state.transform) entry["transform"] = EncodeTransform(*state.transform);
        if (state.controller) entry["controller"] = {{"type", state.controller->type},
            {"version", state.controller->version}, {"data", JsonState(state.controller->data)}};
        j["entities"].push_back(std::move(entry));
    }
    return j;
}
Json Read(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 32) throw std::invalid_argument("save-game JSON nesting is too deep");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate save-game field: " + value.get<std::string>());
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text.begin(), text.end(), callback);
}
}
bool ParseGameDocument(std::string_view json, GameDocument& result, std::string& error) {
    try { auto parsed = Decode(Read(json)); result = std::move(parsed); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool SerializeGameDocument(const GameDocument& document, std::string& json, std::string& error) {
    try { auto text = Encode(document).dump(2); [[maybe_unused]] auto checked = Decode(Read(text));
        json = std::move(text); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace engine
