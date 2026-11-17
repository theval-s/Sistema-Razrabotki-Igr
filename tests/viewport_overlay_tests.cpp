#include <editor/viewport_labels.hpp>
#include <editor/viewport_overlay.hpp>
#include <editor/scene_events.hpp>
#include <editor/selection.hpp>
#include <editor/transform_gizmo.hpp>

#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/core/PathUtils.hpp>

#include <QApplication>
#include <QFontMetricsF>
#include <QFontDatabase>
#include <QImage>
#include <QVariant>
#include <QWidget>
#include <d3d11.h>
#include <wrl/client.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Pixel = std::array<unsigned char, 4>;
using DirectX::SimpleMath::Color;
constexpr Pixel Background{32, 64, 96, 255};

bool LoadOffscreenFonts() {
    // The Windows offscreen plugin uses FreeType rather than the system font
    // database. Register real glyphs before either the WARP or native smoke test.
    static const bool loaded = [] {
        const QString directory = qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/");
        const int regular = QFontDatabase::addApplicationFont(directory + QStringLiteral("arial.ttf"));
        const int italic = QFontDatabase::addApplicationFont(directory + QStringLiteral("ariali.ttf"));
        return regular >= 0 && italic >= 0;
    }();
    return loaded;
}

TEST(ViewportOverlayIntegration, ConstructionDoesNotCreateViewportChildWidget) {
    engine::Scene scene;
    editor::SceneEvents events(scene);
    editor::Selection selection(scene, events);
    editor::TransformGizmo gizmo(scene, selection);
    QWidget viewport;
    editor::ViewportOverlaySubsystem overlay(selection, gizmo, viewport);
    EXPECT_TRUE(viewport.findChildren<QWidget*>().empty());
}

struct NativeFixture {
    std::filesystem::path previousRoot = engine::AssetPathResolver::GetGameRoot();
    HWND window = nullptr;
    ~NativeFixture() {
        if (window) DestroyWindow(window);
        engine::AssetPathResolver::SetGameRoot(previousRoot);
    }
};

// Capture the currently bound swap-chain buffer before FLIP_DISCARD Present.
void CaptureSurface(engine::RenderingContext& context, std::vector<Pixel>& pixels) {
    auto* dc = context.GetDeviceContext();
    ComPtr<ID3D11RenderTargetView> target;
    dc->OMGetRenderTargets(1, &target, nullptr);
    ASSERT_TRUE(target);
    ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source;
    ASSERT_HRESULT_SUCCEEDED(resource.As(&source));
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    ASSERT_EQ(desc.SampleDesc.Count, 1u);
    ASSERT_TRUE(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc, nullptr, &staging));
    dc->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_HRESULT_SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    pixels.resize(static_cast<std::size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * desc.Width,
                    static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    desc.Width * sizeof(Pixel));
    dc->Unmap(staging.Get(), 0);
}

