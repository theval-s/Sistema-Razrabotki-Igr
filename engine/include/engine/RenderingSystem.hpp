#pragma once
#include <wrl/client.h>

#include "GBufferPass.hpp"
#include "LightPass.hpp"
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
    RenderingResources resources{};
    
    std::vector<std::unique_ptr<RenderPass>> renderPasses{};
    
    void Initialize() {
        renderingContext.Initialize();
        resources.Initialize(resourceManager);
        renderPasses.push_back(std::make_unique<ShadowCSMPass>());
        renderPasses.push_back(std::make_unique<GBufferPass>());
        renderPasses.push_back(std::make_unique<LightPass>());
        
        for (const auto& renderPass : renderPasses) {
            renderPass->Initialize(resourceManager, resources);
        }
    }
    
    
    void RenderFrame() {
        renderingContext.ClearState();
        resources.BindCBuffers(renderingContext);
        renderingContext.BindSamplers();
        
        for (const auto& renderPass : renderPasses) {
            renderPass->Render(world, renderingContext, resources);
            renderingContext.ClearHazardousState();
        }
    }

    ~RenderingSystem() {
    }
};

}
