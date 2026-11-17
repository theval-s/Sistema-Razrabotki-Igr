#pragma once
#include <memory>
#include <functional>
#include <vector>

#include <engine/core/Surface.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine_export.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/rendering/RenderDebugView.hpp>

#include <engine/rendering/passes/GBufferPass.hpp>
#include <engine/rendering/passes/LightPass.hpp>
#include <engine/rendering/passes/PresentPass.hpp>
#include <engine/rendering/passes/SelectionMaskPass.hpp>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/passes/RenderPass.hpp>
#include <engine/rendering/passes/ShadowPass.hpp>


namespace engine {

struct ENGINE_API RenderingSystem {
    /// Explicit fresh attempt after construction/Shutdown: clears old failure and resets
    /// debug view to Final. Returns true only when Ready. Catches RenderingException.
    /// Zero extent returns false/SurfaceUnavailable; missing window returns false/Failed.
    bool Initialize(const NativeSurface& surface, bool debugLayer);
    /// Same-window resize/restore; false while suspended or failed. No retry from Failed:
    /// Shutdown + Initialize is required. Preserves debug view. Zero extent suspends.
    /// beforeResize releases host/UI back-buffer references inside the same typed-error
    /// boundary, before ClearState/ResizeBuffers; not invoked while unavailable/failed.
    bool Reinitialize(const NativeSurface& surface, const std::function<void()>& beforeResize = {});
    /// Idempotent partial cleanup. Failed and its first failure survive; otherwise
    /// becomes Uninitialized. Releases GPU resources; external GPU handles become stale.
    void Shutdown() noexcept;
    [[nodiscard]] RenderingDiagnostics GetDiagnostics() const;
    [[nodiscard]] RenderDebugView GetDebugView() const noexcept;
    /// Invalid values return false without changing selection. Valid values may be set
    /// while unavailable; no GPU work or asset reload. Engine thread only.
    [[nodiscard]] bool SetDebugView(RenderDebugView view) noexcept;
    using OverlayCallback = std::function<void(RenderingContext&, const FrameContext&)>;
    // Ready only. Catches RenderingException, latches Failed and stops this frame.
    // The final scene target remains bound during the overlay. This function presents once.
    void RenderFrame(const FrameContext& frame, const OverlayCallback& overlay = {});
    ~RenderingSystem();

    [[nodiscard]] RenderingContext& GetRenderingContext();
    [[nodiscard]] RenderingResourceManager& GetResourceManager();
    [[nodiscard]] RenderWorld& GetWorld();
    /// Low-level inspection/readback view; engine thread only. The reference is
    /// borrowed from this renderer, not an owned diagnostics snapshot or GPU lease.
    /// Contents change during rendering and lifecycle calls. Use GPU handles only
    /// while Ready, reacquire them after Initialize/Reinitialize/Shutdown, and do
    /// not destroy or modify renderer-owned resources through copied handles.
    /// Ready alone does not imply rendered pixels: read completed pass outputs in
    /// the overlay callback or after a successful frame. Native readback must obey
    /// D3D binding/synchronization rules and release resource references before resize.
    [[nodiscard]] const RenderingResources& GetResources() const noexcept { return resources_; }

private:
    void InitializePasses(uint32_t screenWidth, uint32_t screenHeight);
    void RecordFailure(RenderingFailure failure);

    RenderingContext renderingContext_{};
    RenderWorld world_{};
    RenderingResources resources_{0, 0};
    std::vector<std::unique_ptr<RenderPass>> renderPasses_{};
    bool ready_ = false;
    RenderingDiagnostics diagnostics_{};
    RenderDebugView debugView_ = RenderDebugView::Final;
    NativeSurface initializedSurface_{};
};

}
