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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace engine;
using Pixel = std::array<float, 4>;

// Two triangles, not a fullscreen triangle: opposite winding intentionally
// verifies that procedural geometry is uncullled. No input layout/VB exists.
const std::string QuadVertex = R"(
struct Out { float4 p : SV_POSITION; float2 uv : TEXCOORD0; };
Out VSMain(uint v : SV_VertexID) {
    float2 corners[6] = {
        float2(-1,-1), float2(-1,1), float2(1,-1),
        float2(1,-1), float2(1,1), float2(-1,1)};
    Out o; o.p = float4(corners[v], DEPTH, 1);
    o.uv = corners[v] * .5 + .5; return o;
})";

struct Image {
    UINT width{}, height{};
    std::vector<Pixel> pixels;
    const Pixel& At(UINT x, UINT y) const { return pixels.at(size_t(y) * width + x); }
    const Pixel& Center() const { return At(width / 2, height / 2); }
};

void Read(RenderingContext& context, TextureHandle handle, Image& image) {
    const auto* resource = context.GetResourceManager().TryGetTexture(handle);
    ASSERT_NE(resource, nullptr);
    auto* source = resource->GetTexture();
    ASSERT_NE(source, nullptr);
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const bool depth = desc.Format == DXGI_FORMAT_R32_TYPELESS;
    ASSERT_TRUE(depth || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    image.width = desc.Width;
    image.height = desc.Height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc, nullptr, &staging));
    auto* dc = context.GetDeviceContext();
    dc->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_HRESULT_SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    image.pixels.resize(size_t(image.width) * image.height);
    for (UINT y = 0; y < image.height; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = 0; x < image.width; ++x) {
            Pixel p{0, 0, 0, 1};
            if (depth) std::memcpy(p.data(), row + x * 4, 4);
            else {
                std::array<std::uint16_t, 4> half{};
                std::memcpy(half.data(), row + x * 8, 8);
                for (size_t c = 0; c < 4; ++c)
                    p[c] = DirectX::PackedVector::XMConvertHalfToFloat(half[c]);
            }
            image.pixels[size_t(y) * image.width + x] = p;
        }
    }
    dc->Unmap(staging.Get(), 0);
}

void ExpectRgb(const Pixel& actual, const Pixel& expected) {
    for (size_t c = 0; c < 3; ++c) EXPECT_NEAR(actual[c], expected[c], .006f) << "channel " << c;
}

