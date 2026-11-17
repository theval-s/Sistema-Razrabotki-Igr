#include <engine/rendering/RenderingSystem.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/PathUtils.hpp>
#include "LifecycleFailure.hpp"

namespace engine {
namespace {
constexpr const char* PassNames[] = {"ShadowCSMPass", "GBufferPass", "LightPass", "SelectionMaskPass", "PresentPass"};
}

void RenderingSystem::RecordFailure(RenderingFailure failure) {
    ready_ = false;
    if (diagnostics_.lifecycle == RenderingLifecycle::Failed) return;
    diagnostics_.lifecycle = RenderingLifecycle::Failed;
    diagnostics_.failure = std::move(failure);
    const auto& f = *diagnostics_.failure;
    std::string detail = f.detail;
    if (!f.path.empty()) detail += "\nPath: " + ToUtf8(f.path);
    if (f.nativeError) detail += fmt::format("\nHRESULT: 0x{:08X}", static_cast<std::uint32_t>(*f.nativeError));
    if (f.deviceRemovalReason) detail += fmt::format("\nDevice removal reason: 0x{:08X}", static_cast<std::uint32_t>(*f.deviceRemovalReason));
    if (f.shader) {
        detail += fmt::format("\nShader: {} {} {}", f.shader->stage, f.shader->entryPoint, f.shader->target);
        for (const auto& [name, value] : f.shader->defines) detail += "\nDefine: " + name + "=" + value;
    }
    log::Render().error("Rendering failed: {} [{} / {}]\n{}", f.summary, f.pass, f.operation, detail);
}

bool RenderingSystem::Initialize(const NativeSurface& surface, const bool debugLayer) {
    // A live device can still be used by asset workers. Hosts must quiesce it via
    // Engine::Shutdown before requesting a new attempt.
    if (initializedSurface_.windowHandle) return ready_;
    Shutdown();
    diagnostics_ = {};
    debugView_ = RenderDebugView::Final;
    try {
        if (!surface.windowHandle) rendering_detail::Check(E_INVALIDARG, "ValidateSurface", nullptr, "The host supplied no native window.");
        if (!surface.IsValid()) {
            diagnostics_.lifecycle = RenderingLifecycle::SurfaceUnavailable;
            return false;
        }
        if (!renderingContext_.Initialize(surface, debugLayer))
            rendering_detail::Check(E_FAIL, "InitializeContext");
        InitializePasses(surface.width, surface.height);
        initializedSurface_ = surface;
        diagnostics_.lifecycle = RenderingLifecycle::Ready;
        ready_ = true;
        return true;
    } catch (const RenderingException& error) {
        RecordFailure(error.GetFailure());
        Shutdown();
        return false;
    }
}

bool RenderingSystem::Reinitialize(const NativeSurface& surface, const std::function<void()>& beforeResize) {
    if (diagnostics_.lifecycle == RenderingLifecycle::Failed || !initializedSurface_.windowHandle) return false;
    ready_ = false;
    diagnostics_.lifecycle = RenderingLifecycle::SurfaceUnavailable;
    if (!surface.IsValid()) {
        diagnostics_.lifecycle = RenderingLifecycle::SurfaceUnavailable;
        return false;
    }
    try {
        if (surface.windowHandle != initializedSurface_.windowHandle)
            rendering_detail::Check(E_INVALIDARG, "ValidateSurface", nullptr, "A different native window requires Shutdown followed by Initialize.");
        if (beforeResize) beforeResize();
        renderingContext_.ClearState();
        auto& manager = renderingContext_.GetResourceManager();
        for (size_t i = 0; i < renderPasses_.size(); ++i) {
            rendering_detail::InPass(PassNames[i], [&] {
                if (!renderPasses_[i]->PreReinitialize(manager, renderingContext_, resources_))
                    rendering_detail::Check(E_FAIL, "PreReinitialize", nullptr, "Pass could not release resize resources.");
            });
        }
        renderingContext_.GetDeviceContext()->Flush();
        if (!renderingContext_.Reinitialize(surface)) rendering_detail::Check(E_FAIL, "ResizeBuffers");
        resources_.screenWith = surface.width;
        resources_.screenHeight = surface.height;
        for (size_t i = 0; i < renderPasses_.size(); ++i) {
            rendering_detail::InPass(PassNames[i], [&] {
                if (!renderPasses_[i]->Reinitialize(manager, renderingContext_, resources_))
                    rendering_detail::Check(E_FAIL, "ReinitializePass", nullptr, "Pass could not recreate resize resources.");
            });
        }
        initializedSurface_ = surface;
        diagnostics_.lifecycle = RenderingLifecycle::Ready;
        ready_ = true;
        return true;
    } catch (const RenderingException& error) {
        // Keep the device alive until Engine quiesces asset workers.
        RecordFailure(error.GetFailure());
        return false;
    }
}

void RenderingSystem::InitializePasses(const uint32_t width, const uint32_t height) {
    auto& manager = renderingContext_.GetResourceManager();
    resources_ = RenderingResources(width, height);
    resources_.Initialize(manager);
    // Each pass enters ownership before its Initialize can acquire partial resources.
    renderPasses_.push_back(std::make_unique<ShadowCSMPass>());
    renderPasses_.push_back(std::make_unique<GBufferPass>());
    renderPasses_.push_back(std::make_unique<LightPass>());
    renderPasses_.push_back(std::make_unique<SelectionMaskPass>());
    renderPasses_.push_back(std::make_unique<PresentPass>());
    for (size_t i = 0; i < renderPasses_.size(); ++i)
        rendering_detail::InPass(PassNames[i], [&] { renderPasses_[i]->Initialize(manager, renderingContext_, resources_); });
}

void RenderingSystem::RenderFrame(const FrameContext& frame, const OverlayCallback& overlay) {
    if (!ready_) return;
    try {
        auto* device = renderingContext_.GetDevice();
        rendering_detail::Check(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason", device);
        renderingContext_.ClearState();
        resources_.debugView = debugView_;
        resources_.BindCBuffers(renderingContext_);
        resources_.frameCBuffer.Update(renderingContext_, [&](FrameBufferData& data) {
            data.simulationTimeSeconds = static_cast<float>(frame.simulationTimeSeconds);
        });
        renderingContext_.BindSamplers();
        for (size_t i = 0; i < renderPasses_.size(); ++i) {
            rendering_detail::InPass(PassNames[i], [&] { renderPasses_[i]->Render(world_, renderingContext_, resources_); });
            if (i + 1 == renderPasses_.size() && overlay) overlay(renderingContext_, frame);
            renderingContext_.ClearHazardousState();
        }
        renderingContext_.Present();
    } catch (const RenderingException& error) {
        RecordFailure(error.GetFailure());
        renderingContext_.ClearState();
    }
}

void RenderingSystem::Shutdown() noexcept {
    ready_ = false;
    renderingContext_.ClearState();
    for (auto it = renderPasses_.rbegin(); it != renderPasses_.rend(); ++it) {
        try { (*it)->Destroy(renderingContext_.GetResourceManager(), renderingContext_, resources_); }
        catch (...) { /* Cleanup cannot replace the initiating failure. */ }
    }
    renderPasses_.clear();
    try { resources_.Destroy(renderingContext_.GetResourceManager()); } catch (...) {}
    world_.sceneItems.clear();
    world_.renderItems.clear();
    world_.selectedSceneItems.clear();
    world_.lights.clear();
    world_.debugDraw.Clear();
    renderingContext_.Shutdown();
    initializedSurface_ = {};
    if (diagnostics_.lifecycle != RenderingLifecycle::Failed) diagnostics_ = {};
}

RenderingDiagnostics RenderingSystem::GetDiagnostics() const { return diagnostics_; }
RenderDebugView RenderingSystem::GetDebugView() const noexcept { return debugView_; }
bool RenderingSystem::SetDebugView(RenderDebugView view) noexcept {
    if (!IsValidRenderDebugView(view)) return false;
    debugView_ = view;
    return true;
}
RenderingSystem::~RenderingSystem() { Shutdown(); }
RenderingContext& RenderingSystem::GetRenderingContext() { return renderingContext_; }
RenderingResourceManager& RenderingSystem::GetResourceManager() { return renderingContext_.GetResourceManager(); }
RenderWorld& RenderingSystem::GetWorld() { return world_; }
} // namespace engine