TEST(ViewportOverlayIntegration, ProjectNativePipelineCompositesAndClearsSkeletonLabels) {
    const wchar_t* projectPath = _wgetenv(L"SRI_TEST_PROJECT");
    if (!projectPath || !*projectPath) GTEST_SKIP() << "Set SRI_TEST_PROJECT to a project file for the native smoke test";
    ASSERT_TRUE(LoadOffscreenFonts());
    std::string error;
    const auto project = engine::Project::Load(projectPath, error);
    ASSERT_TRUE(project) << error;
    ASSERT_FALSE(project->startupScene.empty());
    NativeFixture fixture;
    engine::AssetPathResolver::SetGameRoot(project->ContentDir());
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(project->ContentDir(), engine::AssetPathResolver::GetRoot() / "engine", error)) << error;
    constexpr UINT width = 640, height = 480;
    fixture.window = CreateWindowExW(0, L"STATIC", L"Viewport overlay smoke", WS_OVERLAPPEDWINDOW,
                                     0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(fixture.window, nullptr);
    {
        engine::RenderingContext probe;
        if (!probe.Initialize({fixture.window, width, height}, false))
            GTEST_SKIP() << "D3D11 hardware/swap-chain initialization unavailable";
    }
    QWidget viewport;
    viewport.resize(width, height);
    viewport.setFont(QFont(QStringLiteral("Arial"), 10));
    viewport.setProperty("showAudioOverlay", false);
    engine::Engine host({.applicationName = "Viewport overlay smoke", .surface = {fixture.window, width, height},
                         .taskThreads = 2, .graphicsDebugLayer = false});
    editor::SceneEvents events(host.GetScene());
    editor::Selection selection(host.GetScene(), events);
    editor::TransformGizmo gizmo(host.GetScene(), selection);
    auto* rendering = host.AddSubsystem<engine::RenderingSubsystem>();
    ASSERT_TRUE(rendering);
    ASSERT_TRUE(host.AddSubsystem<engine::AnimationSubsystem>());
    ASSERT_TRUE(host.AddSubsystem<engine::RenderExtractionSubsystem>());
    ASSERT_TRUE(host.AddSubsystem<editor::ViewportOverlaySubsystem>(selection, gizmo, viewport));
    host.SetEditorMode();
    ASSERT_TRUE(host.Initialize());
    ASSERT_TRUE(host.LoadScene(engine::AssetPathResolver::ToAbsolute(engine::FromUtf8(project->startupScene)), error)) << error;
    entt::entity selected = entt::null;
    host.GetScene().ForEach<engine::SkinnedMeshRenderer>([&](entt::entity entity, const engine::SkinnedMeshRenderer&) {
        if (selected == entt::null) selected = entity;
    });
    ASSERT_TRUE(selected != entt::null) << "Startup scene needs a skinned entity";
    selection.select(selected);
    std::vector<Pixel> captured;
    unsigned captures = 0;
    rendering->SetOverlayCallbacks([&](engine::RenderingContext& context, const engine::FrameContext&) {
        CaptureSurface(context, captured);
        ++captures;
    }, {});
    // Asset requests complete asynchronously. Wait for actual visible cyan label
    // pixels, rather than assuming a fixed number of frames is sufficient.
    std::vector<Pixel> withLabels, withoutLabels;
    std::size_t labelPixels = 0;
    bool geometryReady = false;
    for (int attempt = 0; attempt < 300 && (labelPixels < 10 || !geometryReady); ++attempt) {
        viewport.setProperty("showSkeletonOverlay", true);
        host.Tick();
        withLabels = captured;
        viewport.setProperty("showSkeletonOverlay", false);
        host.Tick();
        withoutLabels = captured;
        geometryReady = !rendering->GetWorld().selectedSceneItems.empty();
        auto& assets = host.GetSceneAssets();
        host.GetScene().ForEach<engine::MeshRenderer>([&](entt::entity, const engine::MeshRenderer& renderer) {
            geometryReady &= bool(assets.Peek(renderer.mesh)) && bool(assets.Peek(renderer.materialInstance));
        });
        host.GetScene().ForEach<engine::SkinnedMeshRenderer>([&](entt::entity, const engine::SkinnedMeshRenderer& renderer) {
            geometryReady &= bool(assets.Peek(renderer.mesh)) && bool(assets.Peek(renderer.materialInstance));
        });
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_EQ(withLabels.size(), static_cast<std::size_t>(width) * height);
        ASSERT_EQ(withoutLabels.size(), withLabels.size());
        labelPixels = 0;
        for (std::size_t i = 0; i < withLabels.size(); ++i) {
            const auto& p = withLabels[i];
            // Cyan text has one low and two high RGB channels in either buffer ordering.
            const bool cyan = p[1] > 150 && ((p[0] > 170 && p[2] < 140) || (p[2] > 170 && p[0] < 140));
            if (cyan && p != withoutLabels[i]) ++labelPixels;
        }
        if (labelPixels < 10 || !geometryReady) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(labelPixels, 10u) << "Skeleton labels did not reach the pre-Present UI callback or did not clear";
    EXPECT_FALSE(rendering->GetWorld().selectedSceneItems.empty()) << "Selected skinned geometry was not extracted";
    std::size_t visibleScene = 0;
    // Exclude the editor's bottom-left screen axes from the scene evidence.
    for (UINT y = 0; y < height - 120; ++y)
        for (UINT x = 120; x < width; ++x) {
            const auto& p = withoutLabels[static_cast<std::size_t>(y) * width + x];
            visibleScene += p[0] > 16 || p[1] > 16 || p[2] > 16;
        }
    EXPECT_GT(visibleScene, 1000u) << "Scene disappeared when labels were disabled";
    EXPECT_GE(captures, 2u);
    rendering->ClearOverlayCallbacks();
    host.Shutdown();
}

class ViewportLabelsTest : public testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> targetTexture;
    ComPtr<ID3D11RenderTargetView> target;
    editor::ViewportLabels labels;
    QFont font{QStringLiteral("Arial")};
    UINT width = 320, height = 180;

    void SetUp() override {
        ASSERT_TRUE(LoadOffscreenFonts()) << "The offscreen font database requires real regular and italic fonts";
        font.setPixelSize(20);
        CreateDevice();
        ASSERT_TRUE(device);
        CreateTarget(width, height);
    }

    void TearDown() override {
        labels.clear();
        if (context) context->ClearState();
    }

    void CreateDevice() {
        ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf()));
    }

    void CreateTarget(UINT w, UINT h) {
        width = w;
        height = h;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        targetTexture.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, targetTexture.GetAddressOf()));
        ASSERT_HRESULT_SUCCEEDED(device->CreateRenderTargetView(targetTexture.Get(), nullptr, target.GetAddressOf()));
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        ClearTarget();
    }

    void ClearTarget() {
        constexpr float background[]{32 / 255.0f, 64 / 255.0f, 96 / 255.0f, 1};
        context->ClearRenderTargetView(target.Get(), background);
    }

    std::vector<Pixel> Readback() {
        D3D11_TEXTURE2D_DESC desc{};
        targetTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        const HRESULT created = device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        EXPECT_HRESULT_SUCCEEDED(created);
        if (FAILED(created)) return {};
        context->CopyResource(staging.Get(), targetTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        EXPECT_HRESULT_SUCCEEDED(hr);
        if (FAILED(hr)) return {};
        std::vector<Pixel> pixels(static_cast<std::size_t>(width) * height);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * width,
                static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch, width * sizeof(Pixel));
        context->Unmap(staging.Get(), 0);
        return pixels;
    }

    // This is a deliberately generous region based on public font metrics, not
    // renderer buffers: every pixel outside must still be the scene background.
    QRect LabelRegion(const QPointF& baseline, const QString& text, qreal dpr = 1) const {
        QRectF bounds = QFontMetricsF(font).boundingRect(text).translated(baseline);
        bounds.adjust(-4, -4, 5, 5); // antialias padding and one-logical-pixel shadow
        return QRectF(bounds.x() * dpr, bounds.y() * dpr,
                      bounds.width() * dpr, bounds.height() * dpr).toAlignedRect();
    }

    void CheckLabel(const std::vector<Pixel>& pixels, const QRect& region) const {
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(width) * height);
        std::size_t changed = 0, colored = 0;
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
                if (!region.contains(static_cast<int>(x), static_cast<int>(y))) {
                    if (pixel != Background) {
                        ADD_FAILURE() << "Scene changed outside label at " << x << "," << y;
                        return;
                    }
                } else {
                    changed += pixel != Background;
                    colored += pixel[0] > 150 && pixel[1] > 100 && pixel[2] < 90;
                }
                EXPECT_EQ(pixel[3], 255) << "Opaque scene alpha must survive premultiplied blending";
            }
        }
        EXPECT_GT(changed, 20u);
        EXPECT_GT(colored, 10u) << "Expected visible gold glyphs, not just the black shadow";
    }

    void CheckBackground(const std::vector<Pixel>& pixels) const {
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            if (pixels[i] != Background) {
                ADD_FAILURE() << "Background changed at " << i % width << "," << i / width;
                return;
            }
        }
    }

    void Draw(const QPointF& baseline, const QString& text, qreal dpr = 1) {
        labels.beginFrame(font, dpr);
        labels.add(baseline, text, Color{1.0f, 0.8f, 0.1f, 1.0f});
        labels.render(device.Get(), context.Get());
    }
};

