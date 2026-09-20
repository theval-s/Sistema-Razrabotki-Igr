#pragma once
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

#include "HandleMap.hpp"

namespace engine {
using Microsoft::WRL::ComPtr;

enum class BufferType
{
    Vertex,
    Index,
    Constant,
    Structured
};

enum class BufferUsage
{
    Immutable, 
    Default,
    Dynamic 
};

struct BufferDesc
{
    BufferType type;
    BufferUsage usage = BufferUsage::Default;

    uint32_t size = 0;
    uint32_t stride = 0;

    bool shaderResource = false;
};

struct GpuBuffer
{
    uint32_t size = 0;
    uint32_t stride = 0;
    ComPtr<ID3D11Buffer> buffer;
};

using BufferHandle = HandleMap<GpuBuffer>::Handle;
}


