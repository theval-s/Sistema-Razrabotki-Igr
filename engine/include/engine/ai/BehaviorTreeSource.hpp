#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <engine/ai/BehaviorTree.hpp>
#include <engine/scripting/ScriptProperties.hpp>

namespace engine::ai {

/// One node of a .bt.lua file as written: children and script stay unresolved names.
struct NodeSource {
    std::string id;
    NodeKind kind = NodeKind::Action;
    std::vector<std::string> children;
    std::string script;
    ScriptPropertyMap parameters;
    bool operator==(const NodeSource&) const = default;
};

/// A .bt.lua file as data, nodes in file order, for tools that edit trees. Not validated.
struct TreeSource {
    std::string root;
    std::vector<NodeSource> nodes;
    bool operator==(const TreeSource&) const = default;
};

/// "Sequence", "Selector", "Condition" or "Action", as .bt.lua files spell them.
[[nodiscard]] ENGINE_API const char* NodeKindName(NodeKind kind);

/// Runs `source` in an empty Lua state (no libraries, bounded instructions and memory) and
/// reads the table it returns. Fields the runtime doesn't read are dropped.
/// On failure `tree` is unchanged.
[[nodiscard]] ENGINE_API bool ReadTreeSource(std::string_view source, TreeSource& tree, std::string& error);
/// The .bt.lua text the editor saves; ReadTreeSource gives `tree` back.
[[nodiscard]] ENGINE_API std::string WriteTreeSource(const TreeSource& tree);

}
