#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>

#include <DirectXPackedVector.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace engine;
using Pixel = std::array<float, 4>;
constexpr UINT InitialWidth = 320, InitialHeight = 240;

struct NativeFiles {
    std::filesystem::path oldGameRoot = AssetPathResolver::GetGameRoot();
    HWND window = CreateWindowExW(0, L"STATIC", L"Native render debug integration", WS_OVERLAPPEDWINDOW,
                                   20, 20, InitialWidth, InitialHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    NativeFiles() {
        AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "example_game");
        if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    ~NativeFiles() {
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldGameRoot);
    }
};

struct Image {
    UINT width = 0, height = 0;
    std::vector<Pixel> pixels;
    const Pixel& At(UINT x, UINT y) const { return pixels.at(static_cast<size_t>(y) * width + x); }
};

// Read attachments by resource identity, never by assuming a pass leaves an SRV
// behind. The temporary binding is removed immediately, before any further draw.
void ReadHandle(RenderingContext& context, TextureHandle handle, Image& image);

void ReadTexture(RenderingContext& context, ID3D11Texture2D* texture, Image& image) {
    ASSERT_NE(texture, nullptr);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const auto format = desc.Format;
    ASSERT_EQ(desc.SampleDesc.Count, 1u);
    const bool bytes = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool halves = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool depth = format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_R32_FLOAT;
    const bool mask = format == DXGI_FORMAT_R8G8_UNORM;
    ASSERT_TRUE(bytes || halves || depth || mask) << format;
    image.width = desc.Width;
    image.height = desc.Height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc, nullptr, &staging));
    auto* dc = context.GetDeviceContext();
    dc->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_HRESULT_SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    image.pixels.resize(static_cast<size_t>(image.width) * image.height);
    for (UINT y = 0; y < image.height; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < image.width; ++x) {
            Pixel pixel{0, 0, 0, 1};
            if (bytes) {
                for (size_t c = 0; c < 4; ++c) pixel[c] = row[x * 4 + c] / 255.0f;
                if (format == DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(pixel[0], pixel[2]);
            } else if (halves) {
                std::array<uint16_t, 4> value{};
                std::memcpy(value.data(), row + x * 8, 8);
                for (size_t c = 0; c < 4; ++c) pixel[c] = DirectX::PackedVector::XMConvertHalfToFloat(value[c]);
            } else if (depth) std::memcpy(&pixel[0], row + x * 4, 4);
            else { pixel[0] = row[x * 2] / 255.0f; pixel[1] = row[x * 2 + 1] / 255.0f; }
            image.pixels[static_cast<size_t>(y) * image.width + x] = pixel;
        }
    }
    dc->Unmap(staging.Get(), 0);
}

void ReadHandle(RenderingContext& context, TextureHandle handle, Image& image) {
    ASSERT_TRUE(handle);
    context.SetShaderResources(BindSlots::Texture::Pass0, handle);
    ComPtr<ID3D11ShaderResourceView> srv;
    context.GetDeviceContext()->PSGetShaderResources(16, 1, &srv);
    ID3D11ShaderResourceView* empty = nullptr;
    context.GetDeviceContext()->PSSetShaderResources(16, 1, &empty);
    ASSERT_TRUE(srv);
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    ASSERT_HRESULT_SUCCEEDED(resource.As(&texture));
    ASSERT_NO_FATAL_FAILURE(ReadTexture(context, texture.Get(), image));
}

void ReadBackbuffer(RenderingContext& context, Image& image) {
    ComPtr<ID3D11RenderTargetView> target;
    context.GetDeviceContext()->OMGetRenderTargets(1, &target, nullptr);
    ASSERT_TRUE(target);
    ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture, swapBuffer;
    ASSERT_HRESULT_SUCCEEDED(resource.As(&texture));
    ASSERT_HRESULT_SUCCEEDED(context.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(&swapBuffer)));
    EXPECT_EQ(texture.Get(), swapBuffer.Get()) << "Overlay must use the actual swap-chain target";
    ASSERT_NO_FATAL_FAILURE(ReadTexture(context, texture.Get(), image));
}

