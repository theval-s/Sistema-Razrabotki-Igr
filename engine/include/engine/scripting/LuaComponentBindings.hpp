#pragma once
#include <functional>
#include <sol/sol.hpp>
#include <engine/engine_export.hpp>
#include <engine/scripting/LuaBindingsContext.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>

namespace engine {
// Components are detached value tables. SetComponent validates a patch against a
// copied component before committing; no registry references escape into Lua.
ENGINE_API void RegisterComponentBindings(sol::state& lua, LuaBindingsContext& context);
// Validate all spawn component/script data before creating the entity.
std::function<void(const LuaEntityHandle&)> PrepareLuaSpawnComponents(
    LuaBindingsContext& context, sol::optional<sol::table> options);
}
