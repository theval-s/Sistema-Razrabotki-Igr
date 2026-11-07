#pragma once

#include <sol/sol.hpp>

#include <engine/scripting/LuaBindingsContext.hpp>

namespace engine {

    void RegisterSceneBindings(
        sol::state& lua,
        LuaBindingsContext& context
    );

} // namespace engine