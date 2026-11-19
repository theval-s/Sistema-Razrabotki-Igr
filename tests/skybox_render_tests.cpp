#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/RenderingSystem.hpp>
#include <engine/rendering/UglyUtils.hpp>
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
using namespace engine;
using Pixel = std::array<float, 4>;
constexpr float Pi = 3.14159265358979323846f;
constexpr std::array<Pixel, 6> Faces{{{4, 0, 0, 1}, {0, 4, 0, 1}, {0, 0, 4, 1},
                                      {4, 4, 0, 1}, {4, 0, 4, 1}, {0, 4, 4, 1}}};
struct Image {
    UINT width{}, height{};
    std::vector<Pixel> pixels;
    const Pixel& Center() const { return pixels.at(static_cast<size_t>(height / 2) * width + width / 2); }
};

void Read(RenderingContext& context, TextureHandle handle, Image& image) {
    const auto* texture = context.GetResourceManager().TryGetTexture(handle);
    ASSERT_NE(texture, nullptr);
    auto* source = texture->GetTexture();
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
    image.pixels.resize(static_cast<size_t>(image.width) * image.height);
    for (UINT y = 0; y < image.height; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = 0; x < image.width; ++x) {
            Pixel pixel{0, 0, 0, 1};
            if (depth) std::memcpy(&pixel[0], row + x * 4, 4);
            else {
                std::array<uint16_t, 4> halves{};
                std::memcpy(halves.data(), row + x * 8, 8);
                for (size_t c = 0; c < 4; ++c) pixel[c] = DirectX::PackedVector::XMConvertHalfToFloat(halves[c]);
            }
            image.pixels[static_cast<size_t>(y) * image.width + x] = pixel;
        }
    }
    dc->Unmap(staging.Get(), 0);
}

void ExpectPixel(const Pixel& actual, const Pixel& expected) {
    for (size_t c = 0; c < 4; ++c) EXPECT_NEAR(actual[c], expected[c], 0.005f) << "channel " << c;
}

void ExpectNoHazards(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, nullptr, &size));
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, message, &size));
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR) << message->pDescription;
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
        std::string text = message->pDescription;
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        EXPECT_EQ(text.find("hazard"), std::string::npos) << text;
        EXPECT_EQ(text.find("still bound on input"), std::string::npos) << text;
    }
    queue->ClearStoredMessages();
}

