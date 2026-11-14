#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/input/Win32InputHost.hpp>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>
#include "ui/UiDx11Renderer.hpp"
#include "ui/UiInterfaces.hpp"

#include <RmlUi/Core.h>
#include <d3d11sdklayers.h>
#include <wincodec.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace engine;
using Microsoft::WRL::ComPtr;

struct MetricsHost final : UiHost {
    std::optional<UiSurfaceMetrics> metrics;
    std::optional<UiSurfaceMetrics> GetSurfaceMetrics() const override { return metrics; }
    bool ReadClipboard(std::string&, std::string&) override { return false; }
    bool WriteClipboard(std::string_view, std::string&) override { return false; }
    void SetCursor(std::string_view) override {}
    void SetTextInput(const UiTextInputState&) override {}
    void CancelComposition() override {}
    void SetCapture(const UiCaptureState&) override {}
};

TEST(UiSurfaceMetricsTest, HostSnapshotsAndExplicitFallbackReachContext) {
    auto platform = std::make_shared<MetricsHost>();
    platform->metrics = UiSurfaceMetrics{640, 480, 1.5f};
    Engine host{EngineConfig{.taskThreads = 1}};
    auto* ui = host.AddSubsystem<UiSubsystem>(UiSubsystemConfig{
        .renderer = UiRendererBackend::Null, .host = platform, .nullSurface = {800, 600, 1.0f}});
    ASSERT_TRUE(host.Initialize());
    auto* context = Rml::GetContext("engine-ui");
    ASSERT_NE(context, nullptr);
    auto check = [&](int w, int h, float dpi) {
        EXPECT_EQ(context->GetDimensions(), Rml::Vector2i(w, h));
        EXPECT_FLOAT_EQ(context->GetDensityIndependentPixelRatio(), dpi);
    };
    check(640, 480, 1.5f);
    platform->metrics = UiSurfaceMetrics{640, 480, 2.0f}; host.Tick(); check(640, 480, 2.0f);
    platform->metrics = UiSurfaceMetrics{900, 700, 2.0f}; host.Tick(); check(900, 700, 2.0f);
    platform->metrics = UiSurfaceMetrics{}; host.Tick(); check(0, 0, 1.0f);
    platform->metrics = UiSurfaceMetrics{900, 700, 2.0f}; host.Tick(); check(900, 700, 2.0f);
    platform->metrics.reset();
    ui->SetSurfaceMetrics({320, 200, 1.25f}); host.Tick(); check(320, 200, 1.25f);
    host.Shutdown();
}

TEST(UiSurfaceMetricsTest, Win32BridgeReportsLostWindowAndRevokedOwner) {
    HWND window = CreateWindowExW(0, L"STATIC", L"metrics", WS_OVERLAPPEDWINDOW,
                                  0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);
    std::shared_ptr<UiHost> bridge;
    {
        Win32InputHost native(window);
        bridge = native.GetUiHost();
        ASSERT_GT(bridge->GetSurfaceMetrics()->pixelWidth, 0u);
        DestroyWindow(window);
        EXPECT_EQ(bridge->GetSurfaceMetrics()->pixelWidth, 0u);
    }
    ASSERT_TRUE(bridge->GetSurfaceMetrics());
    EXPECT_EQ(bridge->GetSurfaceMetrics()->pixelHeight, 0u);
}

