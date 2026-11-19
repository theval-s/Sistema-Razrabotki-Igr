#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/PostProcessing.hpp>
#include <engine/rendering/RenderingSystem.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace engine;
using Pixel = std::array<float, 4>;

constexpr auto CopySource = R"(
Texture2D Source : register(t11);
SamplerState PointClamp : register(s0);
float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return Source.Sample(PointClamp, uv);
})";

// The final capture runs after UI, before Present invalidates flip-discard
// contents. Copy/Map synchronizes real GPU output, not callback-side bookkeeping.
class PostProcessingNative : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    HWND window{};
    RenderingSystem renderer;
    PostProcessOwner owner{}, captureOwner{};
    PostProcessPass capturePass{};
    ComPtr<ID3D11Texture2D> captured;
    ComPtr<ID3D11InfoQueue> messages;
    FrameContext frame{};
    UINT width = 95, height = 71;

    PostProcessing& Effects() { return renderer.GetPostProcessing(); }

    PostProcessShader Shader(PostProcessOwner shaderOwner, const std::string& source) {
        return Effects().CreateShader(shaderOwner, ShaderContext::CompileSource(
            source, std::filesystem::path(SRI_SOURCE_ROOT) / "tests/postprocessing_probe.hlsl", ShaderType::Pixel));
    }

    void SetUp() override {
        AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "tests");
        window = CreateWindowExW(0, L"STATIC", L"Postprocessing GPU regression", WS_OVERLAPPEDWINDOW,
                                 0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        const bool initialized = renderer.Initialize({window, width, height}, true);
        const auto diagnostics = renderer.GetDiagnostics();
        if (!initialized && diagnostics.failure && diagnostics.failure->operation == "D3D11CreateDeviceAndSwapChain")
            GTEST_SKIP() << diagnostics.failure->summary;
        ASSERT_TRUE(initialized) << (diagnostics.failure ? diagnostics.failure->detail : "");
        renderer.GetWorld().camera = FPSCamera{0.8f, float(width) / height, 0.1f, 100};
        renderer.GetWorld().directionalLight =
            {{0, 0, 0}, {0, -1, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};
        ASSERT_HRESULT_SUCCEEDED(renderer.GetRenderingContext().GetDevice()->QueryInterface(
            IID_PPV_ARGS(messages.GetAddressOf())));
        messages->ClearStoredMessages();
        owner = Effects().CreateOwner();
        captureOwner = Effects().CreateOwner();
        const auto copy = Shader(captureOwner, CopySource);
        capturePass = Effects().AddPass(captureOwner, "readback", PostProcessStage::AfterUI,
            (std::numeric_limits<int>::max)(), [this, copy](PostProcessContext& commands, const PostProcessFrame& f) {
                commands.SetPixelShader(copy);
                commands.SetRenderTarget(f.outputColor);
                commands.BindTexture(11, f.inputColor);
                commands.DrawFullscreen();
                CopyBoundTarget(captured);
            });
    }

    void CheckDebugMessages() {
        if (!messages) return;
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{};
            ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, nullptr, &size));
            std::vector<std::max_align_t> storage(
                (size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, message, &size));
            EXPECT_GT(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR) << message->pDescription;
            std::string text = message->pDescription;
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            EXPECT_EQ(text.find("hazard"), std::string::npos) << text;
            EXPECT_EQ(text.find("still bound on input"), std::string::npos) << text;
        }
        messages->ClearStoredMessages();
    }

    void TearDown() override {
        captured.Reset();
        renderer.Shutdown();
        CheckDebugMessages();
        messages.Reset();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
    }

    void CopyBoundTarget(ComPtr<ID3D11Texture2D>& destination) {
        auto& context = renderer.GetRenderingContext();
        ComPtr<ID3D11RenderTargetView> target;
        context.GetDeviceContext()->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        ASSERT_NE(target.Get(), nullptr);
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        ASSERT_HRESULT_SUCCEEDED(resource.As(&texture));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateTexture2D(
            &desc, nullptr, destination.ReleaseAndGetAddressOf()));
        context.GetDeviceContext()->CopyResource(destination.Get(), texture.Get());
    }

    void ReadPixel(ID3D11Texture2D* texture, Pixel& result, UINT x = 2, UINT y = 2) {
        ASSERT_NE(texture, nullptr);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        ASSERT_LT(x, desc.Width);
        ASSERT_LT(y, desc.Height);
        ASSERT_TRUE(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                    desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto* dc = renderer.GetRenderingContext().GetDeviceContext();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_HRESULT_SUCCEEDED(dc->Map(texture, 0, D3D11_MAP_READ, 0, &mapped));
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            for (size_t c = 0; c < 4; ++c) result[c] = row[x * 4 + c] / 255.0f;
            if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(result[0], result[2]);
        } else {
            std::array<std::uint16_t, 4> halves{};
            std::memcpy(halves.data(), row + x * 8, 8);
            for (size_t c = 0; c < 4; ++c)
                result[c] = DirectX::PackedVector::XMConvertHalfToFloat(halves[c]);
        }
        dc->Unmap(texture, 0);
    }

    void ExpectImage(Pixel expected) {
        ASSERT_NE(captured.Get(), nullptr);
        for (const auto& xy : {std::array<UINT, 2>{2, 2}, {width / 2, height / 2}, {width - 3, height - 3}}) {
            Pixel actual{};
            ASSERT_NO_FATAL_FAILURE(ReadPixel(captured.Get(), actual, xy[0], xy[1]));
            for (size_t c = 0; c < 4; ++c)
                EXPECT_NEAR(actual[c], expected[c], 0.012f) << "channel " << c;
        }
    }

    void Render(const RenderingSystem::OverlayCallback& overlay = {}) {
        captured.Reset();
        ++frame.frameIndex;
        renderer.RenderFrame(frame, overlay);
        const auto diagnostics = renderer.GetDiagnostics();
        ASSERT_EQ(diagnostics.lifecycle, RenderingLifecycle::Ready)
            << (diagnostics.failure ? diagnostics.failure->detail : "");
        ASSERT_TRUE(Effects().GetPassError(captureOwner, capturePass).empty());
        ASSERT_NE(captured.Get(), nullptr);
        CheckDebugMessages();
    }

    PostProcessPass Seed(PostProcessOwner seedOwner, PostProcessStage stage, Pixel color, int order = -100) {
        return Effects().AddPass(seedOwner, "seed", stage, order,
            [color](PostProcessContext& commands, const PostProcessFrame& f) {
                commands.ClearRenderTarget(f.outputColor, color);
            });
    }
};