void ExpectPixel(const Pixel& actual, const Pixel& expected, float tolerance = 2.1f / 255) {
    for (size_t c = 0; c < 4; ++c) {
        EXPECT_TRUE(std::isfinite(actual[c]));
        EXPECT_NEAR(actual[c], expected[c], tolerance) << "channel " << c;
    }
}

void ExpectNoHazards(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, nullptr, &size));
        std::vector<std::byte> data(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
        ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, message, &size));
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR) << message->pDescription;
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
        // D3D reports automatic SRV/DSV nulling as warnings, not errors.
        std::string description = message->pDescription;
        std::transform(description.begin(), description.end(), description.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        EXPECT_EQ(description.find("hazard"), std::string::npos) << message->pDescription;
        EXPECT_EQ(description.find("still bound on input"), std::string::npos) << message->pDescription;
    }
    queue->ClearStoredMessages();
}

struct FrameImages { Image color, albedo, normal, depth, lit, mask; };

void ExpectPresentBindings(RenderingSubsystem& rendering) {
    auto& context = rendering.GetContext();
    auto* dc = context.GetDeviceContext();
    for (UINT slot : {5u, 16u}) {
        ComPtr<ID3D11ShaderResourceView> srv;
        dc->PSGetShaderResources(slot, 1, &srv);
        EXPECT_EQ(srv.Get(), nullptr) << "Present must detach source/mask inputs at slot " << slot;
    }
    ComPtr<ID3D11DepthStencilView> dsv;
    dc->OMGetRenderTargets(0, nullptr, &dsv);
    EXPECT_EQ(dsv.Get(), nullptr) << "Presentation must detach the scene DSV";
    ComPtr<ID3D11Buffer> buffer;
    dc->PSGetConstantBuffers(5, 1, &buffer);
    ASSERT_TRUE(buffer);
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    ASSERT_EQ(desc.ByteWidth, 16u);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    ASSERT_HRESULT_SUCCEEDED(context.GetDevice()->CreateBuffer(&desc, nullptr, &staging));
    dc->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_HRESULT_SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    struct Parameters { uint32_t mode; float nearPlane, farPlane, padding; } parameters{};
    static_assert(sizeof(parameters) == 16);
    std::memcpy(&parameters, mapped.pData, sizeof(parameters));
    dc->Unmap(staging.Get(), 0);
    EXPECT_EQ(parameters.mode, static_cast<uint32_t>(rendering.GetDebugView()));
    EXPECT_FLOAT_EQ(parameters.nearPlane, rendering.GetWorld().camera.nearPlane);
    EXPECT_FLOAT_EQ(parameters.farPlane, rendering.GetWorld().camera.farPlane);
}

void Capture(RenderingSubsystem& rendering, FrameImages& images) {
    auto& context = rendering.GetContext();
    const auto& resources = rendering.GetRendering().GetResources();
    ASSERT_NO_FATAL_FAILURE(ExpectPresentBindings(rendering));
    ASSERT_NO_FATAL_FAILURE(ReadBackbuffer(context, images.color));
    ASSERT_NO_FATAL_FAILURE(ReadHandle(context, resources.gBuffer.albedoTexture, images.albedo));
    ASSERT_NO_FATAL_FAILURE(ReadHandle(context, resources.gBuffer.normalTexture, images.normal));
    ASSERT_NO_FATAL_FAILURE(ReadHandle(context, resources.gBuffer.depthTexture, images.depth));
    ASSERT_NO_FATAL_FAILURE(ReadHandle(context, resources.gBuffer.resultTexture, images.lit));
    ASSERT_NO_FATAL_FAILURE(ReadHandle(context, resources.selectionMask, images.mask));
}

