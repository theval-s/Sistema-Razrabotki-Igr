#pragma once
#include <cstdint>
#include <d3d11.h>
#include <dxgiformat.h>
#include <wrl/client.h>
#include <winnt.h>


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

inline boolean HasFlag(const TextureUsage usage, const TextureUsage flag) {
    return (usage & flag) != TextureUsage::None;
}

struct TextureDesc {
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

    [[nodiscard]] uint32_t GetBindFlags() const {
        uint32_t flags = 0;

        if (HasFlag(usage, TextureUsage::ShaderResource)) {
            flags |= D3D11_BIND_SHADER_RESOURCE;
        }
        if (HasFlag(usage, TextureUsage::RenderTarget)) {
            flags |= D3D11_BIND_RENDER_TARGET;
        }
        if (HasFlag(usage, TextureUsage::DepthStencil)) {
            flags |= D3D11_BIND_DEPTH_STENCIL;
        }
        if (HasFlag(usage, TextureUsage::UnorderedAccess)) {
            flags |= D3D11_BIND_UNORDERED_ACCESS;
        }
        return flags;
    }
};

struct RenderingResourceManager;

struct Texture {

    [[nodiscard]] const TextureDesc & GetDesc() const {
        return desc_;
    }
    [[nodiscard]] ID3D11Texture2D * GetTexture() const {
        return resource_.Get();
    }
    [[nodiscard]] ID3D11ShaderResourceView* GetSRV() const {
        assert(srv_);
        return srv_.Get();
    }
    [[nodiscard]] ID3D11RenderTargetView * GetRTV() const {
        assert(rtv_);
        return rtv_.Get();
    }
    [[nodiscard]] ID3D11DepthStencilView * GetDSV() const {
        assert(dsv_);
        return dsv_.Get();
    }
    [[nodiscard]] ID3D11DepthStencilView * GetReadOnlyDSV() const {
        assert(readOnlyDsv_);
        return readOnlyDsv_.Get();
    }
    [[nodiscard]] ID3D11UnorderedAccessView * GetUAV() const {
        assert(uav_);
        return uav_.Get();
    }
    explicit Texture() = default;
    
    explicit Texture(ComPtr<ID3D11ShaderResourceView> && view) : srv_(std::move(view)) {
        
    }
private:
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
    
    HRESULT CreateDefaultSRV(ID3D11Device* device) {
        const DXGI_FORMAT format = desc_.srvFormat;
        if (format == DXGI_FORMAT_UNKNOWN) {
            return device->CreateShaderResourceView(
                resource_.Get(),
                nullptr,
                srv_.GetAddressOf());
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC dxDesc{};
        dxDesc.Format = format;
        
        if (desc_.arraySize > 1) {
            dxDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            dxDesc.Texture2DArray.MostDetailedMip = 0;
            dxDesc.Texture2DArray.MipLevels = desc_.mipLevels;
            dxDesc.Texture2DArray.FirstArraySlice = 0;
            dxDesc.Texture2DArray.ArraySize = desc_.arraySize;
        } else {
            dxDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            dxDesc.Texture2D.MostDetailedMip = 0;
            dxDesc.Texture2D.MipLevels = desc_.mipLevels;
        }

        return device->CreateShaderResourceView(
            resource_.Get(),
            &dxDesc,
            srv_.GetAddressOf());
    }
    
    HRESULT CreateDefaultRTV(ID3D11Device* device) {
        const DXGI_FORMAT format = desc_.rtvFormat;
        if (format == DXGI_FORMAT_UNKNOWN) {
            return device->CreateRenderTargetView(
                resource_.Get(),
                nullptr,
                rtv_.GetAddressOf());
        }

        D3D11_RENDER_TARGET_VIEW_DESC dxDesc{};
        dxDesc.Format = format;
        
        if (desc_.arraySize > 1) {
            dxDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            dxDesc.Texture2DArray.MipSlice = 0;
            dxDesc.Texture2DArray.FirstArraySlice = 0;
            dxDesc.Texture2DArray.ArraySize = desc_.arraySize;
        } else {
            dxDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            dxDesc.Texture2D.MipSlice = 0;
        }

        return device->CreateRenderTargetView(
            resource_.Get(),
            &dxDesc,
            rtv_.GetAddressOf());
    }
    
    HRESULT CreateDefaultDSV(ID3D11Device* device) {
        const DXGI_FORMAT format = desc_.rtvFormat;
        if (format == DXGI_FORMAT_UNKNOWN) {
            //read only depth is not constructed in default case
            return device->CreateDepthStencilView(
                resource_.Get(),
                nullptr,
                dsv_.GetAddressOf());
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dxDesc{};
        dxDesc.Format = format;
        
        if (desc_.arraySize > 1) {
            dxDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dxDesc.Texture2DArray.MipSlice = 0;
            dxDesc.Texture2DArray.FirstArraySlice = 0;
            dxDesc.Texture2DArray.ArraySize = desc_.arraySize;
        } else {
            dxDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dxDesc.Texture2D.MipSlice = 0;
        }

        const auto r = device->CreateDepthStencilView(
            resource_.Get(),
            &dxDesc,
            dsv_.GetAddressOf());
        if (FAILED(r)) {
            return r;
        }
        
        dxDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        return device->CreateDepthStencilView(
            resource_.Get(),
            &dxDesc,
            readOnlyDsv_.GetAddressOf());
    }
    
    HRESULT CreateDefaultUAV(ID3D11Device* device) {
        const DXGI_FORMAT format = desc_.uavFormat;
        if (format == DXGI_FORMAT_UNKNOWN) {
            return device->CreateUnorderedAccessView(
                resource_.Get(),
                nullptr,
                uav_.GetAddressOf());
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC    dxDesc{};
        dxDesc.Format = format;
        
        if (desc_.arraySize > 1) {
            dxDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            dxDesc.Texture2DArray.MipSlice = 0;
            dxDesc.Texture2DArray.FirstArraySlice = 0;
            dxDesc.Texture2DArray.ArraySize = desc_.arraySize;
        } else {
            dxDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            dxDesc.Texture2D.MipSlice = 0;
        }

        return device->CreateUnorderedAccessView(
            resource_.Get(),
            &dxDesc,
            uav_.GetAddressOf());
    }
};

using TextureHandle = HandleMap<Texture>::Handle;
}