TEST_F(PostProcessingNative, HdrIsProcessedBeforeTonemapAndUiIsProcessedAfterOverlay) {
    std::vector<int> events;
    const auto hdr = Shader(owner, R"(
float4 PSMain(float4 p : SV_POSITION) : SV_Target { return float4(4, 1, .25, 1); })");
    const auto half = Shader(owner, R"(
Texture2D Source : register(t11);
SamplerState PointClamp : register(s0);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(Source.Sample(PointClamp, uv).rgb * .5, 1);
})");
    Effects().AddPass(owner, "hdr", PostProcessStage::BeforeTonemap, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            events.push_back(1);
            c.SetPixelShader(hdr);
            c.SetRenderTarget(f.outputColor);
            c.DrawFullscreen();
        });
    Effects().AddPass(owner, "ldr", PostProcessStage::AfterTonemap, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            events.push_back(2);
            c.SetPixelShader(half);
            c.SetRenderTarget(f.outputColor);
            c.BindTexture(11, f.inputColor);
            c.DrawFullscreen();
        });
    Effects().AddPass(owner, "ui-effect", PostProcessStage::AfterUI, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            events.push_back(4);
            c.SetPixelShader(half);
            c.SetRenderTarget(f.outputColor);
            c.BindTexture(11, f.inputColor);
            c.DrawFullscreen();
        });
    Pixel beforeOverlay{};
    ASSERT_NO_FATAL_FAILURE(Render([&](RenderingContext& context, const FrameContext&) {
        events.push_back(3);
        ComPtr<ID3D11Texture2D> scene;
        CopyBoundTarget(scene);
        ReadPixel(scene.Get(), beforeOverlay);
        // Deterministic opaque UI surrogate, using exactly the overlay target
        // supplied to the real UI renderer (not the swapchain).
        ComPtr<ID3D11RenderTargetView> target;
        context.GetDeviceContext()->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        ASSERT_NE(target.Get(), nullptr);
        const Pixel ui{0.8f, 0.4f, 0.2f, 1};
        context.GetDeviceContext()->ClearRenderTargetView(target.Get(), ui.data());
    }));
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3, 4}));
    // tone(4, 1, .25) * .5, not tone((4, 1, .25) * .5).
    EXPECT_NEAR(beforeOverlay[0], .4f, .008f);
    EXPECT_NEAR(beforeOverlay[1], .25f, .008f);
    EXPECT_NEAR(beforeOverlay[2], .1f, .008f);
    ExpectImage({.4f, .2f, .1f, 1});
}

