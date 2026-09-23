#include "engine/RenderingSystem.hpp"

namespace engine {

RenderingSystem RenderingSystem::instance{};

void RenderingSystem::Initialize(const uint32_t screenWidth, const uint32_t screenHeight) {
    renderingContext_.Initialize(screenWidth, screenHeight);
    auto& resourceManager = renderingContext_.GetResourceManager();
    resources_ = std::make_unique<RenderingResources>(screenWidth, screenHeight, resourceManager);
    renderPasses_.push_back(std::make_unique<ShadowCSMPass>());
    renderPasses_.push_back(std::make_unique<GBufferPass>());
    renderPasses_.push_back(std::make_unique<LightPass>());
    renderPasses_.push_back(std::make_unique<PresentPass>());

    for (const auto& renderPass : renderPasses_) {
        renderPass->Initialize(resourceManager, renderingContext_, *resources_);
    }
}

void RenderingSystem::RenderFrame() {
    renderingContext_.ClearState();
    resources_->BindCBuffers(renderingContext_);
    renderingContext_.BindSamplers();

    for (const auto& renderPass : renderPasses_) {
        renderPass->Render(world_, renderingContext_, *resources_);
        renderingContext_.ClearHazardousState();
    }
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
