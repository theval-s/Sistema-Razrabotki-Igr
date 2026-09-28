#pragma once

#include <cstdint>

namespace engine {

/// Timing information for the frame currently being processed.
/// Passed to every Subsystem::Update / Subsystem::Render call.
/// Although I'm not really sure we need this
struct FrameContext {
    std::uint64_t frameIndex = 0;

    /// Wall-clock time since the previous frame, clamped to EngineConfig::maxDeltaSeconds.
    /// Use for things that must keep running while paused (editor camera, UI, hot-reload).
    double realDeltaSeconds = 0.0;
    double realTimeSeconds = 0.0;

    /// Simulation time step: equals realDeltaSeconds, or 0 while the engine is paused.
    /// Gameplay, physics, animation etc. should advance using this one.
    double deltaSeconds = 0.0;
    double simulationTimeSeconds = 0.0;

    bool paused = false; // or more states through enum
};

}  // namespace engine