TEST_F(PostProcessingNative, CustomTonemappingReplacesReinhardBeforeUiAndSurvivesResize) {
    renderer.GetWorld().environment.settings.fallbackColor = {4, 1, .25f};
    const auto shader = Shader(owner, R"(
Texture2D Source : register(t11); SamplerState PointClamp : register(s0);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(1 - exp(-Source.SampleLevel(PointClamp, uv, 0).rgb), 1);
})");
    const auto mapper = Effects().AddPass(owner, "custom mapper", PostProcessStage::BeforeTonemap, 1000,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            EXPECT_THROW(Effects().SetBuiltinTonemappingEnabled(owner, true), std::invalid_argument);
            c.SetPixelShader(shader);
            c.SetRenderTarget(f.outputColor);
            c.BindTexture(11, f.inputColor);
            c.DrawFullscreen();
        });
    Effects().SetBuiltinTonemappingEnabled(owner, false);
    const Pixel mapped{1 - std::exp(-4.f), 1 - std::exp(-1.f), 1 - std::exp(-.25f), 1};
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectImage(mapped); // A second Reinhard application would darken all three channels.

    captured.Reset();
    width = 111;
    height = 79;
    ASSERT_TRUE(renderer.Reinitialize({window, width, height}));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectImage(mapped);
    ASSERT_NO_FATAL_FAILURE(Render([&](RenderingContext& context, const FrameContext&) {
        ComPtr<ID3D11Texture2D> beforeUi;
        CopyBoundTarget(beforeUi);
        Pixel pixel{};
        ReadPixel(beforeUi.Get(), pixel);
        for (size_t c = 0; c < 4; ++c) EXPECT_NEAR(pixel[c], mapped[c], .012f);
        ComPtr<ID3D11RenderTargetView> target;
        context.GetDeviceContext()->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        const Pixel ui{.2f, .4f, .6f, 1};
        context.GetDeviceContext()->ClearRenderTargetView(target.Get(), ui.data());
    }));
    ExpectImage({.2f, .4f, .6f, 1});
    Effects().SetEnabled(owner, mapper, false);
    Effects().SetBuiltinTonemappingEnabled(owner, true);
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectImage({.8f, .5f, .2f, 1});
}

TEST_F(PostProcessingNative, TonemappingBypassWorksWithoutPassesAndIsScopedToLiveOwners) {
    renderer.GetWorld().environment.settings.fallbackColor = {4, .5f, .25f};
    Effects().SetEnabled(captureOwner, capturePass, false);
    const auto captureDirect = [&](RenderingContext&, const FrameContext&) {
        EXPECT_FALSE(Effects().HasPasses());
        CopyBoundTarget(captured);
    };
    ASSERT_NO_FATAL_FAILURE(Render(captureDirect));
    ExpectImage({.8f, 1.f / 3, .2f, 1});
    const auto other = Effects().CreateOwner();
    Effects().SetBuiltinTonemappingEnabled(owner, false);
    Effects().SetBuiltinTonemappingEnabled(owner, false);
    Effects().SetBuiltinTonemappingEnabled(other, false);
    Effects().SetBuiltinTonemappingEnabled(owner, true);
    Effects().ReleaseOwner(owner); // Does not release the other owner's bypass.
    EXPECT_THROW(Effects().SetBuiltinTonemappingEnabled(owner, false), std::invalid_argument);
    ASSERT_NO_FATAL_FAILURE(Render(captureDirect));
    ExpectImage({1, .5f, .25f, 1}); // UNORM clamps HDR without applying a tone curve.
    Effects().ReleaseOwner(other);
    ASSERT_NO_FATAL_FAILURE(Render(captureDirect));
    ExpectImage({.8f, 1.f / 3, .2f, 1});

    for (const auto view : {RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Depth}) {
        ASSERT_TRUE(renderer.SetDebugView(view));
        ASSERT_NO_FATAL_FAILURE(Render(captureDirect));
        Pixel expected{};
        ReadPixel(captured.Get(), expected);
        Effects().SetBuiltinTonemappingEnabled(captureOwner, false);
        ASSERT_NO_FATAL_FAILURE(Render(captureDirect));
        ExpectImage(expected);
        Effects().SetBuiltinTonemappingEnabled(captureOwner, true);
    }
    Effects().SetBuiltinTonemappingEnabled(captureOwner, false);
    renderer.Shutdown();
    EXPECT_TRUE(Effects().IsBuiltinTonemappingEnabled());
}