class ProceduralNative : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    HWND window{};
    RenderingSystem renderer;
    PostProcessOwner owner{};
    FrameContext frame{};
    UINT width = 96, height = 72;
    unsigned drawPassIndex{};
    Image hdr, depth;
    ComPtr<ID3D11InfoQueue> messages;

    PostProcessing& Effects() { return renderer.GetPostProcessing(); }
    CompiledShader Compile(const std::string& source, ShaderType type) {
        return ShaderContext::CompileSource(source,
            std::filesystem::path(SRI_SOURCE_ROOT) / "tests/procedural_probe.hlsl", type);
    }
    PostProcessPipeline Pipeline(const std::string& pixel, float z = .5f,
        ProceduralBlend blend = ProceduralBlend::Opaque,
        ProceduralDepth depthMode = ProceduralDepth::ReadWrite) {
        auto vertex = QuadVertex;
        vertex.replace(vertex.find("DEPTH"), 5, std::to_string(z));
        return Effects().CreatePipeline(owner, Compile(vertex, ShaderType::Vertex),
            Compile(pixel, ShaderType::Pixel), blend, depthMode);
    }
    PostProcessPipeline Solid(const std::string& color, float z = .5f,
        ProceduralBlend blend = ProceduralBlend::Opaque,
        ProceduralDepth depthMode = ProceduralDepth::ReadWrite) {
        return Pipeline("float4 PSMain() : SV_Target { return float4(" + color + "); }",
                        z, blend, depthMode);
    }
    PostProcessPass Draw(PostProcessPipeline pipeline, int order = 0) {
        return Effects().AddProceduralPass(owner, "draw-" + std::to_string(++drawPassIndex), order,
            [pipeline](ProceduralContext& c, const ProceduralFrame&) {
                c.SetPipeline(pipeline);
                c.DrawProcedural(6);
            });
    }
    void SetUp() override {
        AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "tests");
        window = CreateWindowExW(0, L"STATIC", L"Procedural GPU regression", WS_OVERLAPPEDWINDOW,
            0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        const bool initialized = renderer.Initialize({window, width, height}, true);
        const auto diagnostics = renderer.GetDiagnostics();
        if (!initialized && diagnostics.failure &&
            diagnostics.failure->operation == "D3D11CreateDeviceAndSwapChain")
            GTEST_SKIP() << diagnostics.failure->summary;
        ASSERT_TRUE(initialized) << (diagnostics.failure ? diagnostics.failure->detail : "");
        auto& world = renderer.GetWorld();
        world.camera = FPSCamera{.8f, float(width) / height, .1f, 100};
        world.camera.position = {0, 0, 0};
        world.directionalLight = {{0, 0, 0}, {0, -1, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};
        ASSERT_HRESULT_SUCCEEDED(renderer.GetRenderingContext().GetDevice()->QueryInterface(
            IID_PPV_ARGS(messages.GetAddressOf())));
        messages->ClearStoredMessages();
        owner = Effects().CreateOwner();
    }
    void CheckMessages() {
        if (!messages) return;
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{};
            ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(i, nullptr, &size));
            std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
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
        renderer.Shutdown();
        CheckMessages();
        messages.Reset();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
    }
    void Render(double dt = 0) {
        ++frame.frameIndex;
        frame.deltaSeconds = frame.realDeltaSeconds = dt;
        frame.simulationTimeSeconds += dt;
        bool captured = false;
        renderer.RenderFrame(frame, [&](RenderingContext& c, const FrameContext&) {
            Read(c, renderer.GetResources().gBuffer.resultTexture, hdr);
            Read(c, renderer.GetResources().gBuffer.depthTexture, depth);
            captured = true;
        });
        const auto diagnostics = renderer.GetDiagnostics();
        ASSERT_EQ(diagnostics.lifecycle, RenderingLifecycle::Ready)
            << (diagnostics.failure ? diagnostics.failure->detail : "");
        ASSERT_TRUE(captured);
        CheckMessages();
    }
};

