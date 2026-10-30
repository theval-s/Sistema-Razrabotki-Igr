#include <engine/rendering/RenderingSubsystem.hpp>

#include <engine/engine.hpp>

#include <spdlog/spdlog.h>

namespace engine {

std::string_view RenderingSubsystem::GetName() const {
    return "RenderingSubsystem";
}

bool RenderingSubsystem::Initialize() {
    surface_ = GetEngine().GetSurface();
    if (!surface_.IsValid()) {
        spdlog::error("RenderingSubsystem: the host gave the engine no window to render into");
        return false;
    }
    if (!renderer_.Initialize(surface_)) {
        spdlog::error("RenderingSubsystem: the renderer could not be created for {}x{}",
                      surface_.width, surface_.height);
        return false;
    }
    GetEngine().GetEvents().sink<SurfaceChangedEvent>().connect<&RenderingSubsystem::OnSurfaceChanged>(*this);
    return true;
}

void RenderingSubsystem::Shutdown() {
    GetEngine().GetEvents().sink<SurfaceChangedEvent>().disconnect<&RenderingSubsystem::OnSurfaceChanged>(*this);
}

void RenderingSubsystem::OnSurfaceChanged(const SurfaceChangedEvent& event) {
    surface_ = event.surface;
    resizePending_ = true;
}

void RenderingSubsystem::Render(const FrameContext& frame) {
    // A minimized window has no drawable area. Keep the pending resize for restoration.
    if (!surface_.IsValid()) {
        return;
    }
    if (resizePending_) {
        if (!renderer_.Reinitialize(surface_)) {
            return; // retry next frame; never draw with missing or mismatched resources
        }
        resizePending_ = false;
    }
    renderer_.RenderFrame(frame);
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

}  // namespace engine