// This suite deliberately uses the production hardware/device/debug-layer path.
// A missing device or Graphics Tools installation is a test failure, not a skip.
// Run sequentially: RmlUi interfaces and asset roots are process-global.
class UiDx11Test : public testing::Test {
protected:
    static constexpr UINT width = 256, height = 192;
    HWND window = nullptr;
    std::unique_ptr<RenderingContext> graphics;
    std::unique_ptr<Rml::RenderInterface> renderer;
    ComPtr<ID3D11InfoQueue> messages;
    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11RenderTargetView> target;
    std::shared_ptr<ui_detail::NullHost> host = std::make_shared<ui_detail::NullHost>();
    ui_detail::System system{host};
    ui_detail::Files files;
    Rml::Context* context = nullptr;
    bool rmlInitialized = false;
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-ui-dx11-" + NewUuid());
    std::string error;

    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        window = CreateWindowExW(0, L"STATIC", L"UI DX11 integration", WS_OVERLAPPEDWINDOW,
                                 0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        graphics = std::make_unique<RenderingContext>();
        ASSERT_TRUE(graphics->Initialize({window, width, height}));
        ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->QueryInterface(IID_PPV_ARGS(messages.GetAddressOf())));
        messages->ClearStoredMessages();
        renderer = CreateUiDx11Renderer(*graphics, error);
        ASSERT_NE(renderer, nullptr) << error;
        Rml::SetSystemInterface(&system);
        Rml::SetFileInterface(&files);
        Rml::SetRenderInterface(renderer.get());
        rmlInitialized = Rml::Initialise();
        ASSERT_TRUE(rmlInitialized);
        context = Rml::CreateContext("dx11-integration", {width, height});
        ASSERT_NE(context, nullptr);
        system.collecting = files.collecting = true;
        AcquireTarget();
    }

    void AcquireTarget() {
        ASSERT_HRESULT_SUCCEEDED(graphics->GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(backbuffer.ReleaseAndGetAddressOf())));
        ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->CreateRenderTargetView(
            backbuffer.Get(), nullptr, target.ReleaseAndGetAddressOf()));
        graphics->GetDeviceContext()->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    }

    void CheckDebugMessages() {
        if (!messages) return;
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, nullptr, &size));
            // Use suitably aligned storage for the variable-length D3D11_MESSAGE.
            std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, message, &size));
            EXPECT_GT(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR)
                << "D3D11 message " << message->ID << ": " << message->pDescription;
        }
    }

    void TearDown() override {
        if (rmlInitialized) Rml::Shutdown();
        context = nullptr;
        Rml::SetRenderInterface(nullptr);
        Rml::SetFileInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
        renderer.reset();
        if (graphics && graphics->GetDeviceContext()) graphics->GetDeviceContext()->ClearState();
        target.Reset();
        backbuffer.Reset();
        CheckDebugMessages();
        messages.Reset();
        graphics.reset();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    Rml::ElementDocument* Document(const Rml::String& body) {
        auto* document = context->LoadDocumentFromMemory(
            "<rml><head><title>DX11 probe</title><style>"
            "body { margin: 0px; width: 100%; height: 100%; }"
            "div, img { position: absolute; }"
            "</style></head><body>" + body + "</body></rml>");
        EXPECT_NE(document, nullptr);
        if (document) document->Show();
        return document;
    }

    void Draw(UINT w = width, UINT h = height) {
        ASSERT_TRUE(context->Update());
        UiDx11BeginFrame(*renderer, *graphics, static_cast<int>(w), static_cast<int>(h));
        EXPECT_TRUE(context->Render());
        UiDx11EndFrame(*renderer, *graphics);
        for (const auto& diagnostic : system.diagnostics) ADD_FAILURE() << diagnostic;
        for (const auto& diagnostic : files.diagnostics) ADD_FAILURE() << diagnostic;
        system.diagnostics.clear();
        files.diagnostics.clear();
    }

    void Clear(std::array<float, 4> color = {0, 0, 1, 1}) {
        graphics->GetDeviceContext()->ClearRenderTargetView(target.Get(), color.data());
    }

    // Copy and Map synchronize the GPU. Never Present before sampling flip-discard buffers.
    std::vector<std::array<unsigned char, 4>> Readback() {
        D3D11_TEXTURE2D_DESC desc{};
        backbuffer->GetDesc(&desc);
        EXPECT_EQ(desc.Format, DXGI_FORMAT_B8G8R8A8_UNORM);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(graphics->GetDevice()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
            ADD_FAILURE() << "Could not create readback texture";
            return {};
        }
        auto* dc = graphics->GetDeviceContext();
        dc->CopyResource(staging.Get(), backbuffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            ADD_FAILURE() << "Could not map readback texture";
            return {};
        }
        std::vector<std::array<unsigned char, 4>> pixels(std::size_t(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x)
                pixels[y * desc.Width + x] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4], row[x * 4 + 3]};
        }
        dc->Unmap(staging.Get(), 0);
        return pixels;
    }

    static void Pixel(const std::vector<std::array<unsigned char, 4>>& pixels, UINT x, UINT y,
                      std::array<int, 4> expected, UINT stride = width, int tolerance = 3) {
        ASSERT_LT(std::size_t(y) * stride + x, pixels.size());
        const auto& actual = pixels[y * stride + x];
        for (std::size_t c = 0; c < 4; ++c)
            EXPECT_NEAR(actual[c], expected[c], tolerance) << "pixel " << x << ',' << y << " channel " << c;
    }

    void WriteAlphaPng() {
        // Encode straight-alpha PNG; production LoadTexture must premultiply it exactly once.
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(factory.GetAddressOf())));
        ASSERT_HRESULT_SUCCEEDED(factory->CreateStream(stream.GetAddressOf()));
        ASSERT_HRESULT_SUCCEEDED(stream->InitializeFromFilename((root / "alpha.png").c_str(), GENERIC_WRITE));
        ASSERT_HRESULT_SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
        ASSERT_HRESULT_SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ASSERT_HRESULT_SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
        ASSERT_HRESULT_SUCCEEDED(frame->Initialize(nullptr));
        ASSERT_HRESULT_SUCCEEDED(frame->SetSize(2, 1));
        auto format = GUID_WICPixelFormat32bppBGRA;
        ASSERT_HRESULT_SUCCEEDED(frame->SetPixelFormat(&format));
        ASSERT_TRUE(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA));
        std::array<BYTE, 8> pixels{0, 0, 255, 128, 0, 255, 0, 255};
        ASSERT_HRESULT_SUCCEEDED(frame->WritePixels(1, 8, 8, pixels.data()));
        ASSERT_HRESULT_SUCCEEDED(frame->Commit());
        ASSERT_HRESULT_SUCCEEDED(encoder->Commit());
    }
};

