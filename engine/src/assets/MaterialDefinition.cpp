#include <engine/assets/MaterialDefinition.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace engine {
namespace {

using Json = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
constexpr auto KnownHooks = static_cast<uint8_t>(MaterialHook::GBufferVS) |
                            static_cast<uint8_t>(MaterialHook::GBufferPS) |
                            static_cast<uint8_t>(MaterialHook::ShadowVS);

std::string_view Text(const Json& value) {
    return {value.GetString(), value.GetStringLength()};
}

bool Fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

bool Fields(const Json& value, std::initializer_list<std::string_view> allowed,
            std::string_view context, std::string& error) {
    if (!value.IsObject()) return Fail(error, std::string(context) + " must be an object");
    std::set<std::string_view> seen;
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        const auto name = Text(it->name);
        if (!seen.insert(name).second)
            return Fail(error, std::string(context) + ": duplicate field '" + std::string(name) + "'");
        bool known = false;
        for (const auto candidate : allowed) known |= name == candidate;
        if (!known) return Fail(error, std::string(context) + ": unknown field '" + std::string(name) + "'");
    }
    return true;
}

bool LogicalName(std::string_view name, std::string& error) {
    if (name.empty() || name.front() == '/' || name.front() == '\\')
        return Fail(error, "asset references must be non-empty relative logical names");
    std::string segment;
    for (const unsigned char c : name) {
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return Fail(error, "invalid character in asset reference");
        if (c == '/' || c == '\\') {
            if (segment.empty() || segment == "." || segment == "..")
                return Fail(error, "asset references cannot contain empty, '.' or '..' segments");
            segment.clear();
        } else {
            segment += static_cast<char>(c);
        }
    }
    if (segment.empty() || segment == "." || segment == "..")
        return Fail(error, "invalid final segment in asset reference");
    return true;
}

bool Reference(const Json& value, std::string& result, std::string& error) {
    if (!value.IsString()) return Fail(error, "asset reference must be a string");
    if (!LogicalName(Text(value), error)) return false;
    result.assign(Text(value));
    return true;
}

bool Number(const Json& value, float& result, std::string& error) {
    if (!value.IsNumber() || !std::isfinite(value.GetDouble()) || value.GetDouble() < 0 ||
        value.GetDouble() > std::numeric_limits<float>::max())
        return Fail(error, "material parameters must be finite non-negative floats");
    result = static_cast<float>(value.GetDouble());
    return true;
}

bool Parameters(const Json& value, MaterialParameterOverrides& result, std::string& error) {
    if (!Fields(value, {"albedoTexture", "specularColor", "shininess"}, "parameters", error)) return false;
    if (value.HasMember("albedoTexture")) {
        std::string texture;
        if (!value["albedoTexture"].IsNull() && !Reference(value["albedoTexture"], texture, error)) return false;
        result.albedoTexture = std::move(texture);
    }
    if (value.HasMember("specularColor")) {
        const auto& color = value["specularColor"];
        if (!color.IsArray() || color.Size() != 3) return Fail(error, "specularColor must be an array of 3 floats");
        Vector3 parsed;
        if (!Number(color[0], parsed.x, error) || !Number(color[1], parsed.y, error) ||
            !Number(color[2], parsed.z, error)) return false;
        result.specularColor = parsed;
    }
    if (value.HasMember("shininess")) {
        float parsed;
        if (!Number(value["shininess"], parsed, error)) return false;
        result.shininess = parsed;
    }
    return true;
}

bool Document(std::string_view json, rapidjson::Document& result, std::string& error) {
    if (json.starts_with("\xEF\xBB\xBF")) json.remove_prefix(3);
    if (json.empty()) return Fail(error, "empty material JSON");
    result.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag |
                 rapidjson::kParseIterativeFlag>(json.data(), json.size());
    if (result.HasParseError())
        return Fail(error, "invalid JSON at byte " + std::to_string(result.GetErrorOffset()) + ": " +
                           rapidjson::GetParseError_En(result.GetParseError()));
    if (!result.IsObject()) return Fail(error, "material JSON root must be an object");
    if (!result.HasMember("version") || !result["version"].IsUint() || result["version"].GetUint() != 1)
        return Fail(error, "material JSON requires version: 1");
    return true;
}

bool ValidParameters(const MaterialParameterOverrides& value, std::string& error) {
    if (value.albedoTexture && !value.albedoTexture->empty() && !LogicalName(*value.albedoTexture, error)) return false;
    const auto valid = [](float v) { return std::isfinite(v) && v >= 0; };
    if ((value.shininess && !valid(*value.shininess)) ||
        (value.specularColor && (!valid(value.specularColor->x) || !valid(value.specularColor->y) ||
                                !valid(value.specularColor->z))))
        return Fail(error, "material parameters must be finite non-negative floats");
    return true;
}

void String(Writer& writer, const std::string& value) {
    writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
}

