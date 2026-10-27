#include <engine/assets/MaterialDefinition.hpp>
#include "MaterialSchema.hpp"

#include <array>
#include <set>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace engine {
using Json = nlohmann::json;

NLOHMANN_JSON_SERIALIZE_ENUM(MaterialHook, {
    {MaterialHook::None, nullptr}, {MaterialHook::GBufferVS, "GBufferVS"},
    {MaterialHook::GBufferPS, "GBufferPS"}, {MaterialHook::ShadowVS, "ShadowVS"}
})

void from_json(const Json& j, MaterialParameterOverrides& value) {
    if (j.contains("albedoTexture"))
        value.albedoTexture = j.at("albedoTexture").is_null() ? "" : j.at("albedoTexture").get<std::string>();
    if (j.contains("specularColor")) {
        const auto color = j.at("specularColor").get<std::array<float, 3>>();
        value.specularColor = Vector3{color[0], color[1], color[2]};
    }
    if (j.contains("shininess")) value.shininess = j.at("shininess").get<float>();
}

void to_json(Json& j, const MaterialParameterOverrides& value) {
    j = Json::object();
    if (value.albedoTexture)
        j["albedoTexture"] = value.albedoTexture->empty() ? Json(nullptr) : Json(*value.albedoTexture);
    if (value.specularColor)
        j["specularColor"] = {value.specularColor->x, value.specularColor->y, value.specularColor->z};
    if (value.shininess) j["shininess"] = *value.shininess;
}

void from_json(const Json& j, MaterialDefinition& value) {
    value.shaderFile = j.value("shaderFile", std::string{});
    for (const auto hook : j.at("hooks").get<std::vector<MaterialHook>>()) value.hooks = value.hooks | hook;
    value.defaults = j.value("defaults", MaterialParameterOverrides{});
}

void to_json(Json& j, const MaterialDefinition& value) {
    constexpr unsigned knownHooks = 7;
    if ((static_cast<unsigned>(value.hooks) & ~knownHooks) != 0)
        throw std::invalid_argument("unknown material hook bits");
    j = {{"version", 1}, {"hooks", Json::array()}, {"defaults", value.defaults}};
    for (const auto hook : {MaterialHook::GBufferVS, MaterialHook::GBufferPS, MaterialHook::ShadowVS})
        if (HasHook(value.hooks, hook)) j["hooks"].push_back(hook);
    if (!value.shaderFile.empty()) j["shaderFile"] = value.shaderFile;
}

void from_json(const Json& j, MaterialInstanceDefinition& value) {
    j.at("material").get_to(value.material);
    value.parameters = j.value("parameters", MaterialParameterOverrides{});
}

void to_json(Json& j, const MaterialInstanceDefinition& value) {
    j = {{"version", 1}, {"material", value.material}, {"parameters", value.parameters}};
}

namespace {
const nlohmann::json_schema::json_validator& Validator() {
    static const auto validator = [] {
        nlohmann::json_schema::json_validator result;
        result.set_root_schema(Json::parse(detail::MaterialSchema));
        return result;
    }();
    return validator;
}

Json Read(std::string_view text) {
    // The library parser handles syntax/UTF-8; this callback rejects duplicate keys
    // rather than silently accepting the last value. Bound nesting before DOM creation.
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 32) throw std::invalid_argument("material JSON nesting is too deep");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate JSON field: " + value.get<std::string>());
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    auto root = Json::parse(text.begin(), text.end(), callback);
    Validator().validate(root);
    return root;
}

template <typename T>
bool Parse(std::string_view text, T& result, std::string& error) {
    try {
        auto parsed = Read(text).get<T>();
        result = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

template <typename T>
bool Serialize(const T& value, std::string& text, std::string& error) {
    try {
        auto encoded = Json(value).dump(2);
        // Validate the encoded representation too: NaN/Inf become null in JSON.
        Read(encoded).get<T>();
        text = std::move(encoded);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
} // namespace

bool ParseMaterialDefinition(std::string_view text, MaterialDefinition& value, std::string& error) {
    return Parse(text, value, error);
}
bool ParseMaterialInstanceDefinition(std::string_view text, MaterialInstanceDefinition& value, std::string& error) {
    return Parse(text, value, error);
}
bool SerializeMaterialDefinition(const MaterialDefinition& value, std::string& text, std::string& error) {
    return Serialize(value, text, error);
}
bool SerializeMaterialInstanceDefinition(const MaterialInstanceDefinition& value, std::string& text, std::string& error) {
    return Serialize(value, text, error);
}
} // namespace engine
