#pragma once

#include <sol/sol.hpp>

#include <engine/scripting/LuaBindingsContext.hpp>

namespace engine {

    void RegisterEntityHandleBindings(sol::state& lua);

    void RegisterSceneBindings(
        sol::state& lua,
        LuaBindingsContext& context
    );

    void RegisterLuaBindings(
        sol::state& lua,
        LuaBindingsContext& context
    );

} // namespace engine