TEST_F(PostProcessingNative, RetiringCustomMapperRestoresReinhardInTheSameFrame) {
    renderer.GetWorld().environment.settings.fallbackColor = {4, 1, .25f};
    Effects().SetBuiltinTonemappingEnabled(owner, false);
    Effects().AddPass(owner, "retire mapper", PostProcessStage::BeforeTonemap, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            c.ClearRenderTarget(f.outputColor, {.9f, .9f, .9f, 1});
            Effects().ReleaseOwner(owner);
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_TRUE(Effects().IsBuiltinTonemappingEnabled());
    ExpectImage({.8f, .5f, .2f, 1});
}

TEST_F(PostProcessingNative, PriorityAndEqualPriorityRegistrationOrderChainActualPixels) {
    Seed(owner, PostProcessStage::AfterUI, {.1f, .1f, .1f, 1});
    std::vector<int> events;
    auto add = [&](const char* name, int order, int event, const char* expression) {
        const auto shader = Shader(owner, std::string(R"(
Texture2D Source : register(t11);
SamplerState PointClamp : register(s0);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 rgb = Source.Sample(PointClamp, uv).rgb;
    return float4()") + expression + R"(, 1); })");
        Effects().AddPass(owner, name, PostProcessStage::AfterUI, order,
            [&, shader, event](PostProcessContext& c, const PostProcessFrame& f) {
                events.push_back(event);
                c.SetPixelShader(shader);
                c.SetRenderTarget(f.outputColor);
                c.BindTexture(11, f.inputColor);
                c.DrawFullscreen();
            });
    };
    add("multiply", 10, 2, "rgb * 2");
    add("earlier-priority", -10, 1, "rgb + .1");
    add("equal-add", 10, 3, "rgb + .05");
    add("equal-half", 10, 4, "rgb * .5");
    for (int i = 0; i < 3; ++i) {
        events.clear();
        ASSERT_NO_FATAL_FAILURE(Render());
        EXPECT_EQ(events, (std::vector<int>{1, 2, 3, 4}));
        ExpectImage({.225f, .225f, .225f, 1});
    }
}

TEST_F(PostProcessingNative, HalfResolutionBloomMultipleDrawsAndResizeKeepPublicTargetTokens) {
    Seed(owner, PostProcessStage::BeforeTonemap, {2, .5f, .25f, 1});
    const auto a = Effects().CreateTarget(owner, {.scale = .5f});
    const auto b = Effects().CreateTarget(owner, {.scale = .5f});
    const auto fixed = Effects().CreateTarget(owner, {.width = 7, .height = 5});
    const auto threshold = Shader(owner, R"(
Texture2D Source : register(t9); SamplerState LinearClamp : register(s1);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(max(Source.Sample(LinearClamp, uv).rgb - 1, 0), 1);
})");
    const auto attenuate = Shader(owner, R"(
Texture2D Source : register(t9); SamplerState LinearClamp : register(s1);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(Source.Sample(LinearClamp, uv).rgb * .5, 1);
})");
    const auto combine = Shader(owner, R"(
Texture2D Scene : register(t3); Texture2D Bloom : register(t12);
SamplerState LinearClamp : register(s1);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(Scene.Sample(LinearClamp, uv).rgb + Bloom.Sample(LinearClamp, uv).rgb, 1);
})");
    int calls{};
    const auto pass = Effects().AddPass(owner, "bloom", PostProcessStage::BeforeTonemap, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            ++calls;
            EXPECT_EQ(c.GetTextureSize(a), (std::array<std::uint32_t, 2>{(width + 1) / 2, (height + 1) / 2}));
            EXPECT_EQ(c.GetTextureSize(b), c.GetTextureSize(a));
            EXPECT_EQ(c.GetTextureSize(fixed), (std::array<std::uint32_t, 2>{7, 5}));
            EXPECT_EQ(f.inputWidth, width);
            EXPECT_EQ(f.inputHeight, height);
            EXPECT_EQ(f.outputWidth, width);
            EXPECT_EQ(f.outputHeight, height);
            c.SetPixelShader(threshold);
            c.SetRenderTarget(a);
            c.BindTexture(9, f.inputColor);
            c.DrawFullscreen();
            UINT count = 1;
            D3D11_VIEWPORT viewport{};
            renderer.GetRenderingContext().GetDeviceContext()->RSGetViewports(&count, &viewport);
            EXPECT_FLOAT_EQ(viewport.Width, float((width + 1) / 2));
            EXPECT_FLOAT_EQ(viewport.Height, float((height + 1) / 2));
            c.SetPixelShader(attenuate);
            c.SetRenderTarget(b);
            c.BindTexture(9, a);
            c.DrawFullscreen();
            // Reuse a previously sampled target as an RTV; the backend must
            // detach its SRV instead of relying on D3D's hazard auto-unbind.
            c.ClearRenderTarget(a);
            c.SetRenderTarget(a);
            c.BindTexture(9, b);
            c.DrawFullscreen();
            c.SetPixelShader(combine);
            c.SetRenderTarget(f.outputColor);
            c.BindTexture(3, f.inputColor);
            c.BindTexture(12, b);
            c.DrawFullscreen();
        });
    for (int i = 0; i < 2; ++i) {
        ASSERT_NO_FATAL_FAILURE(Render());
        EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
        ExpectImage({2.5f / 3.5f, 1.0f / 3, .2f, 1});
        if (i == 0) {
            captured.Reset();
            width = 111;
            height = 79;
            ASSERT_TRUE(renderer.Reinitialize({window, width, height}));
        }
    }
    EXPECT_EQ(calls, 2);
}