class SkyboxRender : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    HWND window{};
    RenderingSystem renderer;
    TextureHandle cube;
    Image hdr, depth;
    UINT width = 96, height = 72;
    FrameContext frame;

    void SetUp() override {
        // No project particle hooks: use the deterministic engine appearance.
        AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "tests");
        window = CreateWindowExW(0, L"STATIC", L"Skybox GPU regression", WS_OVERLAPPEDWINDOW,
                                 0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        const bool initialized = renderer.Initialize({window, width, height}, true);
        const auto diagnostics = renderer.GetDiagnostics();
        if (!initialized && diagnostics.failure && diagnostics.failure->operation == "D3D11CreateDeviceAndSwapChain")
            GTEST_SKIP() << diagnostics.failure->summary;
        ASSERT_TRUE(initialized) << (diagnostics.failure ? diagnostics.failure->detail : "");
        auto& world = renderer.GetWorld();
        world.camera = FPSCamera{0.8f, float(width) / height, 0.1f, 100};
        world.camera.position = {0, 0, 0};
        world.directionalLight = {{0, 0, 0}, {0, -1, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};

        // Native float cube: +X,-X,+Y,-Y,+Z,-Z, each face a distinct HDR colour.
        D3D11_TEXTURE2D_DESC desc{};
        constexpr UINT faceSize = 16;
        desc.Width = desc.Height = faceSize;
        desc.MipLevels = 1;
        desc.ArraySize = 6;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        std::array<std::array<Pixel, faceSize * faceSize>, 6> facePixels;
        std::array<D3D11_SUBRESOURCE_DATA, 6> initial{};
        for (size_t i = 0; i < initial.size(); ++i) {
            facePixels[i].fill(Faces[i]);
            initial[i] = {facePixels[i].data(), faceSize * sizeof(Pixel), faceSize * faceSize * sizeof(Pixel)};
        }
        auto& context = renderer.GetRenderingContext();
        ComPtr<ID3D11Texture2D> texture;
        ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc, initial.data(), &texture));
        ComPtr<ID3D11ShaderResourceView> srv;
        ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateShaderResourceView(texture.Get(), nullptr, &srv));
        cube = renderer.GetResourceManager().RegisterTexture(Texture(std::move(srv)));
        world.environment.cubemap = cube;
    }

    void TearDown() override {
        renderer.Shutdown();
        EXPECT_FALSE(renderer.GetWorld().environment.cubemap);
        EXPECT_EQ(renderer.GetWorld().environment.settings, EnvironmentSettings{});
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
    }

    void Render(double dt = 0) {
        ++frame.frameIndex;
        frame.deltaSeconds = frame.realDeltaSeconds = dt;
        frame.simulationTimeSeconds += dt;
        frame.realTimeSeconds += dt;
        bool captured = false;
        renderer.RenderFrame(frame, [&](RenderingContext& context, const FrameContext&) {
            ASSERT_NO_FATAL_FAILURE(Read(context, renderer.GetResources().gBuffer.resultTexture, hdr));
            ASSERT_NO_FATAL_FAILURE(Read(context, renderer.GetResources().gBuffer.depthTexture, depth));
            captured = true;
        });
        const auto diagnostics = renderer.GetDiagnostics();
        ASSERT_EQ(diagnostics.lifecycle, RenderingLifecycle::Ready)
            << (diagnostics.failure ? diagnostics.failure->pass + "/" + diagnostics.failure->operation + ": " + diagnostics.failure->detail : "");
        ASSERT_TRUE(captured);
        ASSERT_NO_FATAL_FAILURE(ExpectNoHazards(renderer.GetRenderingContext().GetDevice()));
        ComPtr<ID3D11ShaderResourceView> bound;
        renderer.GetRenderingContext().GetDeviceContext()->PSGetShaderResources(0, 1, &bound);
        EXPECT_EQ(bound.Get(), nullptr);
    }
};

TEST_F(SkyboxRender, SixFacesCameraRotationSkyYawAndTranslation) {
    auto& world = renderer.GetWorld();
    // RH camera looks -Z. Camera yaw +pi/2 looks -X; pitch +pi/2 looks +Y.
    struct Direction { float yaw, pitch; size_t face; };
    for (auto direction : {Direction{0, 0, 5}, {Pi, 0, 4}, {-Pi / 2, 0, 0},
                           {Pi / 2, 0, 1}, {0, Pi / 2 - 0.02f, 2}, {0, -Pi / 2 + 0.02f, 3}}) {
        SCOPED_TRACE(direction.face);
        world.camera.yaw = direction.yaw;
        world.camera.pitch = direction.pitch;
        ASSERT_NO_FATAL_FAILURE(Render());
        ExpectPixel(hdr.Center(), Faces[direction.face]);
        const auto stationary = hdr.pixels;
        world.camera.position = {1000, -2000, 3000};
        ASSERT_NO_FATAL_FAILURE(Render());
        EXPECT_EQ(hdr.pixels, stationary);
        world.camera.position = {0, 0, 0};
    }
    world.camera.yaw = world.camera.pitch = 0;
    world.environment.settings.yawRadians = Pi / 2;
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), Faces[0]);
    world.camera.yaw = Pi / 2;
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), Faces[5]);
}

TEST_F(SkyboxRender, HdrTintBrightnessDirectFallbackAndStaleOrWrongShapeTextures) {
    auto& world = renderer.GetWorld();
    auto& settings = world.environment.settings;
    settings.tint = {0.25f, 0.5f, 2};
    settings.brightness = 3;
    settings.fallbackColor = {2, 0.5f, 8};
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {0, 6, 24, 1});
    world.environment.cubemap = {};
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {2, 0.5f, 8, 1});
    auto& manager = renderer.GetResourceManager();
    world.environment.cubemap = manager.CreateTexture({}); // 2D is not a cube.
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {2, 0.5f, 8, 1});
    manager.DestroyTexture(world.environment.cubemap);
    world.environment.cubemap = cube;
    ASSERT_TRUE(manager.DestroyTexture(cube));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {2, 0.5f, 8, 1});
    world.environment = {}; // Scene unload must detach all sky state immediately.
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {0, 0, 0, 1});
}

