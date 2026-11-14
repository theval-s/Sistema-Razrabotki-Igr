#pragma once

#include <memory>

#include <engine/engine_export.hpp>

namespace engine {

	class Engine;
	class InputSubsystem;
	class UiHost;

	/// Input, rendering, UI, scripting, physics, render extraction; a host input adapter goes first.
	ENGINE_API InputSubsystem* RegisterDefaultSubsystems(Engine& engine, std::shared_ptr<UiHost> uiHost);

}
