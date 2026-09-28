#include "engine/RenderingResources.h"

namespace engine {

RenderingResources::RenderingResources(const uint32_t aScreenWith, const uint32_t aScreenHeight,
                                       RenderingResourceManager& resourceManager) : screenWith(aScreenWith),
                                                                                    screenHeight(aScreenHeight),
                                                                                    frameCBuffer(resourceManager),
                                                                                    viewCBuffer(resourceManager),
                                                                                    materialCBuffer(resourceManager),
                                                                                    objectCBuffer(resourceManager),
                                                                                    shadowCascadesCBuffer(resourceManager),
                                                                                    lightCBuffer(resourceManager) {
}

void RenderingResources::BindCBuffers(RenderingContext& context) {
    //context.BindCBuffer(BindSlot::CBuffer::Frame, frameCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::View, viewCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Material, materialCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Object, objectCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::ShadowCascades, shadowCascadesCBuffer.GetHandle());
    context.BindCBuffer(BindSlots::CBuffer::Pass0, lightCBuffer.GetHandle());
}

}
