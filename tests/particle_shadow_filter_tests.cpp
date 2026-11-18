#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT MapSize = 512, Width = 65;

void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + ": " + std::to_string(hr));
}

// Render the production PSMain, including world-position reconstruction, cascade
// selection, opaque PCF and lighting. No copy of its sampling function is used.
class ParticleShadowFilter : public testing::TestWithParam<D3D_DRIVER_TYPE> {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    std::array<ComPtr<ID3D11ShaderResourceView>, 7> views; // LightPass t0..t6; t5 unused
    std::array<ComPtr<ID3D11SamplerState>, 5> samplers;

    ComPtr<ID3D11ShaderResourceView> Texture(UINT size, UINT layers, DXGI_FORMAT format,
                                          const void* pixels, UINT pitch, bool array = false, UINT slicePitch = 0) {
        D3D11_TEXTURE2D_DESC t{};
        t.Width = t.Height = size;
        t.ArraySize = layers;
        t.MipLevels = t.SampleDesc.Count = 1;
        t.Format = format;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> data(layers, {pixels, pitch, 0});
        for (UINT layer = 0; layer < layers; ++layer)
            data[layer].pSysMem = static_cast<const std::byte*>(pixels) + layer * slicePitch;
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&t, data.data(), &texture), "texture");
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = format;
        s.ViewDimension = array ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (array) { s.Texture2DArray.MipLevels = 1; s.Texture2DArray.ArraySize = layers; }
        else s.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        Check(device->CreateShaderResourceView(texture.Get(), &s, &view), "texture view");
        return view;
    }

    template<size_t N> void Constants(UINT slot, const std::array<float, N>& values) {
        static_assert(N % 4 == 0);
        D3D11_BUFFER_DESC b{};
        b.ByteWidth = UINT(sizeof(values));
        b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&b, &data, &buffer), "constants");
        ctx->PSSetConstantBuffers(slot, 1, buffer.GetAddressOf());
    }

    void SetUp() override {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, GetParam(), nullptr, 0, &level, 1,
                                    D3D11_SDK_VERSION, &device, nullptr, &ctx)))
            GTEST_SKIP() << "D3D11 FL11 device unavailable";

        const auto path = std::filesystem::path(SRI_SOURCE_ROOT) / "engine/shaders/LightPass.hlsl";
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                             "PSMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        ASSERT_TRUE(SUCCEEDED(hr)) << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
        Check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), "PS");
        constexpr char vertex[] =
            "float4 main(uint id:SV_VertexID):SV_POSITION {"
            "float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }";
        code.Reset();
        Check(D3DCompile(vertex, sizeof(vertex)-1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr), "compile VS");
        Check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs), "VS");

        const float white[4]{1,1,1,1}, black[4]{};
        const float depth = .5f;
        views[0] = Texture(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, white, 16);
        const std::vector<std::array<float, 4>> normals(Width * Width, {.5f,.5f,1,0});
        views[1] = Texture(Width, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, normals.data(), Width * 16);
        views[2] = Texture(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, black, 16);
        const std::vector<float> depths(Width * Width, depth);
        views[3] = Texture(Width, 1, DXGI_FORMAT_R32_FLOAT, depths.data(), Width * 4);
        const std::vector<float> opaque(1024 * 1024, 0.f);
        views[4] = Texture(1024, 4, DXGI_FORMAT_R32_FLOAT, opaque.data(), 1024 * 4, true);

        // Common.hlsl slots, matching RenderStates' linear-clamp and reverse-Z PCF.
        for (UINT i = 0; i < samplers.size(); ++i) {
            D3D11_SAMPLER_DESC s{};
            s.Filter = i == 4 ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW = i == 2 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
            s.ComparisonFunc = D3D11_COMPARISON_GREATER_EQUAL;
            s.MaxLOD = D3D11_FLOAT32_MAX;
            Check(device->CreateSamplerState(&s, &samplers[i]), "sampler");
        }
        D3D11_TEXTURE2D_DESC t{};
        t.Width = t.Height = Width;
        t.MipLevels = t.ArraySize = t.SampleDesc.Count = 1;
        t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&t, nullptr, &target), "target");
        Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "RTV");
        t.BindFlags = 0; t.Usage = D3D11_USAGE_STAGING; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&t, nullptr, &staging), "staging");
    }

    void ShadowPattern() {
        std::vector<uint16_t> pixels(4 * MapSize * MapSize);
        for (UINT layer = 0; layer < 4; ++layer) {
            for (UINT y = 0; y < MapSize; ++y) for (UINT x = 0; x < MapSize; ++x) {
                const float value = .125f + layer / 32.f + (x >= 256 ? .25f : 0) + (y >= 256 ? .375f : 0);
                // All values are exact positive normal half floats.
                pixels[(layer * MapSize + y) * MapSize + x] = uint16_t((std::bit_cast<uint32_t>(value) >> 13) - 0x1c000);
            }
        }
        views[6] = Texture(MapSize, 4, DXGI_FORMAT_R16_FLOAT, pixels.data(), MapSize * 2, true, MapSize * MapSize * 2);
    }

    std::vector<float> Render(UINT cascade, bool edge = false) {
        // Common.hlsl ABI, expressed as float registers to keep this test independent
        // of engine binaries. Identity cascades project a tiny planar receiver.
        std::array<float, 56> view{};
        for (UINT base : {0u, 16u, 32u}) for (UINT i : {0u,5u,10u,15u}) view[base+i] = 1;
        view[32] = view[37] = 1.f / MapSize;
        if (edge) { view[44] = -1; view[45] = 1; }
        view[52] = view[53] = float(Width);
        Constants(1, view);
        std::array<float, 76> cascades{};
        for (UINT c = 0; c < 4; ++c) {
            for (UINT i : {0u,5u,10u,15u}) cascades[c*16+i] = 1;
            cascades[64+c] = c < cascade ? .25f : 10.f;
            cascades[68+c] = 10.f;
        }
        Constants(4, cascades);
        std::array<float, 16> light{};
        light[0] = light[1] = light[2] = light[3] = 1;
        light[14] = -1;
        Constants(5, light);
        std::array<ID3D11ShaderResourceView*, 7> resources{};
        for (size_t i = 0; i < views.size(); ++i) resources[i] = views[i].Get();
        ctx->PSSetShaderResources(0, UINT(resources.size()), resources.data());
        std::array<ID3D11SamplerState*, 5> states{};
        for (size_t i = 0; i < states.size(); ++i) states[i] = samplers[i].Get();
        ctx->PSSetSamplers(0, UINT(states.size()), states.data());
        ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_VIEWPORT viewport{0,0,float(Width),float(Width),0,1};
        ctx->RSSetViewports(1, &viewport);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->Draw(3, 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
        std::vector<float> result(Width * Width);
        for (UINT y = 0; y < Width; ++y) {
            const auto row = reinterpret_cast<const float*>(static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < Width; ++x) result[y*Width+x] = row[x*4] - .1f; // remove ambient
        }
        ctx->Unmap(staging.Get(), 0);
        return result;
    }
};

