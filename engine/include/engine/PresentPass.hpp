#pragma once

#include <engine/engine_export.hpp>

#include "RenderPass.hpp"

namespace engine {

struct ENGINE_API PresentPass : RenderPass {
    void Initialize(RenderingResourceManager& resourceManager, RenderingContext& context,
                    RenderingResources& resources) override;
    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override;

private:
    ShaderHandle vertexShader_;
    ShaderHandle pixelShader_;
    TextureHandle backBufferView_;
};

}