TEST_F(SkyboxRender, WideProjectionReconstructsOffAxisRaysWithCorrectVerticalOrientation) {
    auto& world = renderer.GetWorld();
    world.camera.fov = 2.2f;
    world.camera.yaw = 0.31f;
    world.camera.pitch = 0.17f;
    world.environment.settings.yawRadians = 0.45f;
    ASSERT_NO_FATAL_FAILURE(Render());
    const auto rotation = Matrix::CreateFromYawPitchRoll(world.camera.yaw, world.camera.pitch, 0) *
                          Matrix::CreateRotationY(-world.environment.settings.yawRadians);
    size_t checked = 0;
    for (UINT y = 0; y < hdr.height; ++y) {
        for (UINT x = 0; x < hdr.width; ++x) {
            // Independent analytic perspective ray; do not repeat the shader's
            // inverse-projection reconstruction in the test oracle.
            Vector3 ray{(2 * (x + 0.5f) / hdr.width - 1) * std::tan(world.camera.fov / 2) * world.camera.aspectRatio,
                         (1 - 2 * (y + 0.5f) / hdr.height) * std::tan(world.camera.fov / 2), -1};
            ray = Vector3::TransformNormal(ray, rotation);
            const std::array components{ray.x, ray.y, ray.z};
            size_t axis = 0;
            for (size_t i = 1; i < 3; ++i) if (std::abs(components[i]) > std::abs(components[axis])) axis = i;
            float second = 0;
            for (size_t i = 0; i < 3; ++i) if (i != axis) second = std::max(second, std::abs(components[i]));
            if (std::abs(components[axis]) < second * 1.3f) continue; // Exclude seamless filter footprints.
            const auto face = axis * 2 + (components[axis] < 0 ? 1 : 0);
            const auto& actual = hdr.pixels[static_cast<size_t>(y) * hdr.width + x];
            for (size_t c = 0; c < 4; ++c) ASSERT_NEAR(actual[c], Faces[face][c], 0.005f) << x << ',' << y;
            ++checked;
        }
    }
    EXPECT_GT(checked, 1000u);
}

TEST_F(SkyboxRender, OpaqueGeometryOccludesSkyDepthIsUnchangedAndSharedBindingsSurvive) {
    auto& manager = renderer.GetResourceManager();
    auto& world = renderer.GetWorld();
    const auto mesh = ugly_utils::FromRawMesh(manager, ugly_utils::CreateCube(1, 1, 1));
    const auto material = manager.CreateMaterial({});
    ASSERT_TRUE(material);
    const auto instance = manager.CreateMaterialInstance(material);
    world.renderItems.push_back({Matrix::CreateTranslation(0, 0, -3), mesh, instance});
    ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Depth));
    ASSERT_NO_FATAL_FAILURE(Render());
    const auto originalDepth = depth.pixels;
    const auto originalLit = hdr.pixels;
    ASSERT_GT(depth.Center()[0], 0);
    ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Final));
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(depth.pixels, originalDepth);
    size_t foreground = 0, background = 0;
    for (size_t i = 0; i < hdr.pixels.size(); ++i) {
        if (depth.pixels[i][0] > 0) {
            ++foreground;
            ASSERT_EQ(hdr.pixels[i], originalLit[i]) << "Sky overwrote opaque geometry";
        } else {
            ++background;
            for (size_t c = 0; c < 4; ++c) ASSERT_NEAR(hdr.pixels[i][c], Faces[5][c], 0.005f);
        }
    }
    EXPECT_GT(foreground, 50u);
    EXPECT_GT(background, 50u);
    // Present uses its own b5, but b0 must again be the shared Frame buffer.
    auto* dc = renderer.GetRenderingContext().GetDeviceContext();
    for (bool vertex : {true, false}) {
        ComPtr<ID3D11Buffer> buffer;
        if (vertex) dc->VSGetConstantBuffers(0, 1, &buffer);
        else dc->PSGetConstantBuffers(0, 1, &buffer);
        ASSERT_TRUE(buffer);
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        EXPECT_EQ(desc.ByteWidth, sizeof(FrameBufferData));
    }
    ComPtr<ID3D11Buffer> viewBuffer;
    dc->VSGetConstantBuffers(1, 1, &viewBuffer);
    ASSERT_TRUE(viewBuffer);
    D3D11_BUFFER_DESC desc{};
    viewBuffer->GetDesc(&desc);
    ASSERT_EQ(desc.ByteWidth, sizeof(ViewBufferData));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    ASSERT_HRESULT_SUCCEEDED(renderer.GetRenderingContext().GetDevice()->CreateBuffer(&desc, nullptr, &staging));
    dc->CopyResource(staging.Get(), viewBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_HRESULT_SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    ViewBufferData view{};
    std::memcpy(&view, mapped.pData, sizeof(view));
    dc->Unmap(staging.Get(), 0);
    EXPECT_EQ(view.viewMatrix, world.camera.GetViewMatrix());
    EXPECT_EQ(view.projectionMatrix, world.camera.GetProjectionMatrix());
    EXPECT_EQ(view.cameraPos, world.camera.position);
    ComPtr<ID3D11SamplerState> actualSampler, sharedSampler;
    dc->PSGetSamplers(0, 1, &actualSampler);
    renderer.GetRenderingContext().BindSamplers();
    dc->PSGetSamplers(0, 1, &sharedSampler);
    EXPECT_EQ(actualSampler.Get(), sharedSampler.Get());
    world.renderItems.clear();
    manager.DestroyMaterialInstance(instance);
    manager.DestroyMaterial(material);
    manager.DestroyMesh(mesh);
}

