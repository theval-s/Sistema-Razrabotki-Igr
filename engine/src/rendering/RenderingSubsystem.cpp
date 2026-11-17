#include <engine/rendering/RenderingSubsystem.hpp>

#include <engine/engine.hpp>

#include <stdexcept>

namespace engine {

std::string_view RenderingSubsystem::GetName() const {
    return "RenderingSubsystem";
}

bool RenderingSubsystem::Initialize() {
    surface_ = GetEngine().GetSurface();
    resizePending_ = false;
    if (!renderer_.Initialize(surface_, GetEngine().GetConfig().graphicsDebugLayer)) {
        return false;
    }
    GetEngine().GetEvents().sink<SurfaceChangedEvent>().connect<&RenderingSubsystem::OnSurfaceChanged>(*this);
    return true;
}

void RenderingSubsystem::Shutdown() {
    ClearHostOverlayCallbacks();
    ClearOverlayCallbacks();
    GetEngine().GetEvents().sink<SurfaceChangedEvent>().disconnect<&RenderingSubsystem::OnSurfaceChanged>(*this);
    resizePending_ = false;
    renderer_.Shutdown();
}

void RenderingSubsystem::OnSurfaceChanged(const SurfaceChangedEvent& event) {
    surface_ = event.surface;
    resizePending_ = true;
}

void RenderingSubsystem::Render(const FrameContext& frame) {
    struct ClearDebugDraw {
        DebugDraw& draw;
        ~ClearDebugDraw() { draw.Clear(); }
    } clear{GetWorld().debugDraw};
    if (resizePending_ && renderer_.GetDiagnostics().lifecycle != RenderingLifecycle::Failed) {
        if (renderer_.Reinitialize(surface_, [this] {
            if (beforeHostOverlayResize_) beforeHostOverlayResize_();
            if (beforeOverlayResize_) beforeOverlayResize_();
        })) resizePending_ = false;
    }
    if (surface_.IsValid() && !resizePending_) {
        renderer_.RenderFrame(frame, [this](RenderingContext& context, const FrameContext& current) {
            if (hostOverlay_) hostOverlay_(context, current);
            if (overlay_) overlay_(context, current);
        });
    }
}

RenderingDiagnostics RenderingSubsystem::GetDiagnostics() const { return renderer_.GetDiagnostics(); }
RenderDebugView RenderingSubsystem::GetDebugView() const noexcept { return renderer_.GetDebugView(); }
bool RenderingSubsystem::SetDebugView(RenderDebugView view) noexcept { return renderer_.SetDebugView(view); }

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
