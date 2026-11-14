#pragma once

#include <string_view>

#include <engine/rendering/RenderingSystem.hpp>
#include <engine/core/Subsystem.hpp>
#include <engine/core/Surface.hpp>

namespace engine {

struct SurfaceChangedEvent;

/// Bridges the existing RenderingSystem into the Engine kernel: it renders into
/// Engine::GetSurface(), so the host keeps owning the window and its message pump.
class ENGINE_API RenderingSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Render(const FrameContext& frame) override;

    [[nodiscard]] RenderingSystem& GetRendering() noexcept;
    [[nodiscard]] RenderingContext& GetContext();
    [[nodiscard]] RenderingResourceManager& GetResourceManager();
    [[nodiscard]] RenderWorld& GetWorld();
    /// The surface this subsystem renders into, as handed by the host.
    [[nodiscard]] const NativeSurface& GetSurface() const noexcept;

    // Single engine-owned overlay lease. The owner must detach before its shutdown;
    // duplicate registration is rejected rather than replacing a live callback.
    void SetOverlayCallbacks(RenderingSystem::OverlayCallback render, std::function<void()> beforeResize);
    void ClearOverlayCallbacks() noexcept;

private:
    void OnSurfaceChanged(const SurfaceChangedEvent& event);

    RenderingSystem renderer_{};
    NativeSurface surface_{};
    bool resizePending_ = false;
    RenderingSystem::OverlayCallback overlay_;
    std::function<void()> beforeOverlayResize_;
};

}  // namespace engine
