#pragma once

#include <engine/engine_export.hpp>

namespace engine {

	class Engine; 
	class InputSubsystem;
	
	ENGINE_API InputSubsystem* RegisterDefaultSubsystems(Engine& engine);

}