void ExpectRoutedPixels(const FrameImages& images, RenderDebugView mode, const FPSCamera& camera) {
    ASSERT_EQ(images.color.pixels.size(), images.depth.pixels.size());
    const auto inverseProjection = camera.GetProjectionMatrix().Invert();
    size_t geometry = 0, background = 0;
    for (size_t i = 0; i < images.color.pixels.size(); ++i) {
        Pixel expected{0, 0, 0, 1};
        const float raw = images.depth.pixels[i][0];
        if (raw > 0) ++geometry; else ++background;
        if (mode == RenderDebugView::Albedo || mode == RenderDebugView::Normals) {
            expected = (mode == RenderDebugView::Albedo ? images.albedo : images.normal).pixels[i];
            expected[3] = 1;
        } else if (mode == RenderDebugView::Depth && raw > 0) {
            // Independent reconstruction through the actual inverse projection,
            // not a copy of PresentPass's reverse-Z rational expression.
            auto view = Vector4::Transform(Vector4(0, 0, raw, 1), inverseProjection);
            const float distance = -view.z / view.w;
            const float value = 1 - std::clamp((distance - camera.nearPlane) / (camera.farPlane - camera.nearPlane), 0.0f, 1.0f);
            expected = {value, value, value, 1};
        } else if (mode == RenderDebugView::Final) {
            for (size_t c = 0; c < 3; ++c) {
                const float value = images.lit.pixels[i][c];
                expected[c] = value / (1 + value);
            }
        }
        // Stop after the first bad pixel to avoid burying a routing failure in
        // tens of thousands of near-identical assertions.
        for (size_t c = 0; c < 4; ++c) {
            ASSERT_TRUE(std::isfinite(images.color.pixels[i][c]));
            ASSERT_NEAR(images.color.pixels[i][c], expected[c], 2.1f / 255) << "pixel " << i << " channel " << c;
        }
    }
    EXPECT_GT(geometry, 50u);
    EXPECT_GT(background, 50u);
}

void ExpectSelectionComposite(const FrameImages& selected, const std::vector<Pixel>& unselected) {
    ASSERT_EQ(selected.color.pixels.size(), unselected.size());
    for (UINT y = 0; y < selected.color.height; ++y) {
        for (UINT x = 0; x < selected.color.width; ++x) {
            Pixel expected = unselected[static_cast<size_t>(y) * selected.color.width + x];
            const auto& center = selected.mask.At(x, y);
            if (center[0] < 0.5f && center[1] < 0.5f) {
                float visible = 0, hidden = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (dx * dx + dy * dy > 4) continue;
                        const auto sx = static_cast<UINT>(std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(selected.mask.width) - 1));
                        const auto sy = static_cast<UINT>(std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(selected.mask.height) - 1));
                        visible = std::max(visible, selected.mask.At(sx, sy)[0]);
                        hidden = std::max(hidden, selected.mask.At(sx, sy)[1]);
                    }
                }
                if (visible >= 0.5f) expected = {1, 0.72f, 0.12f, 1};
                else if (hidden >= 0.5f && ((x + y) / 4) % 2 == 0) expected = {0.22f, 0.78f, 1, 1};
            }
            for (size_t c = 0; c < 4; ++c)
                ASSERT_NEAR(selected.color.At(x, y)[c], expected[c], 1.1f / 255) << "selection pixel " << x << ',' << y;
        }
    }
}

void LoadSceneFixture(Engine& host, AnimationSubsystem& animation, RenderExtractionSubsystem& extraction) {
    std::string error;
    ASSERT_TRUE(host.LoadScene(std::filesystem::path(SRI_SOURCE_ROOT) / "example_game/scenes/animation.scene.json", error)) << error;
    auto& assets = host.GetSceneAssets();
    ASSERT_TRUE(assets.LoadMesh("engine/primitives/plane/model.obj"));
    ASSERT_TRUE(assets.LoadMaterial("engine/primitives/default.material.json"));
    ASSERT_TRUE(assets.LoadMaterialInstance("engine/primitives/default.material-instance.json"));
    ASSERT_TRUE(assets.LoadSkinnedMesh("game/models/TestBot/model.skinned-mesh.json"));
    ASSERT_TRUE(assets.LoadSkeleton("game/models/TestBot/model.skeleton.json"));
    ASSERT_TRUE(assets.LoadAnimationClip("game/models/TestBot/idle.animation.json"));
    ASSERT_TRUE(assets.LoadAnimationClip("game/models/TestBot/walk.animation.json"));
    assets.Update({});
    animation.LateUpdate({});
    extraction.LateUpdate({});
}

