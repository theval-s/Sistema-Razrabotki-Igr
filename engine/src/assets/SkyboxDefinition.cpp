#include <engine/assets/SkyboxDefinition.hpp>

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace engine {
namespace {
using Json = nlohmann::json;
void Require(bool condition, const std::string& error) {
    if (!condition) throw std::invalid_argument(error);
}
void Fields(const Json& value, std::initializer_list<std::string_view> fields) {
    Require(value.is_object(), "expected skybox object");
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool known = false;
        for (const auto field : fields) known |= it.key() == field;
        Require(known, "unknown skybox field: " + it.key());
    }
}
}

bool ValidateSkyboxDefinition(const SkyboxDefinition& value, std::string& error) {
    if (value.version != 1) { error = "expected skybox version 1"; return false; }
    if (!value.cubemap.id.IsValid()) { error = "skybox requires a valid cubemap UUID"; return false; }
    error.clear();
    return true;
}

bool ParseSkyboxDefinition(std::string_view text, SkyboxDefinition& value, std::string& error) {
    try {
        std::vector<std::set<std::string>> keys;
        const auto callback = [&keys](int depth, Json::parse_event_t event, Json& item) {
            Require(depth <= 8, "skybox JSON nesting is too deep");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key)
                Require(keys.back().insert(item.get<std::string>()).second, "duplicate skybox field");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        };
        const auto root = Json::parse(text.begin(), text.end(), callback);
        Fields(root, {"version", "cubemap"});
        Require(root.at("version").is_number_integer() && root.at("version") == 1,
            "expected skybox version 1");
        const auto& reference = root.at("cubemap");
        Fields(reference, {"id", "path"});
        SkyboxDefinition result;
        result.cubemap.id = AssetUuid(reference.at("id").get<std::string>());
        if (reference.contains("path")) result.cubemap.path = reference.at("path").get<std::string>();
        if (!ValidateSkyboxDefinition(result, error)) return false;
        value = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool SerializeSkyboxDefinition(const SkyboxDefinition& value, std::string& text, std::string& error) {
    try {
        if (!ValidateSkyboxDefinition(value, error)) return false;
        Json reference = {{"id", value.cubemap.id.ToString()}};
        if (!value.cubemap.path.empty()) reference["path"] = value.cubemap.path;
        auto encoded = Json{{"version", 1}, {"cubemap", std::move(reference)}}.dump(2);
        text = std::move(encoded);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace engine