TEST_F(PostProcessingNative, ExplicitSlotsCustomParametersAndEngineBindingsSurviveStateReset) {
    Seed(owner, PostProcessStage::AfterUI, {.2f, .3f, .4f, 1});
    const auto shader = Shader(owner, R"(
cbuffer Frame : register(b0) { float Time; float3 FramePadding; };
cbuffer View : register(b1) {
    row_major float4x4 V; row_major float4x4 P; row_major float4x4 InvVP;
    float3 Camera; float ViewPadding; float2 Viewport; float2 ViewPadding2;
};
cbuffer Parameters : register(b7) { float Gain; };
Texture2D Source : register(t11); SamplerState PointClamp : register(s0);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(Source.Sample(PointClamp, uv).r * Gain, Time, Viewport.x / 200, 1);
})");
    const auto buffer = Effects().CreateConstantBuffer(owner, shader, "Parameters");
    const auto incompatible = Shader(owner, R"(
cbuffer Other : register(b7) { int DifferentLayout; };
float4 PSMain() : SV_Target { return float4(DifferentLayout, 0, 0, 1); })");
    ASSERT_EQ(Effects().GetParameterFields(owner, buffer).size(), 1u);
    auto gain = [&](double value) {
        const std::array updates{ShaderParameterUpdate{"Gain", {value}}};
        Effects().SetParameters(owner, buffer, updates);
    };
    gain(2);
    frame.simulationTimeSeconds = .375;
    const auto copy = Shader(owner, CopySource);
    Effects().AddPass(owner, "dirty-slots", PostProcessStage::AfterUI, -50,
        [copy](PostProcessContext& c, const PostProcessFrame& f) {
            c.SetPixelShader(copy);
            c.SetRenderTarget(f.outputColor);
            c.BindTexture(11, f.inputColor);
            c.DrawFullscreen();
        });
    const auto pass = Effects().AddPass(owner, "bindings", PostProcessStage::AfterUI, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            auto* dc = renderer.GetRenderingContext().GetDeviceContext();
            for (UINT slot = 0; slot < 5; ++slot) {
                ComPtr<ID3D11SamplerState> sampler;
                dc->PSGetSamplers(slot, 1, sampler.GetAddressOf());
                EXPECT_NE(sampler.Get(), nullptr) << "sampler " << slot;
            }
            c.SetPixelShader(shader);
            c.SetRenderTarget(f.outputColor);
            c.BindConstantBuffer(7, buffer);
            c.SetPixelShader(incompatible);
            EXPECT_THROW(c.DrawFullscreen(), std::invalid_argument);
            c.SetPixelShader(shader);
            EXPECT_THROW(c.BindConstantBuffer(0, buffer), std::invalid_argument);
            EXPECT_THROW(c.BindConstantBuffer(6, buffer), std::invalid_argument);
            EXPECT_THROW(c.BindTexture(128, f.inputColor), std::invalid_argument);
            EXPECT_THROW(c.BindTexture(11, f.outputColor), std::invalid_argument);
            // A prior pass bound t11; this callback must not inherit that binding.
            EXPECT_THROW(c.DrawFullscreen(), std::invalid_argument);
            c.BindTexture(11, f.inputColor);
            c.DrawFullscreen();
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
    ExpectImage({.4f, .375f, float(width) / 200, 1});
    gain(3);
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectImage({.6f, .375f, float(width) / 200, 1});
}

