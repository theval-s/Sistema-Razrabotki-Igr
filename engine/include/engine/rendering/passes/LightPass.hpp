#pragma once

#include <engine/engine_export.hpp>

#include <engine/rendering/passes/RenderPass.hpp>

namespace engine {

struct ENGINE_API LightPass : RenderPass {
    void Initialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                    RenderingResources& resources) override;
    void Destroy(RenderingResourceManager& resourceManager, RenderingContext& context,
                 RenderingResources& resources) override;
    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override;

private:
    ShaderHandle vertexShader_;
    ShaderHandle pixelShader_;
    ShaderHandle dirLightVertexShader_;
    MeshHandle lightSphere_;
    MeshHandle spotLightBB_;
};

}
