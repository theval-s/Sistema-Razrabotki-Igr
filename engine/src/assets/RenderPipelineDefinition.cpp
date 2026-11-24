#include <engine/assets/RenderPipelineDefinition.hpp>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace engine {
namespace {
using Json = nlohmann::json;

void Require(bool condition, const std::string& error) {
    if (!condition) throw std::invalid_argument(error);
}

void Fields(const Json& object, std::initializer_list<std::string_view> fields) {
    Require(object.is_object(), "expected render pipeline object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const auto field : fields) known |= it.key() == field;
        Require(known, "unknown render pipeline field: " + it.key());
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

std::string_view BlendName(ProceduralBlend value) {
    switch (value) {
    case ProceduralBlend::Opaque: return "Opaque";
    case ProceduralBlend::Alpha: return "Alpha";
    case ProceduralBlend::Additive: return "Additive";
    }
    return {};
}

std::string_view DepthName(ProceduralDepth value) {
    switch (value) {
    case ProceduralDepth::Disabled: return "Disabled";
    case ProceduralDepth::ReadOnly: return "ReadOnly";
    case ProceduralDepth::ReadWrite: return "ReadWrite";
    }
    return {};
}
} // namespace

bool ValidateRenderPipelineDefinition(const RenderPipelineDefinition& value, std::string& error) {
    if (value.version != 1) { error = "expected render pipeline version 1"; return false; }
    if (!value.shader.id.IsValid()) { error = "render pipeline requires a valid shader UUID"; return false; }
    if (!Text(value.shader.path)) { error = "invalid shader path hint"; return false; }
    if (!Identifier(value.vertexEntryPoint)) { error = "vertexEntryPoint must be a nonempty HLSL identifier"; return false; }
    if (!Identifier(value.pixelEntryPoint)) { error = "pixelEntryPoint must be a nonempty HLSL identifier"; return false; }
    if (BlendName(value.blend).empty()) { error = "invalid render pipeline blend"; return false; }
    if (DepthName(value.depth).empty()) { error = "invalid render pipeline depth"; return false; }
    std::set<std::string> names;
    for (const auto& define : value.defines) {
        if (!Identifier(define.name)) { error = "define name must be a nonempty HLSL identifier"; return false; }
        if (!names.insert(define.name).second) { error = "duplicate shader define: " + define.name; return false; }
        if (!Text(define.value)) { error = "invalid shader define value"; return false; }
    }
    error.clear();
    return true;
}

bool ParseRenderPipelineDefinition(std::string_view text, RenderPipelineDefinition& value, std::string& error) {
    try {
        std::vector<std::set<std::string>> keys;
        const auto callback = [&keys](int depth, Json::parse_event_t event, Json& item) {
            Require(depth <= 8, "render pipeline JSON nesting is too deep");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key)
                Require(keys.back().insert(item.get<std::string>()).second, "duplicate render pipeline field");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        };
        const auto root = Json::parse(text.begin(), text.end(), callback);
        Fields(root, {"version", "shader", "vertexEntryPoint", "pixelEntryPoint", "defines", "blend", "depth"});
        Require(root.at("version").is_number_integer() && root.at("version") == 1,
                "expected render pipeline version 1");
        const auto& shader = root.at("shader");
        Fields(shader, {"id", "path"});
        RenderPipelineDefinition result;
        result.shader.id = AssetUuid(shader.at("id").get<std::string>());
        if (shader.contains("path")) result.shader.path = shader.at("path").get<std::string>();
        if (root.contains("vertexEntryPoint")) result.vertexEntryPoint = root.at("vertexEntryPoint").get<std::string>();
        if (root.contains("pixelEntryPoint")) result.pixelEntryPoint = root.at("pixelEntryPoint").get<std::string>();
        if (root.contains("defines")) {
            const auto& defines = root.at("defines");
            Require(defines.is_array(), "defines must be an array");
            for (const auto& define : defines) {
                Fields(define, {"name", "value"});
                result.defines.push_back({define.at("name").get<std::string>(), define.at("value").get<std::string>()});
            }
        }
        if (root.contains("blend")) {
            const auto name = root.at("blend").get<std::string>();
            if (name == "Opaque") result.blend = ProceduralBlend::Opaque;
            else if (name == "Alpha") result.blend = ProceduralBlend::Alpha;
            else if (name == "Additive") result.blend = ProceduralBlend::Additive;
            else throw std::invalid_argument("invalid render pipeline blend: " + name);
        }
        if (root.contains("depth")) {
            const auto name = root.at("depth").get<std::string>();
            if (name == "Disabled") result.depth = ProceduralDepth::Disabled;
            else if (name == "ReadOnly") result.depth = ProceduralDepth::ReadOnly;
            else if (name == "ReadWrite") result.depth = ProceduralDepth::ReadWrite;
            else throw std::invalid_argument("invalid render pipeline depth: " + name);
        }
        if (!ValidateRenderPipelineDefinition(result, error)) return false;
        value = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool SerializeRenderPipelineDefinition(const RenderPipelineDefinition& value, std::string& text, std::string& error) {
    try {
        if (!ValidateRenderPipelineDefinition(value, error)) return false;
        Json shader = {{"id", value.shader.id.ToString()}};
        if (!value.shader.path.empty()) shader["path"] = value.shader.path;
        Json defines = Json::array();
        for (const auto& define : value.defines)
            defines.push_back({{"name", define.name}, {"value", define.value}});
        auto encoded = Json{{"version", 1}, {"shader", std::move(shader)},
                            {"vertexEntryPoint", value.vertexEntryPoint}, {"pixelEntryPoint", value.pixelEntryPoint},
                            {"defines", std::move(defines)}, {"blend", BlendName(value.blend)},
                            {"depth", DepthName(value.depth)}}.dump(2);
        text = std::move(encoded);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool LoadRenderPipelineDefinition(const std::filesystem::path& file, RenderPipelineDefinition& value,
                                  std::string& error) {
    std::string text;
    if (!ReadTextFile(file, text, error, 1024 * 1024)) return false;
    if (!ParseRenderPipelineDefinition(text, value, error)) {
        error = ToGenericUtf8(file) + ": " + error;
        return false;
    }
    return true;
}

bool RenderPipelineShaderVariants(const RenderPipelineDefinition& definition, const AssetCatalog& catalog,
                                  std::array<ShaderVariant, 2>& value, std::string& error) {
    if (!ValidateRenderPipelineDefinition(definition, error)) return false;
    const auto* entry = catalog.Find(definition.shader.id, CatalogAssetKind::Shader);
    if (!entry) {
        error = "render pipeline shader references missing or wrong-kind asset " + definition.shader.id.ToString() +
            (definition.shader.path.empty() ? "" : " (" + definition.shader.path + ")");
        return false;
    }
    std::array<ShaderVariant, 2> result{
        ShaderVariant{entry->source, ShaderType::Vertex, {definition.vertexEntryPoint, definition.defines}},
        ShaderVariant{entry->source, ShaderType::Pixel, {definition.pixelEntryPoint, definition.defines}}};
    value = std::move(result);
    error.clear();
    return true;
}

} // namespace engine