TEST_F(UiDx11Test, CompositesOntoBoundBackbufferWithoutClearingScene) {
    ASSERT_NE(Document("<div style='left: 16px; top: 16px; width: 48px; height: 48px; background-color: #ff000080;'/>"), nullptr);
    Clear();
    Draw();
    const auto pixels = Readback();
    Pixel(pixels, 32, 32, {128, 0, 127, 255});
    Pixel(pixels, 100, 100, {0, 0, 255, 255});
}

TEST_F(UiDx11Test, RejectsForeignRmlOwnerWithoutReplacingItsInterfaces) {
    Engine engine{EngineConfig{.taskThreads = 1}};
    auto* ui = engine.AddSubsystem<UiSubsystem>(UiSubsystemConfig{.renderer = UiRendererBackend::Null});
    EXPECT_FALSE(engine.Initialize());
    EXPECT_FALSE(ui->IsInitialized());
    EXPECT_EQ(Rml::GetRenderInterface(), renderer.get());
    EXPECT_EQ(Rml::GetSystemInterface(), &system);
    ASSERT_NE(Document("<div style='width: 32px; height: 32px; background-color: #00ff00;'/>"), nullptr);
    Clear();
    Draw();
    Pixel(Readback(), 16, 16, {0, 255, 0, 255});
}

TEST_F(UiDx11Test, CompositesToExactBoundTargetRatherThanAssumingSwapchain) {
    ASSERT_NE(Document("<div style='left: 16px; top: 16px; width: 48px; height: 48px; background-color: #00ff00;'/>"), nullptr);
    Clear();
    auto swapbuffer = backbuffer;
    auto swapTarget = target;
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    backbuffer.Reset();
    target.Reset();
    ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->CreateTexture2D(&desc, nullptr, backbuffer.GetAddressOf()));
    ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->CreateRenderTargetView(backbuffer.Get(), nullptr, target.GetAddressOf()));
    graphics->GetDeviceContext()->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    Clear({1, 0, 0, 1});
    Draw();
    auto pixels = Readback();
    Pixel(pixels, 32, 32, {0, 255, 0, 255});
    Pixel(pixels, 100, 100, {255, 0, 0, 255});
    backbuffer = swapbuffer;
    target = swapTarget;
    pixels = Readback();
    Pixel(pixels, 32, 32, {0, 0, 255, 255});
    Pixel(pixels, 100, 100, {0, 0, 255, 255});
}