TEST_F(SkyboxRender, ResizeDiagnosticViewsAndParticlesCompositeOverSky) {
    auto& world = renderer.GetWorld();
    world.environment.cubemap = {};
    world.environment.settings.fallbackColor = {0, 2, 1};
    for (auto extent : {std::array{120u, 90u}, std::array{80u, 60u}}) {
        ASSERT_TRUE(renderer.Reinitialize({window, extent[0], extent[1]}));
        ASSERT_NO_FATAL_FAILURE(Render());
        EXPECT_EQ(hdr.width, extent[0]);
        EXPECT_EQ(hdr.height, extent[1]);
        for (const auto& pixel : hdr.pixels) ExpectPixel(pixel, {0, 2, 1, 1});
        for (auto mode : {RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Depth}) {
            ASSERT_TRUE(renderer.SetDebugView(mode));
            ASSERT_NO_FATAL_FAILURE(Render());
            for (const auto& pixel : hdr.pixels) ASSERT_EQ(pixel, (Pixel{0, 0, 0, 1}));
        }
        ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Final));
    }
    ASSERT_TRUE(renderer.SetParticleBudget({.maxParticles = 8, .maxEmitters = 2}));
    RenderWorld::EmitterSource source{
        .owner = 42, .settings = {.appearance = 2, .maxAlive = 1, .rate = 4, .lifetime = 20,
            .speed = 0, .radius = 0.5f, .spread = 0, .tint = {4, 0, 0}},
        .position = {0, 0, -3}, .direction = {0, 1, 0}};
    source.effect.fadeIn = source.effect.softDistance = 0;
    source.effect.opacityStart = source.effect.opacityEnd = 0.5f;
    source.effect.sizeStart = source.effect.sizeEnd = 1;
    world.particleEmitters.push_back(source);
    ASSERT_NO_FATAL_FAILURE(Render(0.25));
    const auto center = hdr.Center();
    EXPECT_GT(center[0], 0.5f) << "Particle must contribute over sky";
    EXPECT_GT(center[1], 0.1f) << "Translucent particle must retain background";
    EXPECT_LT(center[1], 1.9f) << "Particle must attenuate background";
    EXPECT_NEAR(center[2], center[1] / 2, 0.01f);
    EXPECT_FLOAT_EQ(depth.Center()[0], 0);
    world.particleEmitters.clear();
    renderer.ResetParticles();
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {0, 2, 1, 1});
}
} // namespace