TEST_F(PostProcessingNative, ThrowingCallbackDiscardsPartialOutputDisablesAndCanBeReenabled) {
    Seed(owner, PostProcessStage::AfterUI, {.2f, .3f, .4f, 1});
    int calls{};
    bool fail = true;
    const auto pass = Effects().AddPass(owner, "failure", PostProcessStage::AfterUI, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            ++calls;
            c.ClearRenderTarget(f.outputColor, {.6f, .1f, .2f, 1});
            if (fail) throw std::runtime_error("intentional regression probe");
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectImage({.2f, .3f, .4f, 1});
    EXPECT_NE(Effects().GetPassError(owner, pass).find("intentional regression probe"), std::string::npos);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(calls, 1);
    ExpectImage({.2f, .3f, .4f, 1});
    fail = false;
    Effects().SetEnabled(owner, pass, true);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
    ExpectImage({.6f, .1f, .2f, 1});
    Effects().SetEnabled(owner, pass, false);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(calls, 2);
    ExpectImage({.2f, .3f, .4f, 1});
}

TEST_F(PostProcessingNative, BorrowedFrameTexturesExpireAndImportedSnapshotsSurviveAssetUnload) {
    std::optional<PostProcessContext> saved;
    PostProcessTexture borrowed{};
    auto& manager = renderer.GetResourceManager();
    const auto asset = manager.CreateTexture({.width = 4, .height = 4,
        .format = DXGI_FORMAT_R8G8B8A8_UNORM, .usage = TextureUsage::ShaderResource});
    const auto imported = Effects().ImportTexture(owner, asset);
    ASSERT_TRUE(manager.DestroyTexture(asset));
    Effects().AddPass(owner, "remember", PostProcessStage::AfterUI, -10,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            if (borrowed.value) EXPECT_THROW(c.GetTextureSize(borrowed), std::invalid_argument);
            borrowed = f.inputColor;
            saved = c;
            EXPECT_EQ(c.GetTextureSize(imported), (std::array<std::uint32_t, 2>{4, 4}));
            EXPECT_THROW(c.SetRenderTarget(imported), std::invalid_argument);
            EXPECT_THROW(c.SetRenderTarget(f.inputColor), std::invalid_argument);
            EXPECT_EQ(c.GetTextureSize(f.depth), (std::array<std::uint32_t, 2>{width, height}));
            EXPECT_EQ(c.GetTextureSize(f.normals), (std::array<std::uint32_t, 2>{width, height}));
            c.ClearRenderTarget(f.outputColor, {.1f, .2f, .3f, 1});
        });
    Effects().AddPass(owner, "different-execution", PostProcessStage::AfterUI, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            EXPECT_THROW(c.BindTexture(11, borrowed), std::invalid_argument);
            EXPECT_THROW(saved->GetTextureSize(borrowed), std::invalid_argument);
            c.ClearRenderTarget(f.outputColor, {.2f, .4f, .6f, 1});
        });
    for (int i = 0; i < 2; ++i) {
        ASSERT_NO_FATAL_FAILURE(Render());
        ASSERT_TRUE(saved.has_value());
        EXPECT_THROW(saved->DrawFullscreen(), std::invalid_argument);
        EXPECT_THROW(saved->GetTextureSize(borrowed), std::invalid_argument);
        ExpectImage({.2f, .4f, .6f, 1});
    }
}

