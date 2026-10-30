#include <engine/rendering/RenderingResources.hpp>

namespace engine {

RenderingResources::RenderingResources(const uint32_t aScreenWith, const uint32_t aScreenHeight) : screenWith(aScreenWith),
    screenHeight(aScreenHeight) {
}

void RenderingResources::Initialize(RenderingResourceManager& resourceManager) {
    frameCBuffer.Initialize(resourceManager);
    viewCBuffer.Initialize(resourceManager);
    materialCBuffer.Initialize(resourceManager);
    objectCBuffer.Initialize(resourceManager);
    shadowCascadesCBuffer.Initialize(resourceManager);
    lightCBuffer.Initialize(resourceManager);

    constexpr uint32_t white = 0xffffffffu;
    D3D11_SUBRESOURCE_DATA data{.pSysMem = &white, .SysMemPitch = sizeof(white)};
    defaultAlbedoTexture_ = resourceManager.CreateTexture({
                                                              .width = 1, .height = 1,
                                                              .format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                                              .usage = TextureUsage::ShaderResource
                                                          },
                                                          std::span{&data, 1});
}

void RenderingResources::Destroy(RenderingResourceManager& resourceManager) {
    frameCBuffer.Destroy(resourceManager);
    viewCBuffer.Destroy(resourceManager);
    materialCBuffer.Destroy(resourceManager);
    objectCBuffer.Destroy(resourceManager);
    shadowCascadesCBuffer.Destroy(resourceManager);
    lightCBuffer.Destroy(resourceManager);
}

void RenderingResources::BindCBuffers(RenderingContext& context) {
    context.BindCBuffer(BindSlots::CBuffer::Frame, frameCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::View, viewCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Material, materialCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Object, objectCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::ShadowCascades, shadowCascadesCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Pass0, lightCBuffer.GetHandle());
}

}