void WriteParameters(Writer& writer, const MaterialParameterOverrides& value) {
    writer.StartObject();
    if (value.albedoTexture) {
        writer.Key("albedoTexture");
        if (value.albedoTexture->empty()) writer.Null();
        else String(writer, *value.albedoTexture);
    }
    if (value.specularColor) {
        writer.Key("specularColor");
        writer.StartArray();
        writer.Double(value.specularColor->x);
        writer.Double(value.specularColor->y);
        writer.Double(value.specularColor->z);
        writer.EndArray();
    }
    if (value.shininess) { writer.Key("shininess"); writer.Double(*value.shininess); }
    writer.EndObject();
}

template <typename T, typename Parse>
bool FinishWrite(const rapidjson::StringBuffer& buffer, std::string& json, std::string& error, Parse parse) {
    T checked;
    const std::string_view text{buffer.GetString(), buffer.GetSize()};
    if (!parse(text, checked, error)) return false; // includes UTF-8 validation for editor-authored strings
    json.assign(text);
    return true;
}

} // namespace

bool ParseMaterialDefinition(std::string_view json, MaterialDefinition& result, std::string& error) {
    rapidjson::Document root;
    if (!Document(json, root, error) ||
        !Fields(root, {"version", "shaderFile", "hooks", "defaults"}, "material", error)) return false;
    MaterialDefinition parsed;
    if (!root.HasMember("hooks") || !root["hooks"].IsArray())
        return Fail(error, "material requires an explicit hooks array (use [] for the built-in material)");
    for (const auto& hook : root["hooks"].GetArray()) {
        if (!hook.IsString()) return Fail(error, "each hook must be a string");
        MaterialHook bit;
        if (Text(hook) == "GBufferVS") bit = MaterialHook::GBufferVS;
        else if (Text(hook) == "GBufferPS") bit = MaterialHook::GBufferPS;
        else if (Text(hook) == "ShadowVS") bit = MaterialHook::ShadowVS;
        else return Fail(error, "unknown material hook '" + std::string(Text(hook)) + "'");
        if (HasHook(parsed.hooks, bit)) return Fail(error, "duplicate material hook");
        parsed.hooks = parsed.hooks | bit;
    }
    if (root.HasMember("shaderFile") && !Reference(root["shaderFile"], parsed.shaderFile, error)) return false;
    if (parsed.hooks != MaterialHook::None && parsed.shaderFile.empty())
        return Fail(error, "shaderFile is required when hooks are declared");
    if (root.HasMember("defaults") && !Parameters(root["defaults"], parsed.defaults, error)) return false;
    result = std::move(parsed);
    error.clear();
    return true;
}

bool ParseMaterialInstanceDefinition(std::string_view json, MaterialInstanceDefinition& result, std::string& error) {
    rapidjson::Document root;
    if (!Document(json, root, error) ||
        !Fields(root, {"version", "material", "parameters"}, "material instance", error)) return false;
    MaterialInstanceDefinition parsed;
    if (!root.HasMember("material")) return Fail(error, "material instance requires a material reference");
    if (!Reference(root["material"], parsed.material, error)) return false;
    if (root.HasMember("parameters") && !Parameters(root["parameters"], parsed.parameters, error)) return false;
    result = std::move(parsed);
    error.clear();
    return true;
}

bool SerializeMaterialDefinition(const MaterialDefinition& value, std::string& json, std::string& error) {
    if ((static_cast<uint8_t>(value.hooks) & ~KnownHooks) != 0)
        return Fail(error, "unknown material hook bits");
    if (!ValidParameters(value.defaults, error)) return false;
    rapidjson::StringBuffer buffer;
    Writer writer(buffer);
    writer.StartObject();
    writer.Key("version"); writer.Uint(1);
    if (!value.shaderFile.empty()) { writer.Key("shaderFile"); String(writer, value.shaderFile); }
    writer.Key("hooks"); writer.StartArray();
    if (HasHook(value.hooks, MaterialHook::GBufferVS)) writer.String("GBufferVS");
    if (HasHook(value.hooks, MaterialHook::GBufferPS)) writer.String("GBufferPS");
    if (HasHook(value.hooks, MaterialHook::ShadowVS)) writer.String("ShadowVS");
    writer.EndArray();
    writer.Key("defaults"); WriteParameters(writer, value.defaults);
    writer.EndObject();
    return FinishWrite<MaterialDefinition>(buffer, json, error, ParseMaterialDefinition);
}

bool SerializeMaterialInstanceDefinition(const MaterialInstanceDefinition& value, std::string& json,
                                         std::string& error) {
    if (!ValidParameters(value.parameters, error)) return false;
    rapidjson::StringBuffer buffer;
    Writer writer(buffer);
    writer.StartObject();
    writer.Key("version"); writer.Uint(1);
    writer.Key("material"); String(writer, value.material);
    writer.Key("parameters"); WriteParameters(writer, value.parameters);
    writer.EndObject();
    return FinishWrite<MaterialInstanceDefinition>(buffer, json, error, ParseMaterialInstanceDefinition);
}

} // namespace engine