TEST_F(UiDx11Test, PngStraightAlphaIsPremultipliedOnceAndChannelsAreCorrect) {
    WriteAlphaPng();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_NE(Document("<img src='game/alpha.png' style='left: 16px; top: 16px; width: 96px; height: 48px;'/>"), nullptr);
    Clear();
    Draw();
    const auto pixels = Readback();
    // Sample texel centers, allowing only the subpixel bilinear interpolation error.
    Pixel(pixels, 39, 32, {128, 0, 127, 255}, width, 4);
    Pixel(pixels, 87, 32, {0, 255, 0, 255}, width, 4);
    Pixel(pixels, 180, 32, {0, 0, 255, 255});
}

TEST_F(UiDx11Test, FilterLayerCompositesPremultipliedAlphaOverScene) {
    ASSERT_NE(Document(R"(<div style="left: 16px; top: 16px; width: 64px; height: 64px;
        background-color: #00ff00; filter: opacity(0.5);"/>)"), nullptr);
    Clear();
    Draw();
    const auto pixels = Readback();
    Pixel(pixels, 48, 48, {0, 128, 127, 255});
    Pixel(pixels, 100, 100, {0, 0, 255, 255});
}

TEST_F(UiDx11Test, ScissorsTransformsRoundedClipsAndGradientShadersProducePixels) {
    ASSERT_NE(Document(R"(
        <div style="left: 16px; top: 16px; width: 32px; height: 32px; overflow: hidden; clip: always;">
            <div style="width: 80px; height: 80px; background-color: #00ff00;"/></div>
        <div style="left: 64px; top: 16px; width: 24px; height: 24px; background-color: #ff0000; transform: translateX(32px);"/>
        <div style="left: 144px; top: 16px; width: 64px; height: 64px; border-radius: 24px; overflow: hidden;">
            <div style="width: 64px; height: 64px; background-color: #00ff00;"/></div>
        <div style="left: 16px; top: 112px; width: 192px; height: 32px; decorator: linear-gradient(to right, #ff0000, #00ff00);"/>
    )"), nullptr);
    Clear();
    Draw();
    const auto pixels = Readback();
    Pixel(pixels, 32, 32, {0, 255, 0, 255});
    Pixel(pixels, 52, 32, {0, 0, 255, 255});
    Pixel(pixels, 72, 24, {0, 0, 255, 255});
    Pixel(pixels, 108, 28, {255, 0, 0, 255});
    Pixel(pixels, 176, 48, {0, 255, 0, 255});
    Pixel(pixels, 145, 17, {0, 0, 255, 255});
    ASSERT_EQ(pixels.size(), width * height);
    EXPECT_GT(pixels[128 * width + 24][0], 220);
    EXPECT_LT(pixels[128 * width + 24][1], 35);
    EXPECT_GT(pixels[128 * width + 200][1], 220);
    EXPECT_LT(pixels[128 * width + 200][0], 35);
}

TEST_F(UiDx11Test, FontAtlasProducesVisibleTextAndRespectsOverflowClip) {
    ASSERT_TRUE(Rml::LoadFontFace("engine/ui/fonts/DejaVuSans.ttf"));
    ASSERT_NE(Document(R"(<div style="left: 16px; top: 16px; width: 64px; height: 48px; overflow: hidden;
        font-family: DejaVu Sans; font-size: 32px; color: #ffffff;">MMMMMMMM</div>)"), nullptr);
    Clear({0, 0, 0, 1});
    Draw();
    const auto pixels = Readback();
    ASSERT_EQ(pixels.size(), width * height);
    unsigned lit = 0;
    for (UINT y = 16; y < 64; ++y)
        for (UINT x = 16; x < 80; ++x)
            if (pixels[y * width + x][0] > 100) ++lit;
    EXPECT_GT(lit, 100u); // Do not require platform-specific glyph edge coverage.
    for (UINT y = 16; y < 64; ++y) Pixel(pixels, 84, y, {0, 0, 0, 255});
}

TEST_F(UiDx11Test, RestoresNonDefaultPipelineStateAndExactOutputTarget) {
    ASSERT_NE(Document("<div style='width: 32px; height: 32px; background-color: white;'/>"), nullptr);
    auto* dc = graphics->GetDeviceContext();
    ComPtr<ID3D11Buffer> constant;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->CreateBuffer(&buffer, nullptr, constant.GetAddressOf()));
    dc->VSSetConstantBuffers(7, 1, constant.GetAddressOf());
    dc->GSSetConstantBuffers(5, 1, constant.GetAddressOf());
    dc->HSSetConstantBuffers(4, 1, constant.GetAddressOf());
    dc->DSSetConstantBuffers(3, 1, constant.GetAddressOf());
    dc->PSSetConstantBuffers(6, 1, constant.GetAddressOf());
    dc->CSSetConstantBuffers(2, 1, constant.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = textureDesc.Height = textureDesc.MipLevels = textureDesc.ArraySize = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->CreateTexture2D(&textureDesc, nullptr, texture.GetAddressOf()));
    ComPtr<ID3D11ShaderResourceView> resource;
    ASSERT_HRESULT_SUCCEEDED(graphics->GetDevice()->CreateShaderResourceView(texture.Get(), nullptr, resource.GetAddressOf()));
    dc->VSSetShaderResources(12, 1, resource.GetAddressOf());
    dc->GSSetShaderResources(13, 1, resource.GetAddressOf());
    dc->HSSetShaderResources(14, 1, resource.GetAddressOf());
    dc->DSSetShaderResources(15, 1, resource.GetAddressOf());
    dc->PSSetShaderResources(16, 1, resource.GetAddressOf());
    dc->CSSetShaderResources(17, 1, resource.GetAddressOf());
    const D3D11_VIEWPORT viewport{3, 5, 120, 90, .2f, .8f};
    const D3D11_RECT scissor{7, 9, 99, 77};
    dc->RSSetViewports(1, &viewport);
    dc->RSSetScissorRects(1, &scissor);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    const float factors[4]{.1f, .2f, .3f, .4f};
    dc->OMSetBlendState(nullptr, factors, 0x12345678);
    dc->OMSetDepthStencilState(nullptr, 37);
    Clear();
    Draw();
    ComPtr<ID3D11RenderTargetView> restored;
    dc->OMGetRenderTargets(1, restored.GetAddressOf(), nullptr);
    EXPECT_EQ(restored.Get(), target.Get());
    D3D11_VIEWPORT actualViewport{};
    UINT count = 1;
    dc->RSGetViewports(&count, &actualViewport);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(std::memcmp(&viewport, &actualViewport, sizeof(viewport)), 0);
    D3D11_RECT actualScissor{};
    count = 1;
    dc->RSGetScissorRects(&count, &actualScissor);
    EXPECT_EQ(std::memcmp(&scissor, &actualScissor, sizeof(scissor)), 0);
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    dc->IAGetPrimitiveTopology(&topology);
    EXPECT_EQ(topology, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    float actualFactors[4]{};
    UINT mask = 0, stencil = 0;
    ComPtr<ID3D11BlendState> blend;
    dc->OMGetBlendState(blend.GetAddressOf(), actualFactors, &mask);
    EXPECT_EQ(blend.Get(), nullptr);
    EXPECT_EQ(mask, 0x12345678u);
    for (int i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(actualFactors[i], factors[i]);
    ComPtr<ID3D11DepthStencilState> depth;
    dc->OMGetDepthStencilState(depth.GetAddressOf(), &stencil);
    EXPECT_EQ(depth.Get(), nullptr);
    EXPECT_EQ(stencil, 37u);
    ComPtr<ID3D11Buffer> actual;
    dc->VSGetConstantBuffers(7, 1, actual.ReleaseAndGetAddressOf()); EXPECT_EQ(actual.Get(), constant.Get());
    dc->GSGetConstantBuffers(5, 1, actual.ReleaseAndGetAddressOf()); EXPECT_EQ(actual.Get(), constant.Get());
    dc->HSGetConstantBuffers(4, 1, actual.ReleaseAndGetAddressOf()); EXPECT_EQ(actual.Get(), constant.Get());
    dc->DSGetConstantBuffers(3, 1, actual.ReleaseAndGetAddressOf()); EXPECT_EQ(actual.Get(), constant.Get());
    dc->PSGetConstantBuffers(6, 1, actual.ReleaseAndGetAddressOf()); EXPECT_EQ(actual.Get(), constant.Get());
    dc->CSGetConstantBuffers(2, 1, actual.ReleaseAndGetAddressOf()); EXPECT_EQ(actual.Get(), constant.Get());
    ComPtr<ID3D11ShaderResourceView> actualResource;
    dc->VSGetShaderResources(12, 1, actualResource.ReleaseAndGetAddressOf()); EXPECT_EQ(actualResource.Get(), resource.Get());
    dc->GSGetShaderResources(13, 1, actualResource.ReleaseAndGetAddressOf()); EXPECT_EQ(actualResource.Get(), resource.Get());
    dc->HSGetShaderResources(14, 1, actualResource.ReleaseAndGetAddressOf()); EXPECT_EQ(actualResource.Get(), resource.Get());
    dc->DSGetShaderResources(15, 1, actualResource.ReleaseAndGetAddressOf()); EXPECT_EQ(actualResource.Get(), resource.Get());
    dc->PSGetShaderResources(16, 1, actualResource.ReleaseAndGetAddressOf()); EXPECT_EQ(actualResource.Get(), resource.Get());
    dc->CSGetShaderResources(17, 1, actualResource.ReleaseAndGetAddressOf()); EXPECT_EQ(actualResource.Get(), resource.Get());
    Pixel(Readback(), 16, 16, {255, 255, 255, 255});
}

TEST_F(UiDx11Test, CompletedAndAbortedFramesReleaseBackbufferReferencesForRepeatedResize) {
    ASSERT_NE(Document("<div style='width: 100%; height: 100%; background-color: #00ff00;'/>"), nullptr);
    for (UINT i = 0; i < 6; ++i) {
        SCOPED_TRACE(i);
        const UINT w = width + i * 16, h = height + i * 8;
        if (i % 2 == 0) {
            Clear();
            Draw(w, h);
        } else {
            UiDx11BeginFrame(*renderer, *graphics, static_cast<int>(w), static_cast<int>(h));
            UiDx11ReleaseSurface(*renderer); // same cancellation hook used before resize/shutdown
        }
        graphics->GetDeviceContext()->ClearState();
        target.Reset();
        backbuffer.Reset();
        // A successful ResizeBuffers is the regression check for retained RTV/resource refs.
        ASSERT_TRUE(graphics->Reinitialize({window, w + 16, h + 8}));
        AcquireTarget();
        ASSERT_FALSE(HasFatalFailure());
        context->SetDimensions({static_cast<int>(w + 16), static_cast<int>(h + 8)});
        Clear();
        Draw(w + 16, h + 8);
        Pixel(Readback(), w, h, {0, 255, 0, 255}, w + 16);
    }
}

// Public subsystem smoke test complements the factory-level pixel probes above.
// Engine owns all subsystems; these pointers are borrowed for the fixture lifetime.
class UiDx11SubsystemTest : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-ui-overlay-" + NewUuid());
    Engine host{EngineConfig{.taskThreads = 1}};
    HWND window = nullptr;
    RenderingSubsystem* rendering = host.AddSubsystem<RenderingSubsystem>();
    std::shared_ptr<MetricsHost> platform = std::make_shared<MetricsHost>();
    UiSubsystem* ui = host.AddSubsystem<UiSubsystem>(UiSubsystemConfig{.host = platform});
    ScriptSubsystem* scripts = host.AddSubsystem<ScriptSubsystem>();
    ComPtr<ID3D11InfoQueue> messages;
    std::string error;

    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        const auto source = root / "overlay.rml";
        ASSERT_TRUE(CreateNewFile(source, R"(<rml><head><title>Actual overlay</title>
            <link type="text/rcss" href="engine/ui/base.rcss"/></head><body>
            <div class="panel" style="width: 160px; height: 64px;">DX11 overlay</div></body></rml>)", error)) << error;
        ASSERT_TRUE(WriteAssetMetadata(source, {NewUuid(), CatalogAssetKind::UiDocument}, error)) << error;
        window = CreateWindowExW(0, L"STATIC", L"UI subsystem integration", WS_OVERLAPPEDWINDOW,
                                 0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        host.SetSurface({window, 320, 240});
        ASSERT_TRUE(host.Initialize());
        ASSERT_TRUE(ui->IsInitialized());
        // This fixture does not register render extraction; supply the scene facts
        // normally initialized by that subsystem before the shadow pass runs.
        rendering->GetWorld().directionalLight.direction = {0, -1, 0};
        rendering->GetWorld().directionalLight.intensity = 0;
        rendering->GetWorld().camera.aspectRatio = 320.0f / 240.0f;
        rendering->GetWorld().camera.fov = 1.0f;
        rendering->GetWorld().camera.nearPlane = 0.1f;
        rendering->GetWorld().camera.farPlane = 100.0f;
        ASSERT_HRESULT_SUCCEEDED(rendering->GetContext().GetDevice()->QueryInterface(IID_PPV_ARGS(messages.GetAddressOf())));
    }

    void TearDown() override {
        host.Shutdown();
        if (messages) {
            for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, nullptr, &size));
                std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, message, &size));
                EXPECT_GT(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR) << message->pDescription;
            }
        }
        messages.Reset();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TEST_F(UiDx11SubsystemTest, HostDpiUsesRenderSurfaceDimensionsAndZeroSuspends) {
    auto* context = Rml::GetContext("engine-ui");
    ASSERT_NE(context, nullptr);
    platform->metrics = UiSurfaceMetrics{800, 600, 2.0f};
    host.Tick();
    EXPECT_EQ(context->GetDimensions(), Rml::Vector2i(320, 240));
    EXPECT_FLOAT_EQ(context->GetDensityIndependentPixelRatio(), 2.0f);
    platform->metrics = UiSurfaceMetrics{};
    host.Tick();
    EXPECT_EQ(context->GetDimensions(), Rml::Vector2i(0, 0));
    platform->metrics = UiSurfaceMetrics{400, 300, 1.5f};
    host.ResizeSurface(400, 300);
    host.Tick();
    EXPECT_EQ(context->GetDimensions(), Rml::Vector2i(400, 300));
    EXPECT_FLOAT_EQ(context->GetDensityIndependentPixelRatio(), 1.5f);
}

TEST_F(UiDx11SubsystemTest, PublicLifecycleRendersOverlayResizesAndDetachesBeforeShutdown) {
    UiSession session;
    UiOwner owner;
    UiDocument document;
    ASSERT_TRUE(ui->BeginNativeSession(session, error)) << error;
    ASSERT_TRUE(ui->CreateNativeOwner(session, "DX11 overlay smoke", owner, error)) << error;
    ASSERT_TRUE(ui->LoadDocument(owner, "game/overlay.rml", {}, document, error)) << error;
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Hud, error)) << error;
    for (UINT i = 0; i < 3; ++i) {
        const UINT w = 320 + i * 32, h = 240 + i * 24;
        host.ResizeSurface(w, h);
        host.Tick(); // Production final-pass -> UI overlay -> Present path.
        ComPtr<ID3D11Texture2D> surface;
        ASSERT_HRESULT_SUCCEEDED(rendering->GetContext().GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf())));
        D3D11_TEXTURE2D_DESC desc{};
        surface->GetDesc(&desc);
        EXPECT_EQ(desc.Width, w);
        EXPECT_EQ(desc.Height, h);
        EXPECT_TRUE(ui->IsVisible(document));
    }
    ui->Shutdown();
    EXPECT_FALSE(ui->IsInitialized());
    host.ResizeSurface(480, 360);
    host.Tick(); // Detached callback must never call the destroyed Rml runtime.
}
} // namespace
