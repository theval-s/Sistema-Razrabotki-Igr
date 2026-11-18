#include <engine/rendering/RenderingResources.hpp>
#include <algorithm>

namespace engine {

RenderingResources::RenderingResources(const uint32_t aScreenWith, const uint32_t aScreenHeight) : screenWith(aScreenWith),
    screenHeight(aScreenHeight) {
}

void RenderingResources::Initialize(RenderingResourceManager& resourceManager) try {
    frameCBuffer.Initialize(resourceManager);
    viewCBuffer.Initialize(resourceManager);
    materialCBuffer.Initialize(resourceManager);
    objectCBuffer.Initialize(resourceManager);
    shadowCascadesCBuffer.Initialize(resourceManager);
    lightCBuffer.Initialize(resourceManager);
    skinningCBuffer.Initialize(resourceManager);

    constexpr uint32_t white = 0xffffffffu;
    D3D11_SUBRESOURCE_DATA data{.pSysMem = &white, .SysMemPitch = sizeof(white)};
    defaultAlbedoTexture_ = resourceManager.CreateTexture({
                                                              .width = 1, .height = 1,
                                                              .format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                                              .usage = TextureUsage::ShaderResource
                                                          },
                                                          std::span{&data, 1});
} catch (...) {
    Destroy(resourceManager);
    throw;
}

void RenderingResources::Destroy(RenderingResourceManager& resourceManager) {
    particleShadowView = nullptr;
    frameCBuffer.Destroy(resourceManager);
    viewCBuffer.Destroy(resourceManager);
    materialCBuffer.Destroy(resourceManager);
    objectCBuffer.Destroy(resourceManager);
    shadowCascadesCBuffer.Destroy(resourceManager);
    lightCBuffer.Destroy(resourceManager);
    skinningCBuffer.Destroy(resourceManager);
    resourceManager.DestroyTexture(defaultAlbedoTexture_);
    defaultAlbedoTexture_ = {};
}

void RenderingResources::BindCBuffers(RenderingContext& context) {
    context.BindCBuffer(BindSlots::CBuffer::Frame, frameCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::View, viewCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Material, materialCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Object, objectCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::ShadowCascades, shadowCascadesCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Pass0, lightCBuffer.GetHandle());
}

void RenderingResources::BindSkinning(RenderingContext& context, const SkinningPalette* palette) {
    if (!palette) {
        context.UnbindCBuffer(BindSlots::CBuffer::Skinning);
        return;
    }
    skinningCBuffer.Update(context, [&](SkinningBufferData& data) {
        // Never read beyond the submitted palette or leave data from a previous draw.
        data.matrices.fill(Matrix::Identity);
        std::copy_n(palette->matrices.begin(), std::min(palette->matrices.size(), MaxSkinningJoints),
                    data.matrices.begin());
    });
    context.BindCBuffer(BindSlots::CBuffer::Skinning, skinningCBuffer.GetHandle());
}

}
