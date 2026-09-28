#pragma once
#include <cstdint>
#include <d3d11.h>
#include <dxgiformat.h>
#include <engine/engine_export.hpp>
#include <wrl/client.h>
#include <winnt.h>

#include "HandleMap.hpp"

namespace engine {
using Microsoft::WRL::ComPtr;

enum class TextureUsage : uint32_t {
    None = 0,
    ShaderResource = 1 << 0,
    RenderTarget = 1 << 1,
    DepthStencil = 1 << 2,
    UnorderedAccess = 1 << 3,
};

DEFINE_ENUM_FLAG_OPERATORS(TextureUsage);

ENGINE_API bool HasFlag(TextureUsage usage, TextureUsage flag);

struct ENGINE_API TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;

    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureUsage usage = TextureUsage::ShaderResource;

    // Only needed when the view format differs from the resource format.
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;

    D3D11_USAGE memoryUsage = D3D11_USAGE_DEFAULT;
    uint32_t cpuAccessFlags = 0;
    uint32_t miscFlags = 0;

    uint32_t sampleCount = 1;
    uint32_t sampleQuality = 0;

    [[nodiscard]] uint32_t GetBindFlags() const;
};

struct RenderingResourceManager;

struct ENGINE_API Texture {
    explicit Texture();
    explicit Texture(ComPtr<ID3D11ShaderResourceView>&& view);
    explicit Texture(ComPtr<ID3D11RenderTargetView>&& view);

    [[nodiscard]] const TextureDesc& GetDesc() const;
    [[nodiscard]] ID3D11Texture2D* GetTexture() const;
    [[nodiscard]] ID3D11ShaderResourceView* GetSRV() const;
    [[nodiscard]] ID3D11RenderTargetView* GetRTV() const;
    [[nodiscard]] ID3D11DepthStencilView* GetDSV() const;
    [[nodiscard]] ID3D11DepthStencilView* GetReadOnlyDSV() const;
    [[nodiscard]] ID3D11UnorderedAccessView* GetUAV() const;

private:
    HRESULT CreateDefaultSRV(ID3D11Device* device);
    HRESULT CreateDefaultRTV(ID3D11Device* device);
    HRESULT CreateDefaultDSV(ID3D11Device* device);
    HRESULT CreateDefaultUAV(ID3D11Device* device);

    TextureDesc desc_;
    ComPtr<ID3D11Texture2D> resource_;
    // Default views.
    // todo: We could optimize out some space as 90% of textures would only use srv_
    // todo: We also need a way to handle arbitrary views like RTV into Mip 2 or smth
    ComPtr<ID3D11ShaderResourceView> srv_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11DepthStencilView> readOnlyDsv_;
    ComPtr<ID3D11UnorderedAccessView> uav_;

    friend RenderingResourceManager;
};

using TextureHandle = HandleMap<Texture>::Handle;
}