TEST_F(ProceduralNative, InstancedSixVertexQuadsUseReflectedVertexDataAndDefaultNativeState) {
    const auto vertex = Compile(R"(
cbuffer Instances : register(b5) { float4 Rect[2]; float4 Color[2]; };
struct Out { float4 p : SV_POSITION; float4 color : COLOR0; };
Out VSMain(uint v : SV_VertexID, uint instance : SV_InstanceID) {
    float2 corners[6] = {float2(-1,-1),float2(-1,1),float2(1,-1),
                        float2(1,-1),float2(1,1),float2(-1,1)};
    Out o; o.p = float4(corners[v] * Rect[instance].zw + Rect[instance].xy, .4, 1);
    o.color = Color[instance]; return o;
})", ShaderType::Vertex);
    const auto pixel = Compile(
        "float4 PSMain(float4 p : SV_POSITION, float4 c : COLOR0) : SV_Target { return c; }",
        ShaderType::Pixel);
    const auto pipeline = Effects().CreatePipeline(owner, vertex, pixel);
    const auto shaders = Effects().GetPipelineShaders(owner, pipeline);
    const std::array initial{
        ShaderParameterUpdate{"Rect", {-.5, 0., .4, .8, .5, 0., .4, .8}},
        ShaderParameterUpdate{"Color", {4., 0., 0., 1., 0., 2., 0., 1.}}};
    const auto buffer = Effects().CreateConstantBuffer(owner, shaders[0], "Instances", initial);
    const auto pass = Effects().AddProceduralPass(owner, "instances", 0,
        [&](ProceduralContext& c, const ProceduralFrame& f) {
            EXPECT_EQ(f.width, width);
            EXPECT_EQ(f.height, height);
            EXPECT_FALSE(Effects().HasPasses()) << "Procedural-only must not activate fullscreen composition";
            // Empty draws are legal no-ops even before choosing a pipeline.
            c.DrawProcedural(0);
            c.DrawProcedural(6, 0);
            c.SetPipeline(pipeline);
            c.BindConstantBuffer(5, buffer);
            EXPECT_THROW(c.DrawProcedural(1), std::invalid_argument);
            EXPECT_THROW(c.DrawProcedural(4, 2), std::invalid_argument);
            c.DrawProcedural(6, 2);
            auto* dc = renderer.GetRenderingContext().GetDeviceContext();
            ComPtr<ID3D11InputLayout> layout;
            dc->IAGetInputLayout(&layout);
            EXPECT_EQ(layout.Get(), nullptr);
            ComPtr<ID3D11Buffer> vb;
            UINT stride{}, offset{};
            dc->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
            EXPECT_EQ(vb.Get(), nullptr);
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            dc->IAGetPrimitiveTopology(&topology);
            EXPECT_EQ(topology, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ComPtr<ID3D11RasterizerState> raster;
            dc->RSGetState(&raster);
            ASSERT_NE(raster.Get(), nullptr);
            D3D11_RASTERIZER_DESC rs{};
            raster->GetDesc(&rs);
            EXPECT_EQ(rs.CullMode, D3D11_CULL_NONE);
            ComPtr<ID3D11DepthStencilState> ds;
            dc->OMGetDepthStencilState(&ds, nullptr);
            ASSERT_NE(ds.Get(), nullptr);
            D3D11_DEPTH_STENCIL_DESC desc{};
            ds->GetDesc(&desc);
            EXPECT_TRUE(desc.DepthEnable);
            EXPECT_EQ(desc.DepthWriteMask, D3D11_DEPTH_WRITE_MASK_ALL);
            EXPECT_EQ(desc.DepthFunc, D3D11_COMPARISON_GREATER_EQUAL);
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
    ExpectRgb(hdr.At(width / 4, height / 4), {4, 0, 0, 1});
    ExpectRgb(hdr.At(3 * width / 4, 3 * height / 4), {0, 2, 0, 1});
    EXPECT_NEAR(depth.At(width / 4, height / 4)[0], .4f, .00001f);
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
}

TEST_F(ProceduralNative, ReverseZReadOnlyAndDisabledAreDistinctFromWritingDepth) {
    const auto seed = Draw(Solid("1,0,0,1", .6f), -10);
    const auto read = Draw(Solid("0,1,0,1", .8f, ProceduralBlend::Opaque, ProceduralDepth::ReadOnly));
    const auto behind = Draw(Solid("0,0,1,1", .7f), 10);
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {0, 0, 1, 1}); // .8 read-only did not occlude .7.
    EXPECT_NEAR(depth.Center()[0], .7f, .00001f);
    Effects().SetEnabled(owner, behind, false);
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {0, 1, 0, 1});
    EXPECT_NEAR(depth.Center()[0], .6f, .00001f);
    Effects().SetEnabled(owner, read, false);
    const auto failedDepth = Draw(Solid("0,0,1,1", .2f), 5);
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {1, 0, 0, 1});
    Effects().RemovePass(owner, failedDepth);
    Draw(Solid("0,0,1,1", .2f, ProceduralBlend::Opaque, ProceduralDepth::Disabled), 5);
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {0, 0, 1, 1});
    EXPECT_NEAR(depth.Center()[0], .6f, .00001f);
    Effects().SetEnabled(owner, seed, false);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
}

TEST_F(ProceduralNative, StraightAlphaAndAdditiveBlendHdrWithoutWritingDisabledDepth) {
    renderer.GetWorld().environment.settings.fallbackColor = {2, 1, .5f};
    const auto alpha = Draw(Solid("4,2,1,.25", .5f, ProceduralBlend::Alpha, ProceduralDepth::Disabled));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {2.5f, 1.25f, .625f, 1});
    Effects().RemovePass(owner, alpha);
    Draw(Solid("4,2,1,.25", .5f, ProceduralBlend::Additive, ProceduralDepth::Disabled));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {6, 3, 1.5f, 1}); // Native additive is One + One, independent of source alpha.
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
}

