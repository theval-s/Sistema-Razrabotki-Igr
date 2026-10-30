#include <engine/scripting/AutoloadDefinition.hpp>

#include <string_view>
#include <unordered_set>

namespace engine {
namespace {

bool IsIdentifier(std::string_view name) {
    if (name.empty()) return false;
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    if (!letter(name.front())) return false;
    for (const char c : name)
        if (!letter(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

}  // namespace

bool ValidateAutoloadDefinitions(std::span<const AutoloadDefinition> definitions, std::string& error) {
    std::unordered_set<std::string> names;
    std::unordered_set<AssetUuid, AssetUuidHash> scripts;
    for (const auto& definition : definitions) {
        if (!IsIdentifier(definition.name)) {
            error = "Autoload name '" + definition.name + "' must be an ASCII identifier ([A-Za-z_][A-Za-z0-9_]*).";
            return false;
        }
        if (!names.insert(definition.name).second) {
            error = "Duplicate autoload name: " + definition.name;
            return false;
        }
        std::string reference;
        if (!SerializeAssetReference(definition.script, reference, error)) {
            error = "Autoload '" + definition.name + "': " + error;
            return false;
        }
        if (!scripts.insert(definition.script.id).second) {
            error = "Script UUID is already used by another autoload: " + definition.script.id.ToString();
            return false;
        }
    }
    error.clear();
    return true;
}

}  // namespace engine
