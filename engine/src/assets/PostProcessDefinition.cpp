#include <engine/assets/PostProcessDefinition.hpp>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

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

void Fields(const Json& object, std::initializer_list<std::string_view> fields) {
    Require(object.is_object(), "expected postprocess object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const auto field : fields) known |= it.key() == field;
        Require(known, "unknown postprocess field: " + it.key());
    }
}

bool Identifier(std::string_view value) {
    const auto letter = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    if (value.empty() || !letter(value.front())) return false;
    for (const unsigned char c : value)
        if (!letter(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

bool Text(std::string_view value) {
    for (const unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
} // namespace

bool ValidatePostProcessDefinition(const PostProcessDefinition& value, std::string& error) {
    if (value.version != 1) { error = "expected postprocess version 1"; return false; }
    if (!value.shader.id.IsValid()) { error = "postprocess requires a valid shader UUID"; return false; }
    if (!Text(value.shader.path)) { error = "invalid shader path hint"; return false; }
    if (!Identifier(value.options.entryPoint)) { error = "entryPoint must be a nonempty HLSL identifier"; return false; }
    std::set<std::string> names;
    for (const auto& define : value.options.defines) {
        if (!Identifier(define.name)) { error = "define name must be a nonempty HLSL identifier"; return false; }
        if (!names.insert(define.name).second) { error = "duplicate shader define: " + define.name; return false; }
        if (!Text(define.value)) { error = "invalid shader define value"; return false; }
    }
    error.clear();
    return true;
}

bool ParsePostProcessDefinition(std::string_view text, PostProcessDefinition& value, std::string& error) {
    try {
        std::vector<std::set<std::string>> keys;
        const auto callback = [&keys](int depth, Json::parse_event_t event, Json& item) {
            Require(depth <= 8, "postprocess JSON nesting is too deep");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key)
                Require(keys.back().insert(item.get<std::string>()).second, "duplicate postprocess field");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        };
        const auto root = Json::parse(text.begin(), text.end(), callback);
        Fields(root, {"version", "shader", "entryPoint", "defines"});
        Require(root.at("version").is_number_integer() && root.at("version") == 1,
                "expected postprocess version 1");
        const auto& shader = root.at("shader");
        Fields(shader, {"id", "path"});
        PostProcessDefinition result;
        result.shader.id = AssetUuid(shader.at("id").get<std::string>());
        if (shader.contains("path")) result.shader.path = shader.at("path").get<std::string>();
        if (root.contains("entryPoint")) result.options.entryPoint = root.at("entryPoint").get<std::string>();
        if (root.contains("defines")) {
            const auto& defines = root.at("defines");
            Require(defines.is_array(), "defines must be an array");
            for (const auto& define : defines) {
                Fields(define, {"name", "value"});
                result.options.defines.push_back({define.at("name").get<std::string>(),
                                                 define.at("value").get<std::string>()});
            }
        }
        if (!ValidatePostProcessDefinition(result, error)) return false;
        value = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool SerializePostProcessDefinition(const PostProcessDefinition& value, std::string& text, std::string& error) {
    try {
        if (!ValidatePostProcessDefinition(value, error)) return false;
        Json shader = {{"id", value.shader.id.ToString()}};
        if (!value.shader.path.empty()) shader["path"] = value.shader.path;
        Json defines = Json::array();
        for (const auto& define : value.options.defines)
            defines.push_back({{"name", define.name}, {"value", define.value}});
        auto encoded = Json{{"version", 1}, {"shader", std::move(shader)},
                            {"entryPoint", value.options.entryPoint}, {"defines", std::move(defines)}}.dump(2);
        text = std::move(encoded);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool LoadPostProcessDefinition(const std::filesystem::path& file, PostProcessDefinition& value, std::string& error) {
    std::string text;
    if (!ReadTextFile(file, text, error, 1024 * 1024)) return false;
    if (!ParsePostProcessDefinition(text, value, error)) {
        error = ToGenericUtf8(file) + ": " + error;
        return false;
    }
    return true;
}

bool PostProcessShaderVariant(const PostProcessDefinition& definition, const AssetCatalog& catalog,
                              ShaderVariant& value, std::string& error) {
    if (!ValidatePostProcessDefinition(definition, error)) return false;
    const auto* entry = catalog.Find(definition.shader.id, CatalogAssetKind::Shader);
    if (!entry) {
        error = "postprocess shader references missing or wrong-kind asset " + definition.shader.id.ToString() +
            (definition.shader.path.empty() ? "" : " (" + definition.shader.path + ")");
        return false;
    }
    value = ShaderVariant{entry->source, ShaderType::Pixel, definition.options};
    error.clear();
    return true;
}

} // namespace engine
