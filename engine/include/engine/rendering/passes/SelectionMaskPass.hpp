#pragma once

#include <engine/rendering/passes/RenderPass.hpp>

namespace engine {

class SelectionMaskPass final : public RenderPass {
public:
    void Initialize(RenderingResourceManager& manager, RenderingContext&, RenderingResources& resources) override;
    void Destroy(RenderingResourceManager& manager, RenderingContext&, RenderingResources& resources) override;
    void Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) override;
    bool Reinitialize(RenderingResourceManager& manager, RenderingContext&, RenderingResources& resources) override;

private:
    ShaderHandle defaultVS_{};
    ShaderHandle visiblePS_{};
    ShaderHandle occludedPS_{};
};

} // namespace engine