TEST_P(ParticleShadowFilter, FractionalReceiverIsSmoothAcrossBothAxesInEveryCascade) {
    ShadowPattern();
    for (UINT c = 0; c < 4; ++c) {
        const auto image = Render(c);
        for (UINT y = 0; y < Width; ++y) for (UINT x = 0; x < Width; ++x) {
            const float expected = .125f + c/32.f + .25f*(x+.5f)/Width + .375f*(y+.5f)/Width;
            ASSERT_NEAR(image[y*Width+x], expected, .003f) << "cascade " << c << " pixel " << x << "," << y;
            if (x) ASSERT_GT(image[y*Width+x], image[y*Width+x-1]);
            if (y) ASSERT_GT(image[y*Width+x], image[(y-1)*Width+x]);
        }
    }
}

TEST_P(ParticleShadowFilter, ClampDoesNotPullOppositeEdgeAndOutsideRemainsUnshadowed) {
    ShadowPattern();
    const auto image = Render(0, true);
    for (UINT y = 0; y < Width; ++y) for (UINT x = 0; x < Width; ++x) {
        const bool inside = x >= Width/2 && y >= Width/2;
        ASSERT_NEAR(image[y*Width+x], inside ? .125f : 1.f, .001f) << x << "," << y;
    }
}

TEST_P(ParticleShadowFilter, MissingParticleMapLeavesOpaqueLightingUnattenuated) {
    for (UINT c = 0; c < 4; ++c) {
        const auto image = Render(c);
        for (float transmission : image) ASSERT_NEAR(transmission, 1.f, .0001f) << "cascade " << c;
    }
}

INSTANTIATE_TEST_SUITE_P(Native, ParticleShadowFilter,
                        testing::Values(D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE),
                        [](const testing::TestParamInfo<D3D_DRIVER_TYPE>& info) {
                            return info.param == D3D_DRIVER_TYPE_WARP ? "WARP" : "Hardware";
                        });
} // namespace
