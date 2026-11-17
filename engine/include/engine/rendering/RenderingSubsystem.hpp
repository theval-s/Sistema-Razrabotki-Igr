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
    /// Owned copy of renderer status, queryable after Engine::Initialize() fails.
    /// Same engine-thread and lifecycle contract as RenderingSystem.
    [[nodiscard]] RenderingDiagnostics GetDiagnostics() const;
    [[nodiscard]] RenderDebugView GetDebugView() const noexcept;
    [[nodiscard]] bool SetDebugView(RenderDebugView view) noexcept;
    /// The surface this subsystem renders into, as handed by the host.
    [[nodiscard]] const NativeSurface& GetSurface() const noexcept;

    // Single engine-owned overlay lease. The owner must detach before its shutdown;
    // duplicate registration is rejected rather than replacing a live callback.
    void SetOverlayCallbacks(RenderingSystem::OverlayCallback render, std::function<void()> beforeResize);
    void ClearOverlayCallbacks() noexcept;

    /// Optional native-host diagnostics, composited after the scene and before
    /// the engine UI, into the same target before Present. Hosts must detach
    /// before their resources die. This lease is independent of the UI lease.
    void SetHostOverlayCallbacks(RenderingSystem::OverlayCallback render, std::function<void()> beforeResize = {});
    void ClearHostOverlayCallbacks() noexcept;

private:
    void OnSurfaceChanged(const SurfaceChangedEvent& event);

    RenderingSystem renderer_{};
    NativeSurface surface_{};
    bool resizePending_ = false;
    RenderingSystem::OverlayCallback overlay_;
    std::function<void()> beforeOverlayResize_;
    RenderingSystem::OverlayCallback hostOverlay_;
    std::function<void()> beforeHostOverlayResize_;
};

}  // namespace engine