TEST_F(ProceduralNative, ClipHolesPreserveSkyAndAllowBehindParticlesWhileFrontParticlesRemainVisible) {
    renderer.GetWorld().environment.settings.fallbackColor = {0, 2, 1};
    // The near half-plane is opaque except for a center hole.
    const auto mask = Pipeline(R"(
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    clip(abs(uv.x - .5) - .10); return float4(0,0,0,1);
})", .1f);
    Draw(mask);
    ASSERT_TRUE(renderer.SetParticleBudget({.maxParticles = 16, .maxEmitters = 2}));
    RenderWorld::EmitterSource source{
        .owner = 42, .settings = {.appearance = 2, .maxAlive = 1, .rate = 4, .lifetime = 20,
            .speed = 0, .radius = .5f, .spread = 0, .tint = {4, 0, 0}},
        .position = {0, 0, -3}, .direction = {0, 1, 0}};
    source.effect.fadeIn = source.effect.softDistance = 0;
    source.effect.opacityStart = source.effect.opacityEnd = .5f;
    source.effect.sizeStart = source.effect.sizeEnd = 4;
    renderer.GetWorld().particleEmitters.push_back(source);
    ASSERT_NO_FATAL_FAILURE(Render(.25));
    EXPECT_GT(hdr.Center()[0], .5f);
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
    const UINT side = width / 3;
    EXPECT_NEAR(depth.At(side, height / 2)[0], .1f, .00001f);
    ExpectRgb(hdr.At(side, height / 2), {0, 0, 0, 1});
    // Move the same large billboard in front of the mask (reverse-Z > .1).
    renderer.ResetParticles();
    source.position = {0, 0, -.5f};
    renderer.GetWorld().particleEmitters.push_back(source);
    ASSERT_NO_FATAL_FAILURE(Render(.25));
    EXPECT_GT(hdr.At(side, height / 2)[0], .5f);
    EXPECT_NEAR(depth.At(side, height / 2)[0], .1f, .00001f); // Particles remain read-only.
}

