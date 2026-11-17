#include <engine/rendering/RenderingSubsystem.hpp>

#include <engine/engine.hpp>

#include <engine/core/Log.hpp>
#include <stdexcept>

namespace engine {

std::string_view RenderingSubsystem::GetName() const {
    return "RenderingSubsystem";
}

bool RenderingSubsystem::Initialize() {
    surface_ = GetEngine().GetSurface();
    if (!surface_.IsValid()) {
        log::Render().error("The host gave the engine no window to render into");
        return false;
    }
    if (!renderer_.Initialize(surface_, GetEngine().GetConfig().graphicsDebugLayer)) {
        log::Render().error("The renderer could not be created for {}x{}",
                      surface_.width, surface_.height);
        return false;
    }
    GetEngine().GetEvents().sink<SurfaceChangedEvent>().connect<&RenderingSubsystem::OnSurfaceChanged>(*this);
    return true;
}

void RenderingSubsystem::Shutdown() {
    ClearHostOverlayCallbacks();
    ClearOverlayCallbacks();
    GetEngine().GetEvents().sink<SurfaceChangedEvent>().disconnect<&RenderingSubsystem::OnSurfaceChanged>(*this);
}

void RenderingSubsystem::OnSurfaceChanged(const SurfaceChangedEvent& event) {
    surface_ = event.surface;
    resizePending_ = true;
}

void RenderingSubsystem::Render(const FrameContext& frame) {
    // A minimized window has no drawable area. Keep the pending resize for restoration.
    if (surface_.IsValid() && resizePending_) {
        if (beforeHostOverlayResize_) beforeHostOverlayResize_();
        if (beforeOverlayResize_) beforeOverlayResize_();
        if (renderer_.Reinitialize(surface_)) resizePending_ = false;
    }
    // A failed resize is retried next frame; never draw with missing or mismatched resources.
    if (surface_.IsValid() && !resizePending_) {
        renderer_.RenderFrame(frame, [this](RenderingContext& context, const FrameContext& current) {
            if (hostOverlay_) hostOverlay_(context, current);
            if (overlay_) overlay_(context, current);
        });
    }
    GetWorld().debugDraw.Clear();
}

RenderingSystem& RenderingSubsystem::GetRendering() noexcept {
    return renderer_;
}

RenderingContext& RenderingSubsystem::GetContext() {
    return renderer_.GetRenderingContext();
}

RenderingResourceManager& RenderingSubsystem::GetResourceManager() {
    return renderer_.GetResourceManager();
}

RenderWorld& RenderingSubsystem::GetWorld() {
    return renderer_.GetWorld();
}

const NativeSurface& RenderingSubsystem::GetSurface() const noexcept {
    return surface_;
}

void RenderingSubsystem::SetOverlayCallbacks(RenderingSystem::OverlayCallback render,
                                             std::function<void()> beforeResize) {
    if (overlay_) throw std::logic_error("A rendering overlay is already registered");
    overlay_ = std::move(render);
    beforeOverlayResize_ = std::move(beforeResize);
}

void RenderingSubsystem::ClearOverlayCallbacks() noexcept {
    overlay_ = {};
    beforeOverlayResize_ = {};
}

void RenderingSubsystem::SetHostOverlayCallbacks(RenderingSystem::OverlayCallback render,
                                                 std::function<void()> beforeResize) {
    if (hostOverlay_) throw std::logic_error("A host rendering overlay is already registered");
    hostOverlay_ = std::move(render);
    beforeHostOverlayResize_ = std::move(beforeResize);
}

void RenderingSubsystem::ClearHostOverlayCallbacks() noexcept {
    hostOverlay_ = {};
    beforeHostOverlayResize_ = {};
}

}  // namespace engine