TEST_F(ViewportLabelsTest, TransparentGlyphsMoveWithoutTrailsAndEmptyFramePreservesScene) {
    const QString text = QStringLiteral("Viewport 123");
    const QPointF first{24, 42}, moved{142, 125};
    Draw(first, text);
    CheckLabel(Readback(), LabelRegion(first, text));

    ClearTarget();
    Draw(moved, text); // same cached texture, new position
    const auto pixels = Readback();
    CheckLabel(pixels, LabelRegion(moved, text)); // includes checking the old region

    ClearTarget();
    labels.beginFrame(font, 1);
    labels.render(device.Get(), context.Get());
    CheckBackground(Readback());
}

TEST_F(ViewportLabelsTest, TargetResizeAndDprTwoUsePhysicalPositionAndSizeExactlyOnce) {
    const QString text = QStringLiteral("Resize");
    const QPointF baseline{43, 51};
    Draw(baseline, text);
    const auto original = Readback();
    CheckLabel(original, LabelRegion(baseline, text));
    CreateTarget(540, 300); // compositor must acquire the current target/viewport
    Draw(baseline, text, 2);
    const auto doubled = Readback();
    CheckLabel(doubled, LabelRegion(baseline, text, 2));
    std::size_t originalInk = 0, doubledInk = 0;
    for (const auto& pixel : original) originalInk += pixel != Background;
    for (const auto& pixel : doubled) doubledInk += pixel != Background;
    EXPECT_GT(doubledInk, originalInk * 2);
    EXPECT_LT(doubledInk, originalInk * 6);
}

