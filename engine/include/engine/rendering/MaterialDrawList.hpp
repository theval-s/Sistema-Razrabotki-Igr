#pragma once
#include <algorithm>
#include <tuple>
#include <vector>

#include <engine/rendering/RenderWorld.hpp>

namespace engine {

struct MaterialDraw {
    const RenderItem* item;
    const MaterialInstance* instance;
    const Material* material;
    ShaderHandle vertexShader;
    ShaderHandle pixelShader;
};

// A separate ordering is built for each pass; the submitted world stays untouched.
inline std::vector<MaterialDraw> BuildMaterialDrawList(const RenderWorld& world,
    const RenderingResourceManager& manager, bool shadow, ShaderHandle defaultVS,
    ShaderHandle defaultPS = {}) {
    std::vector<MaterialDraw> draws;
    draws.reserve(world.renderItems.size() + world.sceneItems.size());
    const auto append = [&](const RenderItem& item) {
        const auto* instance = manager.TryGetMaterialInstance(item.materialInstance);
        const auto* material = instance ? manager.TryGetMaterial(instance->material) : nullptr;
        if (!material) return;
        const auto vs = shadow ? material->shaders.shadowPassVS : material->shaders.gBufferPassVS;
        const auto ps = shadow ? ShaderHandle{} : material->shaders.gBufferPassPS;
        draws.push_back({&item, instance, material, vs ? vs : defaultVS, ps ? ps : defaultPS});
    };
    for (const auto& item : world.renderItems) append(item);
    for (const auto& item : world.sceneItems) append(item);
    const auto key = [](const MaterialDraw& draw) {
        const auto& instance = draw.item->materialInstance;
        const auto& mesh = draw.item->mesh;
        return std::tuple{draw.vertexShader.id, draw.vertexShader.generation,
            draw.pixelShader.id, draw.pixelShader.generation,
            instance.id, instance.generation, mesh.id, mesh.generation};
    };
    std::sort(draws.begin(), draws.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    return draws;
}

} // namespace engine