TEST_F(ProceduralNative, EngineFrameViewAndAllFiveSamplersAreBoundInBothStages) {
    const std::string shared = R"(
cbuffer Frame : register(b0) { float Time; float3 FramePadding; };
cbuffer View : register(b1) {
    row_major float4x4 V; row_major float4x4 P; row_major float4x4 InvVP;
    float3 Camera; float ViewPadding; float2 Viewport; float2 ViewPadding2;
};
)";
    auto vertex = QuadVertex;
    vertex.replace(vertex.find("DEPTH"), 5, ".5");
    // Make VS and PS actually consume engine data, not just inspect nonnull slots.
    vertex.replace(vertex.find("return o;"), 9, "o.uv = float2(Time, Viewport.x / 200); return o;");
    const auto pipeline = Effects().CreatePipeline(owner, Compile(shared + vertex, ShaderType::Vertex),
        Compile(shared + R"(
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(uv.x + Time, uv.y, Viewport.y / 200, 1);
})", ShaderType::Pixel));
    frame.simulationTimeSeconds = .375;
    const auto pass = Effects().AddProceduralPass(owner, "engine bindings", 0,
        [&](ProceduralContext& c, const ProceduralFrame&) {
            c.SetPipeline(pipeline);
            c.DrawProcedural(6);
            auto* dc = renderer.GetRenderingContext().GetDeviceContext();
            for (bool vs : {true, false}) {
                for (UINT slot = 0; slot < 5; ++slot) {
                    ComPtr<ID3D11SamplerState> sampler;
                    if (vs) dc->VSGetSamplers(slot, 1, &sampler);
                    else dc->PSGetSamplers(slot, 1, &sampler);
                    EXPECT_NE(sampler.Get(), nullptr) << "VS=" << vs << " s" << slot;
                }
                for (UINT slot = 0; slot < 2; ++slot) {
                    ComPtr<ID3D11Buffer> buffer;
                    if (vs) dc->VSGetConstantBuffers(slot, 1, &buffer);
                    else dc->PSGetConstantBuffers(slot, 1, &buffer);
                    ASSERT_NE(buffer.Get(), nullptr);
                    D3D11_BUFFER_DESC desc{};
                    buffer->GetDesc(&desc);
                    EXPECT_EQ(desc.ByteWidth, slot ? sizeof(ViewBufferData) : sizeof(FrameBufferData));
                }
            }
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
    ExpectRgb(hdr.Center(), {.75f, float(width) / 200, float(height) / 200, 1});
}

TEST_F(ProceduralNative, VertexAndPixelResourcesAreExplicitResetPerPassAndOwnerScoped) {
    const Pixel texel{.25f, .5f, .75f, 1};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{texel.data(), sizeof(Pixel), sizeof(Pixel)};
    ComPtr<ID3D11Texture2D> native;
    auto& rc = renderer.GetRenderingContext();
    ASSERT_HRESULT_SUCCEEDED(rc.GetDevice()->CreateTexture2D(&desc, &data, &native));
    ComPtr<ID3D11ShaderResourceView> srv;
    ASSERT_HRESULT_SUCCEEDED(rc.GetDevice()->CreateShaderResourceView(native.Get(), nullptr, &srv));
    const auto asset = renderer.GetResourceManager().RegisterTexture(Texture(std::move(srv)));
    const auto texture = Effects().ImportTexture(owner, asset);
    ASSERT_TRUE(renderer.GetResourceManager().DestroyTexture(asset));
    auto vertex = QuadVertex;
    vertex.replace(vertex.find("DEPTH"), 5, ".5");
    vertex.replace(vertex.find("return o;"), 9,
        "o.uv = Source.SampleLevel(PointClamp, float2(.5,.5), 0).rg * Gain; return o;");
    vertex = R"(
cbuffer VertexParameters : register(b5) { float Gain; };
Texture2D Source : register(t7); SamplerState PointClamp : register(s0);
)" + vertex;
    const auto pipeline = Effects().CreatePipeline(owner, Compile(vertex, ShaderType::Vertex),
        Compile(R"(
cbuffer PixelParameters : register(b6) { float Gain; };
Texture2D Source : register(t9); SamplerState LinearClamp : register(s1);
float4 PSMain(float4 p : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return float4(uv * Gain, Source.Sample(LinearClamp, float2(.5,.5)).b * Gain, 1);
})", ShaderType::Pixel));
    const auto shaders = Effects().GetPipelineShaders(owner, pipeline);
    const std::array initial{ShaderParameterUpdate{"Gain", {2.}}};
    const auto vsBuffer = Effects().CreateConstantBuffer(owner, shaders[0], "VertexParameters", initial);
    const auto psBuffer = Effects().CreateConstantBuffer(owner, shaders[1], "PixelParameters", initial);
    const auto incompatible = Effects().CreatePipeline(owner,
        Compile(R"(
cbuffer Different : register(b5) { int Wrong; };
float4 VSMain(uint v : SV_VertexID) : SV_POSITION {
    return float4(v + Wrong, 0, .5, 1);
})", ShaderType::Vertex),
        Compile("float4 PSMain() : SV_Target { return 1; }", ShaderType::Pixel));
    const auto badBuffer = Effects().CreateConstantBuffer(owner,
        Effects().GetPipelineShaders(owner, incompatible)[0], "Different");
    const auto foreignOwner = Effects().CreateOwner();
    const auto foreignPipeline = Effects().CreatePipeline(foreignOwner,
        Compile("float4 VSMain(uint v : SV_VertexID) : SV_POSITION { return float4(v,0,0,1); }",
                ShaderType::Vertex),
        Compile("float4 PSMain() : SV_Target { return 1; }", ShaderType::Pixel));
    const auto foreignTexture = Effects().CreateTarget(foreignOwner, {.width = 2, .height = 2});
    EXPECT_THROW(Effects().GetPipelineShaders(foreignOwner, pipeline), std::invalid_argument);
    EXPECT_THROW(Effects().CreateConstantBuffer(foreignOwner, shaders[0], "VertexParameters"),
                 std::invalid_argument);
    std::optional<ProceduralContext> previous;
    auto callback = [&](ProceduralContext& c, const ProceduralFrame&) {
        if (previous) EXPECT_THROW(previous->DrawProcedural(6), std::invalid_argument);
        EXPECT_THROW(c.SetPipeline(foreignPipeline), std::invalid_argument);
        EXPECT_THROW(c.BindTexture(9, foreignTexture), std::invalid_argument);
        c.SetPipeline(pipeline);
        EXPECT_EQ(c.GetTextureSize(texture), (std::array<std::uint32_t, 2>{1, 1}));
        auto* dc = rc.GetDeviceContext();
        for (bool vs : {true, false}) {
            ComPtr<ID3D11ShaderResourceView> bound;
            if (vs) dc->VSGetShaderResources(7, 1, &bound);
            else dc->PSGetShaderResources(9, 1, &bound);
            EXPECT_EQ(bound.Get(), nullptr) << "Must not inherit preceding pass SRVs";
        }
        EXPECT_THROW(c.DrawProcedural(6), std::invalid_argument);
        EXPECT_THROW(c.BindConstantBuffer(0, vsBuffer), std::invalid_argument);
        EXPECT_THROW(c.BindConstantBuffer(6, vsBuffer), std::invalid_argument);
        EXPECT_THROW(c.BindTexture(128, texture), std::invalid_argument);
        EXPECT_THROW(c.BindTexture(7, texture, ShaderStage::Geometry), std::invalid_argument);
        c.BindTexture(7, texture, ShaderStage::Vertex);
        c.BindTexture(9, texture); // Default is Pixel.
        c.BindConstantBuffer(5, badBuffer);
        c.BindConstantBuffer(6, psBuffer);
        EXPECT_THROW(c.DrawProcedural(6), std::invalid_argument);
        c.BindConstantBuffer(5, vsBuffer);
        c.DrawProcedural(6);
        previous = c;
    };
    const auto first = Effects().AddProceduralPass(owner, "first bindings", 0, callback);
    const auto second = Effects().AddProceduralPass(owner, "reset bindings", 1, callback);
    for (int i = 0; i < 2; ++i) {
        ASSERT_NO_FATAL_FAILURE(Render());
        EXPECT_TRUE(Effects().GetPassError(owner, first).empty());
        EXPECT_TRUE(Effects().GetPassError(owner, second).empty());
        ExpectRgb(hdr.Center(), {1, 2, 1.5f, 1});
    }
    ASSERT_TRUE(previous.has_value());
    EXPECT_THROW(previous->GetTextureSize(texture), std::invalid_argument);
}

TEST_F(ProceduralNative, PipelineRejectsVertexStreamsReservedBuffersAndInvalidStageLinkage) {
    const auto pixel = Compile("float4 PSMain() : SV_Target { return 1; }", ShaderType::Pixel);
    EXPECT_THROW(Effects().CreatePipeline(owner,
        Compile("float4 VSMain(float3 p : POSITION) : SV_POSITION { return float4(p,1); }",
                ShaderType::Vertex), pixel), std::invalid_argument);
    for (int slot : {2, 3, 4}) {
        const auto vertex = Compile("cbuffer Forbidden : register(b" + std::to_string(slot) +
            R"() { float4 Value; };
float4 VSMain(uint v : SV_VertexID) : SV_POSITION { return Value + float4(v,0,0,0); })",
            ShaderType::Vertex);
        EXPECT_THROW(Effects().CreatePipeline(owner, vertex, pixel), std::invalid_argument);
    }
    auto vertex = QuadVertex;
    vertex.replace(vertex.find("DEPTH"), 5, ".5");
    EXPECT_THROW(Effects().CreatePipeline(owner, Compile(vertex, ShaderType::Vertex),
        Compile(R"(
float4 PSMain(float4 p : SV_POSITION, float4 absent : COLOR3) : SV_Target { return absent; }
)", ShaderType::Pixel)), std::invalid_argument);
    EXPECT_THROW(Effects().CreatePipeline(owner, pixel, Compile(vertex, ShaderType::Vertex)),
                 std::invalid_argument);
}

TEST_F(ProceduralNative, ReflectedTextureTypesAndResourceIdentityProtectSceneAttachments) {
    const auto pipeline = Pipeline(R"(
Texture2D<uint4> Values : register(t5);
float4 PSMain() : SV_Target { return float4(Values.Load(int3(0,0,0))); }
)");
    auto& manager = renderer.GetResourceManager();
    const std::array<std::uint32_t, 4> texel{2, 3, 4, 1};
    const std::array data{D3D11_SUBRESOURCE_DATA{texel.data(), sizeof(texel), sizeof(texel)}};
    const auto uintAsset = manager.CreateTexture(
        {.format = DXGI_FORMAT_R32G32B32A32_UINT}, data);
    const auto uintTexture = Effects().ImportTexture(owner, uintAsset);
    const auto unormAsset = manager.CreateTexture({});
    const auto unorm = Effects().ImportTexture(owner, unormAsset);
    const auto floatTarget = Effects().CreateTarget(owner, {.width = 1, .height = 1});
    const auto arrayAsset = manager.CreateTexture(
        {.arraySize = 2, .format = DXGI_FORMAT_R32G32B32A32_UINT});
    const auto arrayTexture = Effects().ImportTexture(owner, arrayAsset);
    // ImportTexture retains separate SRV-backed registrations: their handles
    // differ from the scene attachments, but their underlying resources alias.
    const auto sceneColor = Effects().ImportTexture(owner, renderer.GetResources().gBuffer.resultTexture);
    const auto sceneDepth = Effects().ImportTexture(owner, renderer.GetResources().gBuffer.depthTexture);
    const auto pass = Effects().AddProceduralPass(owner, "typed textures and aliases", 0,
        [&](ProceduralContext& c, const ProceduralFrame&) {
            c.SetPipeline(pipeline);
            EXPECT_THROW(c.BindTexture(5, sceneColor), std::invalid_argument);
            EXPECT_THROW(c.BindTexture(5, sceneDepth), std::invalid_argument);
            EXPECT_THROW(c.BindTexture(5, sceneColor, ShaderStage::Vertex), std::invalid_argument);
            EXPECT_THROW(c.BindTexture(5, sceneDepth, ShaderStage::Vertex), std::invalid_argument);
            for (auto incompatible : {unorm, floatTarget, arrayTexture}) {
                c.BindTexture(5, incompatible);
                EXPECT_THROW(c.DrawProcedural(6), std::invalid_argument);
            }
            c.BindTexture(5, uintTexture);
            c.DrawProcedural(6);
        });
    // Reject layouts that would reinterpret automatic engine buffers, rather
    // than accidentally accepting them just because b0/b1 are reserved slots.
    const auto pixel = Compile("float4 PSMain() : SV_Target { return 1; }", ShaderType::Pixel);
    EXPECT_THROW(Effects().CreatePipeline(owner,
        Compile(R"(
cbuffer InvalidFrame : register(b0) { int Time; };
float4 VSMain(uint v : SV_VertexID) : SV_POSITION { return float4(v, Time, .5, 1); }
)", ShaderType::Vertex), pixel), std::invalid_argument);
    EXPECT_THROW(Effects().CreatePipeline(owner,
        Compile(R"(
cbuffer InvalidView : register(b1) { column_major float4x4 View; };
float4 VSMain(uint v : SV_VertexID) : SV_POSITION { return mul(View, float4(v, 0, .5, 1)); }
)", ShaderType::Vertex), pixel), std::invalid_argument);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
    ExpectRgb(hdr.Center(), {2, 3, 4, 1});
    EXPECT_TRUE(manager.DestroyTexture(uintAsset));
    EXPECT_TRUE(manager.DestroyTexture(unormAsset));
    EXPECT_TRUE(manager.DestroyTexture(arrayAsset));
}

TEST_F(ProceduralNative, OrderTiesEnableRemoveAndRetirementCancelScheduledCallbacks) {
    std::vector<int> events;
    const auto pipeline = Solid("2,0,0,1");
    auto add = [&](int order, int event) {
        return Effects().AddProceduralPass(owner, "ordered-" + std::to_string(event), order,
            [&, event](ProceduralContext& c, const ProceduralFrame&) {
                events.push_back(event);
                c.SetPipeline(pipeline);
                c.DrawProcedural(6);
            });
    };
    const auto a = add(5, 2);
    const auto b = add(-5, 1);
    const auto d = add(5, 3);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
    events.clear();
    Effects().SetEnabled(owner, a, false);
    Effects().RemovePass(owner, d);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(events, (std::vector<int>{1}));
    Effects().SetEnabled(owner, a, true);
    Effects().RemovePass(owner, b);
    const auto other = Effects().CreateOwner();
    Effects().AddProceduralPass(other, "retire owner", -100,
        [&](ProceduralContext&, const ProceduralFrame&) {
            Effects().ReleaseOwner(owner);
            EXPECT_FALSE(Effects().IsOwnerAlive(owner));
        });
    events.clear();
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_TRUE(events.empty());
    EXPECT_THROW(Effects().GetPipelineShaders(owner, pipeline), std::invalid_argument);
    ExpectRgb(hdr.Center(), {0, 0, 0, 1});
}

TEST_F(ProceduralNative, CallbackFailureRetainsIssuedColorAndDepthDisablesPassAndExpiresContext) {
    const auto pipeline = Solid("4,1,.25,1", .6f);
    std::optional<ProceduralContext> saved;
    int calls{};
    bool fail = true;
    const auto pass = Effects().AddProceduralPass(owner, "failure", 0,
        [&](ProceduralContext& c, const ProceduralFrame&) {
            ++calls;
            saved = c;
            c.SetPipeline(pipeline);
            c.DrawProcedural(6);
            if (fail) throw std::runtime_error("procedural failure probe");
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectRgb(hdr.Center(), {4, 1, .25f, 1});
    EXPECT_NEAR(depth.Center()[0], .6f, .00001f);
    EXPECT_NE(Effects().GetPassError(owner, pass).find("procedural failure probe"), std::string::npos);
    ASSERT_TRUE(saved.has_value());
    EXPECT_THROW(saved->SetPipeline(pipeline), std::invalid_argument);
    EXPECT_THROW(saved->DrawProcedural(6), std::invalid_argument);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(calls, 1);
    ExpectRgb(hdr.Center(), {0, 0, 0, 1});
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
    fail = false;
    Effects().SetEnabled(owner, pass, true);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(Effects().GetPassError(owner, pass).empty());
}

TEST_F(ProceduralNative, RemovingPendingPassAndRetiringInsideDrawKeepIssuedGpuWorkAlive) {
    const auto pipeline = Solid("3,.5,1,1", .7f);
    PostProcessPass pending{};
    int canceledCalls{}, retiredCalls{};
    Effects().AddProceduralPass(owner, "draw then retire", -10,
        [&](ProceduralContext& c, const ProceduralFrame&) {
            ++retiredCalls;
            c.SetPipeline(pipeline);
            c.DrawProcedural(6);
            Effects().RemovePass(owner, pending);
            Effects().ReleaseOwner(owner);
            EXPECT_THROW(c.DrawProcedural(6), std::invalid_argument);
        });
    pending = Effects().AddProceduralPass(owner, "removed while scheduled", 0,
        [&](ProceduralContext&, const ProceduralFrame&) { ++canceledCalls; });
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(canceledCalls, 0);
    EXPECT_EQ(retiredCalls, 1);
    ExpectRgb(hdr.Center(), {3, .5f, 1, 1});
    EXPECT_NEAR(depth.Center()[0], .7f, .00001f);
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(retiredCalls, 1);
    ExpectRgb(hdr.Center(), {0, 0, 0, 1});
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
}

TEST_F(ProceduralNative, ResizePreservesPipelineAndNonFinalViewsBypassCallbacksAndDepthWrites) {
    const auto pipeline = Solid("4,1,.25,1", .6f);
    int calls{};
    Effects().AddProceduralPass(owner, "resize", 0,
        [&](ProceduralContext& c, const ProceduralFrame& f) {
            ++calls;
            EXPECT_EQ(f.width, width);
            EXPECT_EQ(f.height, height);
            c.SetPipeline(pipeline);
            c.DrawProcedural(6);
        });
    ASSERT_NO_FATAL_FAILURE(Render());
    width = 111;
    height = 79;
    ASSERT_TRUE(renderer.Reinitialize({window, width, height}));
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(hdr.width, width);
    EXPECT_EQ(hdr.height, height);
    EXPECT_EQ(calls, 2);
    ExpectRgb(hdr.Center(), {4, 1, .25f, 1});
    for (const auto view : {RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Depth}) {
        ASSERT_TRUE(renderer.SetDebugView(view));
        ASSERT_NO_FATAL_FAILURE(Render());
        EXPECT_EQ(calls, 2);
        EXPECT_FLOAT_EQ(depth.Center()[0], 0);
    }
    ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Final));
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(calls, 3);
    ExpectRgb(hdr.Center(), {4, 1, .25f, 1});
}
} // namespace
