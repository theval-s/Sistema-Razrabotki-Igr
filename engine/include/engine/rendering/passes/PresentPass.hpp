#pragma once

#include <engine/engine_export.hpp>

#include <engine/rendering/passes/RenderPass.hpp>

namespace engine {

struct ENGINE_API PresentPass : RenderPass {
    void Initialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                    RenderingResources& resources) override;
    void Destroy(RenderingResourceManager& resourceManager, RenderingContext& context,
        RenderingResources& resources) override;

    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override;

    bool PreReinitialize(RenderingResourceManager& resourceManager, RenderingContext& context,
        RenderingResources& resources) override;
    bool Reinitialize(RenderingResourceManager& resourceManager, RenderingContext& context,
        RenderingResources& resources) override;

private:
    ShaderHandle vertexShader_;
    ShaderHandle pixelShader_;
    ShaderHandle outlinedPixelShader_;
    ShaderHandle linesVertexShader_;
    ShaderHandle linesPixelShader_;
    TextureHandle backBufferView_;
};

}
