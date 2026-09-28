#pragma once

#include <engine/core/Surface.hpp>

// Events published by the Engine itself on Engine::GetEvents().
// They rely on entt::dispatcher

namespace engine {

/// The output surface was set, replaced or resized.
struct SurfaceChangedEvent {
    NativeSurface surface;
};

/// Engine::SetPaused() changed the paused state.
struct PauseChangedEvent {
    bool paused = false;
};

/// Engine::RequestExit() was called; the current Run() loop ends after this frame.
struct ExitRequestedEvent {};

}  // namespace engine
