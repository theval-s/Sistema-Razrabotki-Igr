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
    void RenderTo(RenderWorld& world, RenderingContext& context, RenderingResources& resources,
                  TextureHandle sceneColor, TextureHandle target, bool builtinTonemappingEnabled = true);
    [[nodiscard]] TextureHandle GetBackbuffer() const noexcept { return backBufferView_; }

    bool PreReinitialize(RenderingResourceManager& resourceManager, RenderingContext& context,
        RenderingResources& resources) override;
    bool Reinitialize(RenderingResourceManager& resourceManager, RenderingContext& context,
        RenderingResources& resources) override;

private:
    // Matches PresentParameters at b5 in PresentPass.hlsl (one 16-byte register).
    struct PresentBufferData {
        uint32_t debugView = 0;
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        uint32_t builtinTonemappingEnabled = 1;
    };
    static_assert(sizeof(PresentBufferData) == 16);
    BufferHandle presentBuffer_;
    ShaderHandle vertexShader_;
    ShaderHandle pixelShader_;
    ShaderHandle outlinedPixelShader_;
    ShaderHandle linesVertexShader_;
    ShaderHandle screenLinesVertexShader_;
    ShaderHandle linesGeometryShader_;
    ShaderHandle linesPixelShader_;
    TextureHandle backBufferView_;
};

}
