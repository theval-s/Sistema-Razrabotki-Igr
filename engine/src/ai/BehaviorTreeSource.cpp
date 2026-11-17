#include <engine/ai/BehaviorTreeSource.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <sol/sol.hpp>

namespace engine::ai {
namespace {

constexpr std::size_t MemoryLimit = 16 * 1024 * 1024;
constexpr int InstructionLimit = 1'000'000;
constexpr std::string_view Header =
    "-- Saved by the behavior tree editor, which rewrites this file: comments and formatting are not kept.\n";

[[noreturn]] void Fail(const std::string& error) { throw std::runtime_error(error); }

struct Memory {
    std::size_t used = 0;
    bool limited = true;
};

// A ceiling, so a definition that builds huge values fails instead of exhausting the editor.
void* Allocate(void* user, void* block, std::size_t oldSize, std::size_t newSize) {
    auto& memory = *static_cast<Memory*>(user);
    if (block == nullptr) oldSize = 0;  // then it is the new object's type, not a size
    if (newSize == 0) {
        std::free(block);
        memory.used -= oldSize;
        return nullptr;
    }
    if (memory.limited && newSize > oldSize && memory.used - oldSize + newSize > MemoryLimit) return nullptr;
    void* resized = std::realloc(block, newSize);
    if (resized != nullptr) memory.used = memory.used - oldSize + newSize;
    return resized;
}

void Exhausted(lua_State* state, lua_Debug*) { luaL_error(state, "the definition runs too long"); }

sol::table Table(const sol::object& object, const std::string& what) {
    if (object.get_type() != sol::type::table) Fail(what + " must be a table");
    return object.as<sol::table>();
}

std::string String(const sol::object& object, const std::string& what) {
    if (object.get_type() != sol::type::string) Fail(what + " must be a string");
    return object.as<std::string>();
}

std::vector<sol::object> Array(const sol::object& object, const std::string& what) {
    const auto table = Table(object, what);
    const auto length = table.size();
    std::vector<sol::object> items;
    items.reserve(length);
    for (std::size_t i = 1; i <= length; ++i) items.push_back(table.raw_get<sol::object>(i));
    std::size_t keys = 0;
    for (const auto& item : table) {
        (void)item;
        ++keys;
    }
    if (keys != length) Fail(what + " must be a list without gaps");
    return items;
}

NodeKind Kind(const std::string& name, const std::string& node) {
    for (const auto kind : {NodeKind::Sequence, NodeKind::Selector, NodeKind::Condition, NodeKind::Action})
        if (name == NodeKindName(kind)) return kind;
    Fail(node + ": unknown kind '" + name + "'");
}

ScriptPropertyMap Parameters(const sol::object& object, const std::string& node) {
    ScriptPropertyMap parameters;
    if (object.get_type() == sol::type::nil) return parameters;
    for (const auto& [key, value] : Table(object, node + ": parameters")) {
        const auto name = String(key, node + ": a parameter name");
        switch (value.get_type()) {
        case sol::type::boolean: parameters.emplace(name, value.as<bool>()); break;
        case sol::type::number: parameters.emplace(name, value.as<double>()); break;
        case sol::type::string: parameters.emplace(name, value.as<std::string>()); break;
        default: Fail(node + ": parameter '" + name + "' must be a string, boolean or number");
        }
    }
    std::string error;
    if (!ValidateScriptProperties(parameters, error)) Fail(node + ": " + error);
    return parameters;
}

TreeSource Tree(const sol::object& result) {
    const auto definition = Table(result, "The definition's result");
    TreeSource tree;
    tree.root = String(definition.raw_get<sol::object>("root"), "root");
    const auto nodes = Array(definition.raw_get<sol::object>("nodes"), "nodes");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto table = Table(nodes[i], "Node " + std::to_string(i + 1));
        NodeSource node;
        node.id = String(table.raw_get<sol::object>("id"), "Node " + std::to_string(i + 1) + ": id");
        const auto name = "Node '" + node.id + "'";
        node.kind = Kind(String(table.raw_get<sol::object>("kind"), name + ": kind"), name);
        if (const auto children = table.raw_get<sol::object>("children"); children.get_type() != sol::type::nil)
            for (const auto& child : Array(children, name + ": children"))
                node.children.push_back(String(child, name + ": a child"));
        if (const auto script = table.raw_get<sol::object>("script"); script.get_type() != sol::type::nil)
            node.script = String(script, name + ": script");
        node.parameters = Parameters(table.raw_get<sol::object>("parameters"), name);
        tree.nodes.push_back(std::move(node));
    }
    return tree;
}

bool Identifier(std::string_view name) {
    static constexpr std::array<std::string_view, 22> keywords{
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if",
        "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !name.empty() && letter(name.front()) && std::ranges::find(keywords, name) == keywords.end() &&
           std::ranges::all_of(name, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

std::string Quote(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': quoted += "\\\""; break;
        case '\\': quoted += "\\\\"; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\t': quoted += "\\t"; break;
        default:
            if (const auto byte = static_cast<unsigned char>(c); byte < 0x20 || byte == 0x7f) {
                std::array<char, 8> escape{};
                std::snprintf(escape.data(), escape.size(), "\\%03u", byte);
                quoted += escape.data();
            } else {
                quoted += c;
            }
        }
    }
    return quoted + "\"";
}

std::string Value(const StateValue& value) {
    if (const auto* text = std::get_if<std::string>(&value.value)) return Quote(*text);
    if (const auto* boolean = std::get_if<bool>(&value.value)) return *boolean ? "true" : "false";
    if (const auto* integer = std::get_if<std::int64_t>(&value.value)) return std::to_string(*integer);
    if (const auto* integer = std::get_if<std::uint64_t>(&value.value)) return std::to_string(*integer);
    const double number = value.Number();
    // Spelled so that reading the file back reports them instead of reading a nil global.
    if (std::isnan(number)) return "0/0";
    if (std::isinf(number)) return number > 0 ? "1/0" : "-1/0";
    std::array<char, 32> buffer{};
    const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    return {buffer.data(), written.ptr};
}

}  // namespace

const char* NodeKindName(const NodeKind kind) {
    switch (kind) {
    case NodeKind::Sequence: return "Sequence";
    case NodeKind::Selector: return "Selector";
    case NodeKind::Condition: return "Condition";
    case NodeKind::Action: return "Action";
    }
    return "Action";
}

bool ReadTreeSource(std::string_view source, TreeSource& tree, std::string& error) {
    Memory memory;  // outlives the state
    const std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(Allocate, &memory), lua_close);
    if (!state) {
        error = "Cannot create a Lua state to read the definition";
        return false;
    }
    lua_sethook(state.get(), Exhausted, LUA_MASKCOUNT, InstructionLimit);
    // Text only: a precompiled chunk can crash the reader.
    if (luaL_loadbufferx(state.get(), source.data(), source.size(), "=definition", "t") != LUA_OK ||
        lua_pcall(state.get(), 0, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(state.get(), -1);
        error = message != nullptr ? message : "The definition raised an error";
        return false;
    }
    // Reading runs outside a protected call, where a Lua error would abort the editor.
    lua_sethook(state.get(), nullptr, 0, 0);
    memory.limited = false;
    try {
        TreeSource read = Tree(sol::stack::pop<sol::object>(state.get()));
        tree = std::move(read);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::string WriteTreeSource(const TreeSource& tree) {
    std::string text(Header);
    text += "return {\n    root = " + Quote(tree.root) + ",\n    nodes = {\n";
    for (const auto& node : tree.nodes) {
        text += "        { id = " + Quote(node.id) + ", kind = " + Quote(NodeKindName(node.kind));
        if (!node.children.empty()) {
            text += ", children = {";
            for (std::size_t i = 0; i < node.children.size(); ++i) text += (i == 0 ? " " : ", ") + Quote(node.children[i]);
            text += " }";
        }
        if (!node.script.empty()) text += ", script = " + Quote(node.script);
        if (!node.parameters.empty()) {
            text += ", parameters = {";
            bool first = true;
            for (const auto& [name, value] : node.parameters) {
                text += first ? " " : ", ";
                first = false;
                text += (Identifier(name) ? name : "[" + Quote(name) + "]") + " = " + Value(value);
            }
            text += " }";
        }
        text += " },\n";
    }
    text += "    },\n}\n";
    return text;
}

bool ValidateTreeSource(const TreeSource& tree, const AssetCatalog& catalog, std::string& error) {
    try {
        std::unordered_map<std::string, std::size_t> ids;
        for (std::size_t i = 0; i < tree.nodes.size(); ++i) ids.emplace(tree.nodes[i].id, i);
        const auto index = [&](const std::string& id, const std::string& what) {
            const auto found = ids.find(id);
            if (found == ids.end()) Fail(what + ": no node '" + id + "'");
            return found->second;
        };
        const auto root = index(tree.root, "root");
        std::vector<Node> nodes;
        nodes.reserve(tree.nodes.size());
        for (const auto& source : tree.nodes) {
            const auto what = "node '" + source.id + "'";
            Node node{source.id, source.kind, {}, {}, source.parameters};
            if (source.kind == NodeKind::Condition || source.kind == NodeKind::Action) {
                if (source.script.empty()) Fail(what + ": no script");
                // As the runtime resolves it: a UUID, else a path.
                const auto* script = catalog.Find(std::string_view(source.script), CatalogAssetKind::Script);
                if (script == nullptr) script = catalog.FindPath(source.script);
                if (script == nullptr || script->metadata.kind != CatalogAssetKind::Script)
                    Fail(what + ": no script asset '" + source.script + "'");
                node.script = {script->metadata.id, script->path};
            }
            for (const auto& child : source.children) node.children.push_back(index(child, what));
            nodes.push_back(std::move(node));
        }
        if (!TreeDefinition::Create(std::move(nodes), root, error)) return false;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    error.clear();
    return true;
}

}
