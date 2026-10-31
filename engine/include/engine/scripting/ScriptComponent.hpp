#pragma once
#include <engine/assets/AssetCatalog.hpp>

namespace engine {
/// Authored data only. Lua instances belong to ScriptSubsystem, never the scene file.
struct ScriptComponent {
    AssetReference script;
    bool enabled = true;
};
}
