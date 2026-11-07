#pragma once
#include <functional>

namespace engine {

    class Engine;
    class Scene;

    /// Context shared by all Lua binding groups.
    struct LuaBindingsContext {
        Engine* engine = nullptr;
        std::function<void(std::function<void()>)> enqueueSceneCommand;
        [[nodiscard]] Scene* GetScene() const noexcept;
    };

} // namespace engine