TEST(RenderDebugNative, SceneModesSelectionCameraRangesResizeAndSuspendUseActualPresentPass) {
    NativeFiles files;
    ASSERT_NE(files.window, nullptr);
    Engine host({.applicationName = "Native debug view tests", .surface = {files.window, InitialWidth, InitialHeight},
                 .taskThreads = 2, .graphicsDebugLayer = true});
    auto* rendering = host.AddSubsystem<RenderingSubsystem>();
    auto* animation = host.AddSubsystem<AnimationSubsystem>();
    auto* extraction = host.AddSubsystem<RenderExtractionSubsystem>();
    ASSERT_TRUE(rendering && animation && extraction);
    ASSERT_TRUE(host.Initialize());
    ASSERT_NO_FATAL_FAILURE(LoadSceneFixture(host, *animation, *extraction));
    auto& world = rendering->GetWorld();
    const auto sceneItems = world.sceneItems;
    ASSERT_TRUE(std::any_of(sceneItems.begin(), sceneItems.end(), [](const auto& item) { return !item.skinning; }));
    ASSERT_TRUE(std::any_of(sceneItems.begin(), sceneItems.end(), [](const auto& item) { return bool(item.skinning); }));
    auto* originalDevice = rendering->GetContext().GetDevice();
    auto* originalSwapChain = rendering->GetContext().GetSwapChain();
    FrameImages images;
    UINT callbacks = 0;
    rendering->SetOverlayCallbacks([&](RenderingContext&, const FrameContext&) {
        ++callbacks;
        Capture(*rendering, images);
    }, {});
    const auto render = [&] {
        const UINT before = callbacks;
        rendering->Render({});
        ASSERT_EQ(rendering->GetDiagnostics().lifecycle, RenderingLifecycle::Ready);
        ASSERT_EQ(callbacks, before + 1);
        ASSERT_EQ(rendering->GetContext().GetDevice(), originalDevice);
        ASSERT_EQ(rendering->GetContext().GetSwapChain(), originalSwapChain);
        ASSERT_NO_FATAL_FAILURE(ExpectNoHazards(originalDevice));
    };

    // Freeze animation/extraction; all comparisons use identical checked-in
    // geometry and camera. Each isolated group must contribute actual pixels.
    for (bool skinned : {false, true}) {
        world.sceneItems.clear();
        for (const auto& item : sceneItems) if (bool(item.skinning) == skinned) world.sceneItems.push_back(item);
        ASSERT_TRUE(rendering->SetDebugView(RenderDebugView::Normals));
        ASSERT_NO_FATAL_FAILURE(render());
        ASSERT_NO_FATAL_FAILURE(ExpectRoutedPixels(images, RenderDebugView::Normals, world.camera));
        if (!skinned) {
            // The checked-in plane has authored normal (0,1,0), identity
            // rotation and y=0. These are independent known surface values,
            // beyond simply comparing presentation with another attachment.
            for (size_t i = 0; i < images.depth.pixels.size(); ++i) {
                if (images.depth.pixels[i][0] <= 0) continue;
                for (size_t c = 0; c < 3; ++c) {
                    const Pixel expectedNormal{0.5f, 1, 0.5f, 1};
                    ASSERT_NEAR(images.color.pixels[i][c], expectedNormal[c], 2.1f / 255);
                    ASSERT_NEAR(images.albedo.pixels[i][c], 1, 1.1f / 255);
                }
            }
            ASSERT_TRUE(rendering->SetDebugView(RenderDebugView::Depth));
            ASSERT_NO_FATAL_FAILURE(render());
            const auto view = world.camera.GetViewMatrix();
            const auto inverseViewProjection = (view * world.camera.GetProjectionMatrix()).Invert();
            for (UINT y = 0; y < images.color.height; ++y) {
                for (UINT x = 0; x < images.color.width; ++x) {
                    if (images.depth.At(x, y)[0] <= 0) continue;
                    auto nearPoint = Vector4::Transform(Vector4(2 * (x + 0.5f) / images.color.width - 1,
                        1 - 2 * (y + 0.5f) / images.color.height, 1, 1), inverseViewProjection);
                    const Vector3 ray = Vector3(nearPoint.x, nearPoint.y, nearPoint.z) / nearPoint.w - world.camera.position;
                    const auto hit = world.camera.position + ray * (-world.camera.position.y / ray.y);
                    const float distance = -Vector3::Transform(hit, view).z;
                    const float expected = 1 - std::clamp((distance - world.camera.nearPlane) /
                        (world.camera.farPlane - world.camera.nearPlane), 0.0f, 1.0f);
                    ASSERT_NEAR(images.color.At(x, y)[0], expected, 2.1f / 255) << "known floor depth at " << x << ',' << y;
                }
            }
        }
    }
    world.sceneItems = sceneItems;
    const auto directional = world.directionalLight;
    const auto lights = world.lights;
    world.directionalLight.intensity = 0;
    world.lights.clear();
    ASSERT_TRUE(rendering->SetDebugView(RenderDebugView::Albedo));
    ASSERT_NO_FATAL_FAILURE(render());
    ASSERT_NO_FATAL_FAILURE(ExpectRoutedPixels(images, RenderDebugView::Albedo, world.camera));
    EXPECT_TRUE(std::any_of(images.color.pixels.begin(), images.color.pixels.end(), [](const Pixel& pixel) {
        return pixel[0] > 0.2f || pixel[1] > 0.2f || pixel[2] > 0.2f;
    })) << "Albedo must remain readable with lights disabled";
    world.directionalLight = directional;
    world.lights = lights;
    for (const auto range : {std::array{0.1f, 100.0f}, std::array{1.0f, 30.0f}}) {
        world.camera.nearPlane = range[0];
        world.camera.farPlane = range[1];
        for (int repeat = 0; repeat < 2; ++repeat) {
            for (auto mode : {RenderDebugView::Final, RenderDebugView::Albedo, RenderDebugView::Normals,
                              RenderDebugView::Depth, RenderDebugView::Final}) {
                ASSERT_TRUE(rendering->SetDebugView(mode));
                world.selectedSceneItems.clear();
                ASSERT_NO_FATAL_FAILURE(render());
                ASSERT_NO_FATAL_FAILURE(ExpectRoutedPixels(images, mode, world.camera));
                const auto unselected = images.color.pixels;
                world.selectedSceneItems = sceneItems;
                ASSERT_NO_FATAL_FAILURE(render());
                size_t different = 0, maskPixels = 0;
                for (size_t i = 0; i < unselected.size(); ++i) {
                    different += unselected[i] != images.color.pixels[i];
                    maskPixels += images.mask.pixels[i][0] > 0 || images.mask.pixels[i][1] > 0;
                }
                EXPECT_GT(maskPixels, 50u) << "The real selection pass must draw static/skinned items";
                if (mode == RenderDebugView::Final) {
                    EXPECT_GT(different, 10u);
                    ASSERT_NO_FATAL_FAILURE(ExpectSelectionComposite(images, unselected));
                } else EXPECT_EQ(different, 0u) << "Diagnostic data must not contain selection outlines";
            }
        }
    }
    world.selectedSceneItems.clear();
    ASSERT_TRUE(rendering->SetDebugView(RenderDebugView::Depth));
    for (const auto extent : {std::array{400u, 300u}, std::array{240u, 180u}, std::array{320u, 240u}}) {
        ASSERT_TRUE(SetWindowPos(files.window, nullptr, 20, 20, static_cast<int>(extent[0]), static_cast<int>(extent[1]), SWP_NOZORDER | SWP_NOACTIVATE));
        host.ResizeSurface(extent[0], extent[1]);
        ASSERT_NO_FATAL_FAILURE(render());
        EXPECT_EQ(images.color.width, extent[0]);
        EXPECT_EQ(images.color.height, extent[1]);
        EXPECT_EQ(rendering->GetDebugView(), RenderDebugView::Depth);
        ASSERT_NO_FATAL_FAILURE(ExpectRoutedPixels(images, RenderDebugView::Depth, world.camera));
        const auto beforeSuspend = images.color.pixels;
        const UINT before = callbacks;
        ShowWindow(files.window, SW_MINIMIZE);
        host.ResizeSurface(0, 0);
        rendering->Render({});
        rendering->Render({});
        EXPECT_EQ(rendering->GetDiagnostics().lifecycle, RenderingLifecycle::SurfaceUnavailable);
        EXPECT_EQ(callbacks, before);
        EXPECT_EQ(rendering->GetDebugView(), RenderDebugView::Depth);
        ShowWindow(files.window, SW_RESTORE);
        host.ResizeSurface(extent[0], extent[1]);
        ASSERT_NO_FATAL_FAILURE(render());
        EXPECT_EQ(images.color.pixels, beforeSuspend) << "Restore retained a stale target or changed diagnostic state";
    }
    ASSERT_TRUE(rendering->SetDebugView(RenderDebugView::Final));
    ASSERT_NO_FATAL_FAILURE(render());
    ASSERT_NO_FATAL_FAILURE(ExpectRoutedPixels(images, RenderDebugView::Final, world.camera));
    rendering->ClearOverlayCallbacks();
    host.Shutdown();
}

