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
    if (j.contains("albedoTexture")) {
        const auto& texture = j.at("albedoTexture");
        if (texture.is_null()) value.albedoTexture = "";
        else if (texture.is_object()) {
            std::string error;
            AssetReference ref;
            if (!ParseAssetReference(texture.dump(), ref, error)) throw std::invalid_argument(error);
            value.albedoTexture = ref.path;
            value.albedoReference = std::move(ref);
        } else value.albedoTexture = texture.get<std::string>();
    }
    if (j.contains("specularColor")) {
        const auto color = j.at("specularColor").get<std::array<float, 3>>();
        value.specularColor = Vector3{color[0], color[1], color[2]};
    }
    if (j.contains("shininess")) value.shininess = j.at("shininess").get<float>();
}

void to_json(Json& j, const MaterialParameterOverrides& value) {
    j = Json::object();
    if (value.albedoTexture) {
        if (value.albedoTexture->empty()) j["albedoTexture"] = nullptr;
        else if (value.albedoReference) {
            std::string text, error;
            if (!SerializeAssetReference(*value.albedoReference, text, error)) throw std::invalid_argument(error);
            j["albedoTexture"] = Json::parse(text);
        } else j["albedoTexture"] = *value.albedoTexture;
    }
    if (value.specularColor)
        j["specularColor"] = {value.specularColor->x, value.specularColor->y, value.specularColor->z};
    if (value.shininess) j["shininess"] = *value.shininess;
}

void from_json(const Json& j, MaterialDefinition& value) {
    value.version = j.at("version").get<int>();
    if (j.contains("shaderFile")) {
        if (j.at("shaderFile").is_object()) {
            std::string error;
            AssetReference ref;
            if (!ParseAssetReference(j.at("shaderFile").dump(), ref, error)) throw std::invalid_argument(error);
            value.shaderFile = ref.path;
            value.shaderReference = std::move(ref);
        } else value.shaderFile = j.at("shaderFile").get<std::string>();
    }
    for (const auto hook : j.at("hooks").get<std::vector<MaterialHook>>()) value.hooks = value.hooks | hook;
    value.defaults = j.value("defaults", MaterialParameterOverrides{});
}

void to_json(Json& j, const MaterialDefinition& value) {
    constexpr unsigned knownHooks = 7;
    if ((static_cast<unsigned>(value.hooks) & ~knownHooks) != 0)
        throw std::invalid_argument("unknown material hook bits");
    j = {{"version", value.version}, {"hooks", Json::array()}, {"defaults", value.defaults}};
    for (const auto hook : {MaterialHook::GBufferVS, MaterialHook::GBufferPS, MaterialHook::ShadowVS})
        if (HasHook(value.hooks, hook)) j["hooks"].push_back(hook);
    if (value.shaderReference) {
        std::string text, error;
        if (!SerializeAssetReference(*value.shaderReference, text, error)) throw std::invalid_argument(error);
        j["shaderFile"] = Json::parse(text);
    } else if (!value.shaderFile.empty()) j["shaderFile"] = value.shaderFile;
}

void from_json(const Json& j, MaterialInstanceDefinition& value) {
    value.version = j.at("version").get<int>();
    if (j.at("material").is_object()) {
        std::string error;
        AssetReference ref;
        if (!ParseAssetReference(j.at("material").dump(), ref, error)) throw std::invalid_argument(error);
        value.material = ref.path;
        value.materialReference = std::move(ref);
    } else j.at("material").get_to(value.material);
    value.parameters = j.value("parameters", MaterialParameterOverrides{});
}

void to_json(Json& j, const MaterialInstanceDefinition& value) {
    j = {{"version", value.version}, {"parameters", value.parameters}};
    if (value.materialReference) {
        std::string text, error;
        if (!SerializeAssetReference(*value.materialReference, text, error)) throw std::invalid_argument(error);
        j["material"] = Json::parse(text);
    } else j["material"] = value.material;
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
    const int version = root.at("version").get<int>();
    const auto checkRef = [version](const Json& object, const char* field) {
        if (object.contains(field) && !object.at(field).is_null() &&
            object.at(field).is_object() != (version == 2))
            throw std::invalid_argument(std::string(field) + " must be a " +
                                        (version == 2 ? "UUID reference" : "path") + " in this version");
    };
    checkRef(root, root.contains("material") ? "material" : "shaderFile");
    if (root.contains("defaults")) checkRef(root.at("defaults"), "albedoTexture");
    if (root.contains("parameters")) checkRef(root.at("parameters"), "albedoTexture");
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
