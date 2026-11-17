#include <engine/rendering/passes/PresentPass.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace {
using namespace engine;
using Pixel = std::array<float, 4>;

std::filesystem::path ShaderPath(const char* name = "PresentPass.hlsl") {
    return std::filesystem::path(SRI_SOURCE_ROOT) / "engine/shaders" / name;
}

void Check(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("D3D11 test fixture operation failed: " + std::to_string(result));
}

// Deliberately execute the checked-in VS/PS on WARP, without going through the
// hardware-only RenderingContext device factory or copying visualization math.
class RenderDebugWarpTest : public ::testing::Test {
protected:
    struct Parameters {
        uint32_t mode;
        float nearPlane;
        float farPlane;
        float padding = 0;
    };
    static_assert(sizeof(Parameters) == 16);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, outlinedPS;
    ComPtr<ID3D11Buffer> parameters;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;

    void SetUp() override {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, &context));
        const auto vertex = ShaderContext::CompileCpu(ShaderPath(), ShaderType::Vertex);
        const auto pixel = ShaderContext::CompileCpu(ShaderPath(), ShaderType::Pixel);
        const auto outlined = ShaderContext::CompileCpu(ShaderPath(), ShaderType::Pixel,
                                                       {.defines = {{"SELECTION_OUTLINE", "1"}}});
        Check(device->CreateVertexShader(vertex.blob->GetBufferPointer(), vertex.blob->GetBufferSize(), nullptr, &vs));
        Check(device->CreatePixelShader(pixel.blob->GetBufferPointer(), pixel.blob->GetBufferSize(), nullptr, &ps));
        Check(device->CreatePixelShader(outlined.blob->GetBufferPointer(), outlined.blob->GetBufferSize(), nullptr, &outlinedPS));
        parameters = Buffer(Parameters{});
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&samplerDesc, &sampler));
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&rasterDesc, &raster));
    }

    template<class T> ComPtr<ID3D11Buffer> Buffer(const T& data) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(T);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{&data};
        ComPtr<ID3D11Buffer> result;
        Check(device->CreateBuffer(&desc, &initial, &result));
        return result;
    }

    ComPtr<ID3D11Texture2D> Texture(UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                   const void* data = nullptr, UINT pitch = 0) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = flags;
        D3D11_SUBRESOURCE_DATA initial{data, pitch};
        ComPtr<ID3D11Texture2D> result;
        Check(device->CreateTexture2D(&desc, data ? &initial : nullptr, &result));
        return result;
    }

    ComPtr<ID3D11ShaderResourceView> Source(const std::vector<Pixel>& data) {
        auto texture = Texture(static_cast<UINT>(data.size()), 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                               D3D11_BIND_SHADER_RESOURCE, data.data(), static_cast<UINT>(data.size() * sizeof(Pixel)));
        ComPtr<ID3D11ShaderResourceView> result;
        Check(device->CreateShaderResourceView(texture.Get(), nullptr, &result));
        return result;
    }

    std::vector<Pixel> Read(ID3D11Texture2D* target) {
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<Pixel> result(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(result.data() + static_cast<size_t>(y) * desc.Width,
                        static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * sizeof(Pixel));
        context->Unmap(staging.Get(), 0);
        return result;
    }

    std::vector<Pixel> Draw(ID3D11ShaderResourceView* source, UINT width, UINT height,
                            RenderDebugView mode, float nearPlane = 1, float farPlane = 101,
                            ID3D11ShaderResourceView* mask = nullptr, bool outlined = false) {
        auto target = Texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        // Like PresentPass, detach any preceding writable/read-only depth view
        // before sampling and rebind every per-mode input without ClearState.
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const float poison[]{-99, -99, -99, -99};
        context->ClearRenderTargetView(rtv.Get(), poison);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(raster.Get());
        context->OMSetDepthStencilState(nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(outlined ? outlinedPS.Get() : ps.Get(), nullptr, 0);
        const Parameters data{static_cast<uint32_t>(mode), nearPlane, farPlane};
        context->UpdateSubresource(parameters.Get(), 0, nullptr, &data, 0, 0);
        context->PSSetConstantBuffers(5, 1, parameters.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(5, 1, &source);
        context->PSSetShaderResources(16, 1, &mask);
        context->Draw(6, 0);
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(5, 1, &empty);
        context->PSSetShaderResources(16, 1, &empty);
        return Read(target.Get());
    }

    void ExpectPixel(const Pixel& actual, const Pixel& expected, float tolerance = 1e-5f) {
        for (size_t c = 0; c < 4; ++c) {
            EXPECT_TRUE(std::isfinite(actual[c]));
            EXPECT_NEAR(actual[c], expected[c], tolerance) << "channel " << c;
        }
    }
};

TEST(RenderDebugView, PresentationBufferMatchesRegisterAndFieldOffsets) {
    for (bool outlined : {false, true}) {
        ShaderCompileOptions options;
        if (outlined) options.defines.push_back({"SELECTION_OUTLINE", "1"});
        const auto shader = ShaderContext::CompileCpu(ShaderPath(), ShaderType::Pixel, options);
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        ASSERT_TRUE(SUCCEEDED(shader.reflection->GetResourceBindingDescByName("PresentParameters", &binding)));
        EXPECT_EQ(binding.BindPoint, 5u);
        auto* buffer = shader.reflection->GetConstantBufferByName("PresentParameters");
        D3D11_SHADER_BUFFER_DESC desc{};
        ASSERT_TRUE(SUCCEEDED(buffer->GetDesc(&desc)));
        EXPECT_EQ(desc.Size, 16u);
        const std::array names{"DebugView", "NearPlane", "FarPlane", "PresentPadding"};
        for (UINT i = 0; i < names.size(); ++i) {
            D3D11_SHADER_VARIABLE_DESC variable{};
            ASSERT_TRUE(SUCCEEDED(buffer->GetVariableByName(names[i])->GetDesc(&variable)));
            EXPECT_EQ(variable.StartOffset, i * 4);
            EXPECT_EQ(variable.Size, 4u);
        }
    }
}

TEST_F(RenderDebugWarpTest, RepeatedSourceSwitchingAndTargetResizeKeepExactColorsAndBlackBackground) {
    auto lit = Source({Pixel{4, 1, 0.25f, 1}, Pixel{0, 0, 0, 0}});
    auto albedo = Source({Pixel{0.2f, 0.6f, 0.9f, 1}, Pixel{0, 0, 0, 0}});
    auto normal = Source({Pixel{0, 0.5f, 1, 1}, Pixel{0, 0, 0, 0}});
    auto depth = Source({Pixel{1, 0, 0, 0}, Pixel{0, 0, 0, 0}});
    // Changing extent is feasible offscreen; native minimize/single Present and
    // lifecycle suspension are integration-owner coverage, not simulated here.
    for (UINT width : {2u, 16u, 4u, 2u}) {
        for (int repeat = 0; repeat < 4; ++repeat) {
            for (auto mode : {RenderDebugView::Final, RenderDebugView::Depth,
                              RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Final}) {
                auto* input = mode == RenderDebugView::Final ? lit.Get() : mode == RenderDebugView::Albedo ? albedo.Get()
                            : mode == RenderDebugView::Normals ? normal.Get() : depth.Get();
                const auto pixels = Draw(input, width, 3, mode);
                const Pixel expected = mode == RenderDebugView::Final ? Pixel{0.8f, 0.5f, 0.2f, 1}
                                     : mode == RenderDebugView::Albedo ? Pixel{0.2f, 0.6f, 0.9f, 1}
                                     : mode == RenderDebugView::Normals ? Pixel{0, 0.5f, 1, 1} : Pixel{1, 1, 1, 1};
                for (UINT y = 0; y < 3; ++y)
                    for (UINT x = 0; x < width; ++x)
                        ExpectPixel(pixels[y * width + x], x < width / 2 ? expected : Pixel{0, 0, 0, 1});
            }
        }
    }
}

TEST_F(RenderDebugWarpTest, LinearDepthMatchesActualReverseProjectionAcrossCameraRanges) {
    for (const auto range : {std::array{0.1f, 1000.0f}, std::array{1.0f, 101.0f}, std::array{3.0f, 17.0f}}) {
        FPSCamera camera{};
        camera.fov = 1.0f;
        camera.aspectRatio = 1;
        camera.nearPlane = range[0];
        camera.farPlane = range[1];
        const auto projection = camera.GetProjectionMatrix();
        std::vector<Pixel> raw;
        for (float fraction : {0.0f, 0.1f, 0.5f, 0.9f}) {
            const float distance = range[0] + fraction * (range[1] - range[0]);
            const auto clip = Vector4::Transform(Vector4(0, 0, -distance, 1), projection);
            raw.push_back({clip.z / clip.w, 0, 0, 0});
        }
        raw.push_back({0, 0, 0, 0});
        auto source = Source(raw);
        const auto pixels = Draw(source.Get(), 5, 1, RenderDebugView::Depth, range[0], range[1]);
        const std::array expected{1.0f, 0.9f, 0.5f, 0.1f, 0.0f};
        for (size_t i = 0; i < expected.size(); ++i)
            ExpectPixel(pixels[i], {expected[i], expected[i], expected[i], 1}, 2e-5f);
    }
}

TEST_F(RenderDebugWarpTest, InvalidDepthAndCameraRangesStayFiniteAndBlack) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto invalid = Source({Pixel{0, 0, 0, 0}, Pixel{-1, 0, 0, 0}, Pixel{nan, 0, 0, 0},
                           Pixel{inf, 0, 0, 0}, Pixel{2, 0, 0, 0}});
    for (const auto& pixel : Draw(invalid.Get(), 5, 1, RenderDebugView::Depth))
        ExpectPixel(pixel, {0, 0, 0, 1});
    auto valid = Source({Pixel{0.5f, 0, 0, 0}});
    for (const auto range : {std::array{0.0f, 10.0f}, std::array{1.0f, 1.0f}, std::array{10.0f, 1.0f},
                             std::array{nan, 10.0f}, std::array{1.0f, inf}})
        ExpectPixel(Draw(valid.Get(), 1, 1, RenderDebugView::Depth, range[0], range[1])[0], {0, 0, 0, 1});
}

TEST_F(RenderDebugWarpTest, SelectionOutlineOnlyModifiesFinalEvenWithOutlinedShaderBound) {
    auto source = Source(std::vector<Pixel>(5, Pixel{0.25f, 0.5f, 0.75f, 1}));
    auto mask = Source({Pixel{0, 0, 0, 0}, Pixel{0, 0, 0, 0}, Pixel{1, 0, 0, 0},
                        Pixel{0, 0, 0, 0}, Pixel{0, 0, 0, 0}});
    const auto final = Draw(source.Get(), 5, 1, RenderDebugView::Final, 1, 101, mask.Get(), true);
    ExpectPixel(final[0], {1, 0.72f, 0.12f, 1});
    for (auto mode : {RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Depth}) {
        const auto plain = Draw(source.Get(), 5, 1, mode);
        const auto outlined = Draw(source.Get(), 5, 1, mode, 1, 101, mask.Get(), true);
        for (size_t i = 0; i < plain.size(); ++i) ExpectPixel(outlined[i], plain[i]);
    }
}

TEST_F(RenderDebugWarpTest, WritableDepthIsDetachedForSamplingAndCanBeReboundAfterward) {
    auto depth = Texture(2, 1, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    Check(device->CreateDepthStencilView(depth.Get(), &dsvDesc, &dsv));
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    Check(device->CreateShaderResourceView(depth.Get(), &srvDesc, &srv));
    for (float raw : {1.0f, 0.0f, 0.5f}) {
        context->OMSetRenderTargets(0, nullptr, dsv.Get());
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, raw, 0);
        const auto pixels = Draw(srv.Get(), 2, 1, RenderDebugView::Depth);
        const float expected = raw == 1 ? 1.0f : raw == 0 ? 0.0f : 0.990196078f;
        for (const auto& pixel : pixels) ExpectPixel(pixel, {expected, expected, expected, 1});
        context->OMSetRenderTargets(0, nullptr, dsv.Get());
        ComPtr<ID3D11DepthStencilView> bound;
        context->OMGetRenderTargets(0, nullptr, &bound);
        EXPECT_EQ(bound.Get(), dsv.Get());
        ComPtr<ID3D11ShaderResourceView> boundSource;
        context->PSGetShaderResources(5, 1, &boundSource);
        EXPECT_EQ(boundSource.Get(), nullptr);
    }
}

TEST_F(RenderDebugWarpTest, StaticAndSkinnedGBufferShadersFeedStoredColorNormalAndProjectedDepth) {
    // The generated wrapper supplies deterministic geometry to the production
    // GBufferVS, including its real skinning path. The production GBufferPS
    // writes the engine's actual attachment formats before presentation.
    constexpr auto source = R"(
#define VSMain GBufferVertexMain
#include "GBufferVS.hlsl"
#undef VSMain
GBufferInput VSMain(uint id : SV_VertexID) {
    GBufferVertexInput v = (GBufferVertexInput)0;
    v.pos = float4(id == 1 ? 306 : -102, id == 2 ? 306 : -102, -51, 1);
    v.normal = float3(1, 0, 0);
#ifdef ENGINE_SKINNED
    v.joints = uint4(0, 0, 0, 0);
    v.weights = float4(1, 0, 0, 0);
#endif
    return GBufferVertexMain(v);
})";
    auto color = Source({Pixel{0.2f, 0.6f, 0.8f, 1}});
    const auto pixel = ShaderContext::CompileCpu(ShaderPath("MaterialTemplate.hlsl"), ShaderType::Pixel,
                                                 {.defines = {{"MATERIAL_GBUFFER_PS", "1"}}});
    ComPtr<ID3D11PixelShader> gbufferPS;
    Check(device->CreatePixelShader(pixel.blob->GetBufferPointer(), pixel.blob->GetBufferSize(), nullptr, &gbufferPS));
    FPSCamera camera{};
    camera.fov = DirectX::XM_PIDIV2;
    camera.aspectRatio = 1;
    camera.nearPlane = 1;
    camera.farPlane = 101;
    ViewBufferData view{};
    view.viewMatrix = Matrix::Identity;
    view.projectionMatrix = camera.GetProjectionMatrix();
    auto viewBuffer = Buffer(view);
    ObjectBufferData object{Matrix::Identity, Matrix::Identity};
    auto objectBuffer = Buffer(object);
    auto materialBuffer = Buffer(MaterialBufferData{});
    SkinningBufferData palette{};
    palette.matrices.fill(Matrix::Identity);
    palette.matrices[0] = Matrix::CreateRotationZ(DirectX::XM_PIDIV2);
    auto skinBuffer = Buffer(palette);
    D3D11_DEPTH_STENCIL_DESC depthStateDesc{};
    depthStateDesc.DepthEnable = TRUE;
    depthStateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStateDesc.DepthFunc = D3D11_COMPARISON_GREATER;
    ComPtr<ID3D11DepthStencilState> depthState;
    Check(device->CreateDepthStencilState(&depthStateDesc, &depthState));

    for (bool skinned : {false, true}) {
        ShaderCompileOptions options;
        if (skinned) options.defines.push_back({"ENGINE_SKINNED", "1"});
        const auto vertex = ShaderContext::CompileSource(source, ShaderPath("DebugFixture.hlsl"), ShaderType::Vertex, options);
        ComPtr<ID3D11VertexShader> gbufferVS;
        Check(device->CreateVertexShader(vertex.blob->GetBufferPointer(), vertex.blob->GetBufferSize(), nullptr, &gbufferVS));
        std::array<ComPtr<ID3D11Texture2D>, 3> targets;
        std::array<ComPtr<ID3D11RenderTargetView>, 3> rtvs;
        std::array<ComPtr<ID3D11ShaderResourceView>, 3> srvs;
        for (size_t i = 0; i < targets.size(); ++i) {
            targets[i] = Texture(1, 1, i == 1 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            Check(device->CreateRenderTargetView(targets[i].Get(), nullptr, &rtvs[i]));
            Check(device->CreateShaderResourceView(targets[i].Get(), nullptr, &srvs[i]));
            const float clear[]{0, 0, 0, 0};
            context->ClearRenderTargetView(rtvs[i].Get(), clear);
        }
        auto depth = Texture(1, 1, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc{};
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> dsv;
        Check(device->CreateDepthStencilView(depth.Get(), &depthDesc, &dsv));
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> depthSRV;
        Check(device->CreateShaderResourceView(depth.Get(), &srvDesc, &depthSRV));
        const std::array<ID3D11RenderTargetView*, 3> rawRTVs{rtvs[0].Get(), rtvs[1].Get(), rtvs[2].Get()};
        context->OMSetRenderTargets(3, rawRTVs.data(), dsv.Get());
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
        context->OMSetDepthStencilState(depthState.Get(), 0);
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(gbufferVS.Get(), nullptr, 0);
        context->PSSetShader(gbufferPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(1, 1, viewBuffer.GetAddressOf());
        context->VSSetConstantBuffers(3, 1, objectBuffer.GetAddressOf());
        context->VSSetConstantBuffers(9, 1, skinBuffer.GetAddressOf());
        context->PSSetConstantBuffers(2, 1, materialBuffer.GetAddressOf());
        context->PSSetSamplers(2, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, color.GetAddressOf());
        context->Draw(3, 0);
        ExpectPixel(Draw(srvs[0].Get(), 1, 1, RenderDebugView::Albedo)[0], {0.2f, 0.6f, 0.8f, 1}, 1.0f / 255);
        ExpectPixel(Draw(srvs[1].Get(), 1, 1, RenderDebugView::Normals)[0],
                    skinned ? Pixel{0.5f, 1, 0.5f, 1} : Pixel{1, 0.5f, 0.5f, 1}, 0.001f);
        ExpectPixel(Draw(depthSRV.Get(), 1, 1, RenderDebugView::Depth)[0], {0.5f, 0.5f, 0.5f, 1});
    }
}

TEST(RenderDebugView, PresentFailureIsStructuredAndRepeatedCleanupIsSafeWithoutDevice) {
    RenderingContext context;
    auto& manager = context.GetResourceManager();
    RenderingResources resources(8, 8);
    PresentPass pass;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            pass.Initialize(manager, context, resources);
            FAIL() << "An absent swap chain must fail";
        } catch (const RenderingException& error) {
            EXPECT_EQ(error.GetFailure().operation, "ValidateBackbuffer");
            EXPECT_EQ(error.GetFailure().pass, "PresentPass");
            EXPECT_EQ(error.GetFailure().nativeError, static_cast<std::int64_t>(E_POINTER));
        }
        pass.Destroy(manager, context, resources);
        pass.Destroy(manager, context, resources);
        EXPECT_TRUE(pass.PreReinitialize(manager, context, resources));
        EXPECT_THROW(pass.Reinitialize(manager, context, resources), RenderingException);
    }
}
} // namespace
