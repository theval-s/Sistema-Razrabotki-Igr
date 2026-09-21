#include "engine/Texture.hpp"

#include <cassert>
#include <utility>

namespace engine {

bool HasFlag(const TextureUsage usage, const TextureUsage flag) {
    return (usage & flag) != TextureUsage::None;
}

uint32_t TextureDesc::GetBindFlags() const {
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

Texture::Texture() = default;

Texture::Texture(ComPtr<ID3D11ShaderResourceView>&& view) : srv_(std::move(view)) {
}

Texture::Texture(ComPtr<ID3D11RenderTargetView>&& view) : rtv_(std::move(view)) {
}

const TextureDesc& Texture::GetDesc() const {
    return desc_;
}

ID3D11Texture2D* Texture::GetTexture() const {
    return resource_.Get();
}

ID3D11ShaderResourceView* Texture::GetSRV() const {
    assert(srv_);
    return srv_.Get();
}

ID3D11RenderTargetView* Texture::GetRTV() const {
    assert(rtv_);
    return rtv_.Get();
}

ID3D11DepthStencilView* Texture::GetDSV() const {
    assert(dsv_);
    return dsv_.Get();
}

ID3D11DepthStencilView* Texture::GetReadOnlyDSV() const {
    assert(readOnlyDsv_);
    return readOnlyDsv_.Get();
}

ID3D11UnorderedAccessView* Texture::GetUAV() const {
    assert(uav_);
    return uav_.Get();
}

HRESULT Texture::CreateDefaultSRV(ID3D11Device* device) {
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

HRESULT Texture::CreateDefaultRTV(ID3D11Device* device) {
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

HRESULT Texture::CreateDefaultDSV(ID3D11Device* device) {
    const DXGI_FORMAT format = desc_.dsvFormat;
    if (format == DXGI_FORMAT_UNKNOWN) {
        // read only depth is not constructed in default case
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

HRESULT Texture::CreateDefaultUAV(ID3D11Device* device) {
    const DXGI_FORMAT format = desc_.uavFormat;
    if (format == DXGI_FORMAT_UNKNOWN) {
        return device->CreateUnorderedAccessView(
            resource_.Get(),
            nullptr,
            uav_.GetAddressOf());
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC dxDesc{};
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

}
