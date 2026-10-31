#include <engine/rendering/RenderingSystem.hpp>
#include <spdlog/spdlog.h>

namespace engine {

bool RenderingSystem::Initialize(const NativeSurface& surface) {
    if (!renderingContext_.Initialize(surface)) {
        return false;
    }
    InitializePasses(surface.width, surface.height);
    ready_ = true;
    return true;
}

bool RenderingSystem::Reinitialize(const NativeSurface& surface) {
    if (!surface.IsValid()) {
        return false;
    }
    ready_ = false;
    // D3D11 bindings hold references even after the pass releases its back-buffer view.
    renderingContext_.ClearState();
    auto& resourceManager = renderingContext_.GetResourceManager();
    for (const auto& renderPass : renderPasses_) {
        if (!renderPass->PreReinitialize(resourceManager, renderingContext_, resources_)) {
            spdlog::error("Rendering pass failed to release resources before resize");
            return false;
        }
    }
    // ClearState drops bindings; Flush ensures their deferred references are released too.
    renderingContext_.GetDeviceContext()->Flush();
    if (!renderingContext_.Reinitialize(surface)) {
        // ResizeBuffers leaves the old swap chain intact on failure. Restore its view so
        // the next resize attempt can release it again and retry cleanly.
        for (const auto& renderPass : renderPasses_) {
            if (!renderPass->Reinitialize(resourceManager, renderingContext_, resources_)) {
                spdlog::error("Failed to restore pass resources after resize failure");
            }
        }
        return false;
    }
    resources_.screenWith = surface.width;
    resources_.screenHeight = surface.height;
    for (const auto& renderPass : renderPasses_) {
        if (!renderPass->Reinitialize(resourceManager, renderingContext_, resources_)) {
            spdlog::error("Rendering pass failed to recreate resources after resize");
            return false;
        }
    }
    ready_ = true;
    return true;
}

void RenderingSystem::InitializePasses(const uint32_t screenWidth, const uint32_t screenHeight) {
    auto& resourceManager = renderingContext_.GetResourceManager();
    resources_ = RenderingResources(screenWidth, screenHeight);
    resources_.Initialize(resourceManager);
    
    renderPasses_.push_back(std::make_unique<ShadowCSMPass>());
    renderPasses_.push_back(std::make_unique<GBufferPass>());
    renderPasses_.push_back(std::make_unique<LightPass>());
    renderPasses_.push_back(std::make_unique<SelectionMaskPass>());
    renderPasses_.push_back(std::make_unique<PresentPass>());

    for (const auto& renderPass : renderPasses_) {
        renderPass->Initialize(resourceManager, renderingContext_, resources_);
    }
}

void RenderingSystem::RenderFrame(const FrameContext& frame) {
    if (!ready_) {
        return;
    }
    renderingContext_.ClearState();
    resources_.BindCBuffers(renderingContext_);
    resources_.frameCBuffer.Update(renderingContext_, [&](FrameBufferData& data) {
        data.simulationTimeSeconds = static_cast<float>(frame.simulationTimeSeconds);
    });
    renderingContext_.BindSamplers();

    for (const auto& renderPass : renderPasses_) {
        renderPass->Render(world_, renderingContext_, resources_);
        renderingContext_.ClearHazardousState();
    }
}

RenderingSystem::~RenderingSystem() {
    renderingContext_.ClearState();
    for (const auto& renderPass : renderPasses_) {
        renderPass->Destroy(renderingContext_.GetResourceManager(), renderingContext_, resources_);
    }
    resources_.Destroy(renderingContext_.GetResourceManager());
}


RenderingContext& RenderingSystem::GetRenderingContext() {
    return renderingContext_;
}

RenderingResourceManager& RenderingSystem::GetResourceManager() {
    return renderingContext_.GetResourceManager();
}

RenderWorld& RenderingSystem::GetWorld() {
    return world_;
}

}
