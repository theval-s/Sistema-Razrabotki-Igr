#include <engine/core/DefaultSubsystems.hpp>

#include <utility>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>

namespace engine {

    InputSubsystem* RegisterDefaultSubsystems(Engine& engine, std::shared_ptr<UiHost> uiHost) {
        auto* input = engine.AddSubsystem<InputSubsystem>();
        engine.AddSubsystem<RenderingSubsystem>();
        UiSubsystemConfig uiConfig;
        uiConfig.host = std::move(uiHost);
#ifndef NDEBUG
        uiConfig.enableDebugger = true;
#endif
        engine.AddSubsystem<UiSubsystem>(std::move(uiConfig));
        engine.AddSubsystem<ScriptSubsystem>();
        engine.AddSubsystem<PhysicsSubsystem>();
        engine.AddSubsystem<RenderExtractionSubsystem>();
        return input;
    }

}  // namespace engine
