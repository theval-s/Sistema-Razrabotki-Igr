#pragma once

#include <sol/forward.hpp>

namespace engine {
struct LuaBindingsContext;
void RegisterAudioBindings(sol::state& lua, LuaBindingsContext& context);
void RegisterQueryBindings(sol::state& lua, LuaBindingsContext& context);
}
