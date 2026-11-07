#include <engine/scripting/LuaSaveGameBindings.hpp>
#include <engine/engine.hpp>

namespace engine {
void RegisterSaveGameBindings(sol::state& lua, LuaBindingsContext& context) {
    lua.new_usertype<SaveGameRequest>("SaveGameRequest", sol::no_constructor,
        "IsComplete", &SaveGameRequest::IsComplete,
        "GetResult", [](const SaveGameRequest& request) {
            const auto [ok, error] = request.GetResult();
            return std::make_tuple(ok, error);
        });
    auto saves = lua.create_named_table("SaveGame");
    const auto queue = [&context](const std::string& slot, bool load) {
        std::string error;
        std::shared_ptr<SaveGameRequest> request;
        if (!context.engine) error = "SaveGame requires an owning Engine";
        else request = load ? context.engine->RequestLoad(slot, error) : context.engine->RequestSave(slot, error);
        return std::make_tuple(request, error);
    };
    saves.set_function("RequestSave", [queue](const std::string& slot) { return queue(slot, false); });
    saves.set_function("RequestLoad", [queue](const std::string& slot) { return queue(slot, true); });
}
}
