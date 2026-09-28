#pragma once

#include <string_view>

#include <engine/engine_export.hpp>

namespace engine {

class Engine;
struct FrameContext;

/// Base class for everything the engine runs every frame: rendering, input,
/// audio, physics, scripting, asset streaming, ...
///
/// Lifecycle (all driven by Engine, on the main thread):
///   Initialize()  once, in registration order
///   Update()      every Tick(), in registration order
///   Render()      every Tick() after all Updates, in registration order
///   Shutdown()    once, in reverse registration order
///
/// Subsystems find each other through GetEngine().GetSubsystem<T>() and talk
/// through GetEngine().GetEvents()
class ENGINE_API Subsystem {
public:
    virtual ~Subsystem();

    Subsystem(const Subsystem&) = delete;
    Subsystem& operator=(const Subsystem&) = delete;

    /// Human-readable name, used for logs and editor UI. Might get removed later, idk
    [[nodiscard]] virtual std::string_view GetName() const = 0;

    /// Return false if the subsystem can't run (log the reason); the engine
    /// then shuts down everything initialized so far. Engine code doesn't throw.
    [[nodiscard]] virtual bool Initialize();
    virtual void Shutdown();
    virtual void Update(const FrameContext& frame);
    virtual void Render(const FrameContext& frame);

protected:
    Subsystem() = default;

    /// Valid from Initialize() until after Shutdown().
    [[nodiscard]] Engine& GetEngine() const;

private:
    friend class Engine;
    Engine* engine_ = nullptr;
};

}  // namespace engine
