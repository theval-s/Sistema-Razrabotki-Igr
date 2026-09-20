#pragma once
#include "Buffer.hpp"
#include "VertexLayout.hpp"


namespace engine {

struct RenderedMesh {
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    MeshLayoutHandle meshLayout;
    uint32_t vertexCount;
};

using MeshHandle = HandleMap<RenderedMesh>::Handle;
}

