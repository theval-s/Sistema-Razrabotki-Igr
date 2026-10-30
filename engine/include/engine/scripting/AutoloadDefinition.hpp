#pragma once

#include <span>
#include <string>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>

namespace engine {

/// Authored project settings. Vector order defines startup order; runtime state is separate.
struct AutoloadDefinition {
    std::string name;
    AssetReference script;
    bool enabled = true;
};

/// ASCII identifiers, unique names and script UUIDs, and well-formed asset references.
/// Does not access the catalog or disk: missing assets must not prevent opening a project.
[[nodiscard]] ENGINE_API bool ValidateAutoloadDefinitions(
    std::span<const AutoloadDefinition> definitions, std::string& error);

}  // namespace engine