TEST_F(PostProcessingNative, OwnersCannotShareCapabilitiesAndRevocationImmediatelySkipsScheduledWork) {
    const auto other = Effects().CreateOwner();
    Seed(other, PostProcessStage::AfterUI, {.15f, .25f, .35f, 1});
    const auto target = Effects().CreateTarget(owner, {});
    const auto shader = Shader(owner, R"(
cbuffer Parameters : register(b7) { float Gain; };
float4 PSMain(float4 p : SV_POSITION) : SV_Target { return float4(Gain, 0, 0, 1); })");
    const auto buffer = Effects().CreateConstantBuffer(owner, shader, "Parameters");
    auto& manager = renderer.GetResourceManager();
    const auto asset = manager.CreateTexture({.width = 4, .height = 4,
        .format = DXGI_FORMAT_R8G8B8A8_UNORM, .usage = TextureUsage::ShaderResource});
    const auto imported = Effects().ImportTexture(owner, asset);
    EXPECT_THROW(Effects().CreateConstantBuffer(other, shader, "Parameters"), std::invalid_argument);
    EXPECT_THROW(Effects().GetParameterFields(other, buffer), std::invalid_argument);
    const std::array updates{ShaderParameterUpdate{"Gain", {0.5}}};
    EXPECT_THROW(Effects().SetParameters(other, buffer, updates), std::invalid_argument);
    int foreignCalls{}, revokedCalls{}, laterCalls{};
    Effects().AddPass(other, "foreign", PostProcessStage::AfterUI, -20,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            ++foreignCalls;
            EXPECT_THROW(c.SetPixelShader(shader), std::invalid_argument);
            EXPECT_THROW(c.BindConstantBuffer(7, buffer), std::invalid_argument);
            EXPECT_THROW(c.BindTexture(11, target), std::invalid_argument);
            EXPECT_THROW(c.ClearRenderTarget(target), std::invalid_argument);
            c.ClearRenderTarget(f.outputColor, {.15f, .25f, .35f, 1});
        });
    const auto revoke = Effects().AddPass(owner, "revoke", PostProcessStage::AfterUI, 0,
        [&](PostProcessContext& c, const PostProcessFrame& f) {
            ++revokedCalls;
            EXPECT_EQ(c.GetTextureSize(imported), (std::array<std::uint32_t, 2>{4, 4}));
            EXPECT_THROW(c.SetRenderTarget(imported), std::invalid_argument);
            c.ClearRenderTarget(f.outputColor, {1, 0, 0, 1});
            Effects().ReleaseOwner(owner);
            EXPECT_FALSE(Effects().IsOwnerAlive(owner));
            EXPECT_THROW(c.ClearRenderTarget(f.outputColor), std::invalid_argument);
        });
    Effects().AddPass(owner, "must-not-run", PostProcessStage::AfterUI, 1,
        [&](PostProcessContext&, const PostProcessFrame&) { ++laterCalls; });
    EXPECT_THROW(Effects().SetEnabled(other, revoke, false), std::invalid_argument);
    EXPECT_THROW(Effects().RemovePass(other, revoke), std::invalid_argument);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(revokedCalls, 1);
    EXPECT_EQ(laterCalls, 0);
    ExpectImage({.15f, .25f, .35f, 1});
    EXPECT_NE(manager.TryGetTexture(asset), nullptr); // Imported assets are borrowed, never destroyed by retirement.
    EXPECT_THROW(Effects().CreateTarget(owner, {}), std::invalid_argument);
    EXPECT_THROW(Effects().SetParameters(owner, buffer, updates), std::invalid_argument);
    Effects().ReleaseOwner(owner); // Idempotent retirement.
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(revokedCalls, 1);
    EXPECT_EQ(foreignCalls, 2);
    EXPECT_EQ(laterCalls, 0);
    ExpectImage({.15f, .25f, .35f, 1});
    Effects().ReleaseOwner(other);
    EXPECT_TRUE(manager.DestroyTexture(asset));
}
} // namespace
