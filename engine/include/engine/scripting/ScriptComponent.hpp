#pragma once
#include <engine/assets/AssetCatalog.hpp>
#include <engine/scripting/ScriptProperties.hpp>

namespace engine {
/// Authored data only. Lua instances belong to ScriptSubsystem, never the scene file.
struct ScriptComponent {
    AssetReference script;
    bool enabled = true;
    ScriptPropertyMap properties;
};
}
