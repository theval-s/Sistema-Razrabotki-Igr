#include <engine/scripting/LuaGameBindings.hpp>
#include <engine/core/Log.hpp>
#include <engine/engine.hpp>

namespace engine {

    void RegisterGameBindings(sol::state& lua, LuaBindingsContext& context) {
        auto game = lua.create_named_table("Game");
        game.set_function("Quit", [&context] {
            if (!context.engine) return;
            if (context.engine->IsEditorMode()) {      // the editor owns the loop: use Stop
                log::Engine().info("Game.Quit() is ignored in the editor");
                return;
            }
            context.engine->RequestExit();
            });
    }

}