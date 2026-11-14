#include <engine/core/DefaultSubsystems.hpp>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>

namespace engine {

    InputSubsystem* RegisterDefaultSubsystems(Engine& engine) {
        auto* input = engine.AddSubsystem<InputSubsystem>();
        engine.AddSubsystem<ScriptSubsystem>();
        engine.AddSubsystem<RenderingSubsystem>();
        engine.AddSubsystem<PhysicsSubsystem>();
        engine.AddSubsystem<RenderExtractionSubsystem>();
        return input;
    }

}  // namespace engine