// ClearView is a deterministic native overlay primitive: no copied presentation
// shader and no Qt widget over the HWND. Both callbacks operate on the target
// produced by the actual PresentPass, after its real screen-space gizmo draw.
void PaintMarker(RenderingContext& context, const Pixel& color) {
    ComPtr<ID3D11DeviceContext1> dc;
    ASSERT_HRESULT_SUCCEEDED(context.GetDeviceContext()->QueryInterface(IID_PPV_ARGS(&dc)));
    ComPtr<ID3D11RenderTargetView> target;
    dc->OMGetRenderTargets(1, &target, nullptr);
    ASSERT_TRUE(target);
    const D3D11_RECT rect{4, 7, 16, 10};
    dc->ClearView(target.Get(), color.data(), &rect, 1);
}

TEST(RenderDebugNative, GizmoHostAndEngineOverlayLeasesShareBackbufferInOrderAcrossModes) {
    NativeFiles files;
    ASSERT_NE(files.window, nullptr);
    Engine host({.applicationName = "Native debug overlays", .surface = {files.window, InitialWidth, InitialHeight},
                 .taskThreads = 2, .graphicsDebugLayer = true});
    auto* rendering = host.AddSubsystem<RenderingSubsystem>();
    ASSERT_TRUE(host.Initialize());
    auto& world = rendering->GetWorld();
    world.camera = FPSCamera{1.0f, float(InitialWidth) / InitialHeight, 0.1f, 100};
    world.directionalLight = {{0, 0, 0}, {0, -1, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};
    std::vector<int> order;
    rendering->SetHostOverlayCallbacks([&](RenderingContext& context, const FrameContext&) {
        order.push_back(1);
        Image image;
        ASSERT_NO_FATAL_FAILURE(ReadBackbuffer(context, image));
        ExpectPixel(image.At(8, 8), {0, 1, 0, 1});
        ASSERT_NO_FATAL_FAILURE(PaintMarker(context, {1, 0, 0, 1}));
    });
    rendering->SetOverlayCallbacks([&](RenderingContext& context, const FrameContext&) {
        order.push_back(2);
        Image image;
        ASSERT_NO_FATAL_FAILURE(ReadBackbuffer(context, image));
        ExpectPixel(image.At(8, 8), {1, 0, 0, 1});
        ASSERT_NO_FATAL_FAILURE(PaintMarker(context, {0, 0, 1, 1}));
        ASSERT_NO_FATAL_FAILURE(ReadBackbuffer(context, image));
        ExpectPixel(image.At(8, 8), {0, 0, 1, 1});
        // Screen-line pixels outside the overlay marker must survive in all modes.
        ExpectPixel(image.At(24, 8), {0, 1, 0, 1});
    }, {});
    for (auto mode : {RenderDebugView::Final, RenderDebugView::Albedo, RenderDebugView::Normals,
                      RenderDebugView::Depth, RenderDebugView::Final}) {
        order.clear();
        ASSERT_TRUE(rendering->SetDebugView(mode));
        world.debugDraw.ScreenLine({4, 8.5f}, {40, 8.5f}, {0, 1, 0, 1});
        UINT before = 0, after = 0;
        ASSERT_HRESULT_SUCCEEDED(rendering->GetContext().GetSwapChain()->GetLastPresentCount(&before));
        rendering->Render({});
        ASSERT_HRESULT_SUCCEEDED(rendering->GetContext().GetSwapChain()->GetLastPresentCount(&after));
        EXPECT_EQ(after, before + 1) << "Exactly one Present must contain scene, gizmo and both overlays";
        EXPECT_EQ(order, (std::vector<int>{1, 2}));
        EXPECT_EQ(rendering->GetDiagnostics().lifecycle, RenderingLifecycle::Ready);
        EXPECT_TRUE(world.debugDraw.GetScreenLines().empty());
        ASSERT_NO_FATAL_FAILURE(ExpectNoHazards(rendering->GetContext().GetDevice()));
    }
    rendering->ClearHostOverlayCallbacks();
    rendering->ClearOverlayCallbacks();
    host.Shutdown();
}

std::filesystem::path Executable() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot resolve test executable path");
    path.resize(length);
    return path;
}

