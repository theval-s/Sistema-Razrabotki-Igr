#pragma once
#include <memory>
#include <vector>

#include <engine/engine_export.hpp>

#include "GBufferPass.hpp"
#include "LightPass.hpp"
#include "PresentPass.hpp"
#include "RenderingContext.hpp"
#include "RenderPass.hpp"
#include "ShadowPass.h"


namespace engine {

struct ENGINE_API RenderingSystem {
    static RenderingSystem instance;

    void Initialize(uint32_t screenWidth, uint32_t screenHeight);
    void RenderFrame();

    [[nodiscard]] RenderingContext& GetRenderingContext();
    [[nodiscard]] RenderingResourceManager& GetResourceManager();
    [[nodiscard]] RenderWorld& GetWorld();

private:
    RenderingContext renderingContext_{};
    RenderWorld world_{};
    std::unique_ptr<RenderingResources> resources_;
    std::vector<std::unique_ptr<RenderPass>> renderPasses_{};
};

}