TEST_F(ViewportLabelsTest, PointSizedViewportFontProducesVisibleGlyphs) {
    font.setPointSizeF(12);
    const QPointF baseline{30, 65};
    const QString text = QStringLiteral("Point sized labels");
    Draw(baseline, text);
    CheckLabel(Readback(), LabelRegion(baseline, text));
}

TEST_F(ViewportLabelsTest, UnicodeAndItalicNegativeBearingsSurviveDeviceRecreation) {
    font.setItalic(true);
    // Explicit escapes keep the source independent of the compiler code page.
    const QString text = QString::fromUtf8("j \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xCE\xA9");
    const QPointF baseline{30, 65};
    Draw(baseline, text);
    const auto before = Readback();
    CheckLabel(before, LabelRegion(baseline, text));

    // Keep the compositor alive, including its cached SRVs, across device loss.
    context->ClearState();
    target.Reset();
    targetTexture.Reset();
    CreateDevice();
    ASSERT_TRUE(device);
    CreateTarget(width, height);
    Draw(baseline, text);
    const auto after = Readback();
    CheckLabel(after, LabelRegion(baseline, text));
    EXPECT_EQ(before, after);
}

TEST_F(ViewportLabelsTest, ClearAndInvalidInputsDrawNothing) {
    const QString text = QStringLiteral("Discard me");
    Draw({25, 50}, text);
    CheckLabel(Readback(), LabelRegion({25, 50}, text));
    ClearTarget();
    labels.clear();
    labels.render(device.Get(), context.Get());
    CheckBackground(Readback());

    labels.beginFrame(font, std::numeric_limits<qreal>::quiet_NaN());
    labels.add({25, 50}, text, Color{1, 1, 1, 1});
    labels.render(device.Get(), context.Get());
    CheckBackground(Readback());
    labels.beginFrame(font, 1);
    labels.add({std::numeric_limits<qreal>::infinity(), 50}, text, Color{1, 1, 1, 1});
    labels.render(device.Get(), context.Get());
    CheckBackground(Readback());
}

TEST_F(ViewportLabelsTest, ContextChangeRecreatesBatchForTheNewContext) {
    const QPointF baseline{25, 50};
    const QString text = QStringLiteral("New context");
    Draw(baseline, text);
    CheckLabel(Readback(), LabelRegion(baseline, text));
    ClearTarget();

    ComPtr<ID3D11DeviceContext> deferred;
    ASSERT_HRESULT_SUCCEEDED(device->CreateDeferredContext(0, deferred.GetAddressOf()));
    deferred->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    deferred->RSSetViewports(1, &viewport);
    labels.beginFrame(font, 1);
    labels.add(baseline, text, Color{1, 0.8f, 0.1f, 1});
    labels.render(device.Get(), deferred.Get());
    ComPtr<ID3D11CommandList> commands;
    ASSERT_HRESULT_SUCCEEDED(deferred->FinishCommandList(FALSE, commands.GetAddressOf()));
    context->ExecuteCommandList(commands.Get(), TRUE);
    CheckLabel(Readback(), LabelRegion(baseline, text));
}

TEST_F(ViewportLabelsTest, CacheEvictionKeepsAlreadyPreparedSpritesAlive) {
    labels.beginFrame(font, 1);
    // More than the cache entry bound, with one visible label followed by many
    // offscreen labels. The first texture must remain alive through SpriteBatch End.
    const QString first = QStringLiteral("Visible gold");
    labels.add({25, 50}, first, Color{1, 0.8f, 0.1f, 1});
    for (int i = 0; i < 300; ++i)
        labels.add({-10000, -10000}, QString::number(i), Color{1, 0.8f, 0.1f, 1});
    labels.render(device.Get(), context.Get());
    CheckLabel(Readback(), LabelRegion({25, 50}, first));
}

TEST_F(ViewportLabelsTest, TranslucentTintUsesPremultipliedAlpha) {
    labels.beginFrame(font, 1);
    labels.add({30, 65}, QStringLiteral("MMMM"), Color{1, 0, 0, 0.5f});
    labels.render(device.Get(), context.Get());
    const auto pixels = Readback();
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(width) * height);
    std::size_t translucentRed = 0;
    for (const auto& pixel : pixels) {
        EXPECT_EQ(pixel[3], 255);
        // Straight-alpha tint would produce almost-full red (~255) instead.
        EXPECT_LE(pixel[0], 145);
        translucentRed += pixel[0] >= 130 && pixel[0] <= 145 && pixel[1] < 40;
    }
    EXPECT_GT(translucentRed, 10u);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