struct TemporaryDeployment {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-present-startup-" + NewUuid());
    TemporaryDeployment() { std::filesystem::create_directories(root); }
    ~TemporaryDeployment() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

// Executed only by the parent test below, from a disposable executable-relative
// asset deployment. Engine shader roots are intentionally immutable in-process.
TEST(RenderDebugNative, DISABLED_PresentShaderFailureChild) {
    const auto root = AssetPathResolver::GetRoot();
    ASSERT_TRUE(std::filesystem::exists(root / "present-failure-mode.txt"));
    std::string mode;
    std::ifstream(root / "present-failure-mode.txt") >> mode;
    ASSERT_TRUE(mode == "missing" || mode == "invalid");
    NativeFiles files;
    ASSERT_NE(files.window, nullptr);
    RenderingSystem renderer;
    const NativeSurface surface{files.window, InitialWidth, InitialHeight};
    ASSERT_FALSE(renderer.Initialize(surface, true));
    const auto failed = renderer.GetDiagnostics();
    ASSERT_EQ(failed.lifecycle, RenderingLifecycle::Failed);
    ASSERT_TRUE(failed.failure);
    EXPECT_EQ(failed.failure->pass, "PresentPass");
    EXPECT_EQ(failed.failure->path.filename(), "DebugDraw.hlsl");
    EXPECT_EQ(failed.failure->operation, mode == "missing" ? "ReadShaderSource" : "D3DCompile");
    EXPECT_FALSE(failed.failure->detail.empty());
    ASSERT_TRUE(failed.failure->shader);
    EXPECT_EQ(failed.failure->shader->stage, "vertex");
    EXPECT_EQ(failed.failure->shader->entryPoint, "VSMain");
    if (mode == "invalid") EXPECT_NE(failed.failure->detail.find("present_startup_fixture"), std::string::npos);
    // DebugDraw is acquired only after the backbuffer, presentation buffer and
    // all three PresentPass shader variants. Failure exercises real rollback of
    // partial PresentPass acquisition plus the system's remaining pass owners.
    EXPECT_EQ(renderer.GetRenderingContext().GetDevice(), nullptr);
    renderer.Shutdown();
    renderer.Shutdown();
    EXPECT_EQ(renderer.GetDiagnostics().failure->detail, failed.failure->detail);
    std::filesystem::copy_file(root / "DebugDraw.valid.hlsl", root / "engine/shaders/DebugDraw.hlsl",
                               std::filesystem::copy_options::overwrite_existing);
    ASSERT_TRUE(renderer.Initialize(surface, true));
    EXPECT_EQ(renderer.GetDiagnostics().lifecycle, RenderingLifecycle::Ready);
    auto& world = renderer.GetWorld();
    world.camera = FPSCamera{1, float(InitialWidth) / InitialHeight, 0.1f, 100};
    world.directionalLight = {{0, 0, 0}, {0, -1, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};
    unsigned captured = 0;
    renderer.RenderFrame({}, [&](RenderingContext& context, const FrameContext&) {
        Image image;
        ASSERT_NO_FATAL_FAILURE(ReadBackbuffer(context, image));
        ExpectPixel(image.At(InitialWidth / 2, InitialHeight / 2), {0, 0, 0, 1});
        ++captured;
    });
    EXPECT_EQ(captured, 1u);
    EXPECT_EQ(renderer.GetDiagnostics().lifecycle, RenderingLifecycle::Ready);
    EXPECT_EQ(failed.lifecycle, RenderingLifecycle::Failed);
    ASSERT_NO_FATAL_FAILURE(ExpectNoHazards(renderer.GetRenderingContext().GetDevice()));
    renderer.Shutdown();
}

TEST(RenderDebugNative, MissingAndInvalidRequiredPresentShaderInTemporaryDeploymentUnwindAndRestart) {
    const auto executable = Executable();
    for (const auto* mode : {"missing", "invalid"}) {
        SCOPED_TRACE(mode);
        TemporaryDeployment temporary;
        const auto child = temporary.root / executable.filename();
        std::filesystem::copy_file(executable, child);
        // Use the exact runtime DLLs from the parent deployment. No checked-in
        // shader or normal build output is modified to provoke these failures.
        for (const auto& entry : std::filesystem::directory_iterator(executable.parent_path())) {
            if (entry.is_regular_file() && entry.path().extension() == L".dll")
                std::filesystem::copy_file(entry.path(), temporary.root / entry.path().filename());
        }
        std::filesystem::create_directories(temporary.root / "engine/shaders");
        std::filesystem::copy(std::filesystem::path(SRI_SOURCE_ROOT) / "engine/shaders", temporary.root / "engine/shaders",
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(temporary.root / "engine/shaders/DebugDraw.hlsl", temporary.root / "DebugDraw.valid.hlsl");
        std::ofstream(temporary.root / "present-failure-mode.txt") << mode;
        if (std::string(mode) == "missing") std::filesystem::remove(temporary.root / "engine/shaders/DebugDraw.hlsl");
        else std::ofstream(temporary.root / "engine/shaders/DebugDraw.hlsl") << "#error present_startup_fixture invalid required shader\n";
        std::wstring command = L"\"" + child.wstring() +
            L"\" --gtest_also_run_disabled_tests --gtest_filter=RenderDebugNative.DISABLED_PresentShaderFailureChild";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto logPath = temporary.root / "child.log";
        HANDLE output = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(output, INVALID_HANDLE_VALUE);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            CloseHandle(output);
            FAIL() << "Cannot open child standard input";
        }
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = output;
        startup.hStdInput = input;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                            temporary.root.c_str(), &startup, &process);
        const DWORD createError = GetLastError();
        CloseHandle(output);
        CloseHandle(input);
        ASSERT_TRUE(created) << createError;
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, 120000);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 10000);
        }
        DWORD exitCode = 1;
        const BOOL gotExitCode = GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        ASSERT_EQ(wait, WAIT_OBJECT_0) << "Temporary-deployment child timed out";
        ASSERT_TRUE(gotExitCode);
        std::ifstream childLog(logPath, std::ios::binary);
        const std::string details((std::istreambuf_iterator<char>(childLog)), std::istreambuf_iterator<char>());
        EXPECT_EQ(exitCode, 0u) << "Required Present shader failure child failed:\n" << details;
    }
}
} // namespace
