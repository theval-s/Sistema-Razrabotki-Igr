#pragma once
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "GBufferPass.hpp"
#include "LightPass.hpp"
#include "PresentPass.hpp"
#include "RenderingContext.hpp"
#include "RenderPass.hpp"
#include "ShadowPass.h"


namespace engine {

using Microsoft::WRL::ComPtr;

struct RenderingSystem {
    static RenderingSystem instance;
    RenderingContext renderingContext{};
    RenderingResourceManager& resourceManager = renderingContext.resourceManager_;
    RenderWorld world{};
    std::unique_ptr<RenderingResources> resources;
    
    std::vector<std::unique_ptr<RenderPass>> renderPasses{};
    
    void Initialize(uint32_t screenWidth, uint32_t screenHeight) {
        renderingContext.Initialize();
        resources = std::make_unique<RenderingResources>(screenWidth, screenHeight, resourceManager);
        renderPasses.push_back(std::make_unique<ShadowCSMPass>());
        renderPasses.push_back(std::make_unique<GBufferPass>());
        renderPasses.push_back(std::make_unique<LightPass>());
        renderPasses.push_back(std::make_unique<PresentPass>());
        
        for (const auto& renderPass : renderPasses) {
            renderPass->Initialize(resourceManager, renderingContext, *resources);
        }
    }
    
    
    void RenderFrame() {
        renderingContext.ClearState();
        resources->BindCBuffers(renderingContext);
        renderingContext.BindSamplers();
        
        for (const auto& renderPass : renderPasses) {
            renderPass->Render(world, renderingContext, *resources);
            renderingContext.ClearHazardousState();
        }
    }

    ~RenderingSystem() {
    }
};

}
