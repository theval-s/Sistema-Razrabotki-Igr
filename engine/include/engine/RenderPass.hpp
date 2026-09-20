#pragma once

#include "RenderingResources.h"
#include "RenderWorld.hpp"


namespace engine {

struct RenderPass {
    
    virtual ~RenderPass() = default;

    virtual void Render(RenderWorld & world, RenderingContext & context, RenderingResources & resources) = 0;
    
    virtual void Initialize(RenderingResourceManager & resourceManager, RenderingResources & resources) = 0;
};


}
