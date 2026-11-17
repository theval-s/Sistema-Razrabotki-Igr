#pragma once

#include <engine/engine_export.hpp>

#include <engine/rendering/RenderingResources.hpp>
#include <engine/rendering/RenderWorld.hpp>


namespace engine {

struct ENGINE_API RenderPass {
    /// Initialize/Render/resize resource failures throw RenderingException. The system
    /// adds the dispatch pass name if missing and catches at its lifecycle boundary.
    /// Resize bools remain for source compatibility: built-in passes return true or throw;
    /// a legacy false is converted by the system to an operation/pass-specific failure.
    /// Destroy must not throw and must tolerate partial initialization and repeated calls.
    
    virtual ~RenderPass() = default;
    
    /// As almost all rendering resources are created with RenderingResourceManager, they cannot be deallocated without it, hence we have this "second destructor"
    virtual void Destroy(RenderingResourceManager & resourceManager, RenderingContext & context, RenderingResources & resources) = 0;
    
    virtual void Render(RenderWorld & world, RenderingContext & context, RenderingResources & resources) = 0;
    
    /// Called on first initialization
    virtual void Initialize(RenderingResourceManager & resourceManager, RenderingContext & context, RenderingResources & resources) = 0;
    
    ///Called before backbuffer is resized on re-init, re-init will fail if some pointers to backbuffer are alive, so this ugly thing is necessary
    virtual bool PreReinitialize(RenderingResourceManager & resourceManager, RenderingContext & context, RenderingResources & resources) {
        return true;
    }
    
    ///Called when window is resized to adjust size dependant resources
    virtual bool Reinitialize(RenderingResourceManager & resourceManager, RenderingContext & context, RenderingResources & resources) {
        return true;
    }
};


}
