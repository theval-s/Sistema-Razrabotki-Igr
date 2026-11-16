#pragma once

#include <sol/sol.hpp>
#include <engine/scripting/LuaBindingsContext.hpp>

namespace engine {
void RegisterAnimationBindings(sol::state& lua, LuaBindingsContext& context);
}
