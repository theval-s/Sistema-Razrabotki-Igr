#include <engine/core/Uuid.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
using namespace engine;

struct TemporaryShader {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / ("sri-render-diagnostics-" + NewUuid());
    std::filesystem::path path = directory / "broken.hlsl";
    TemporaryShader() {
        std::filesystem::create_directories(directory);
        std::ofstream(path) << "float4 PSMain() : SV_TARGET { return definitely_not_declared; }";
    }
    ~TemporaryShader() { std::error_code error; std::filesystem::remove_all(directory, error); }
};

TEST(RenderingDiagnostics, HandleMapClearDestroysValuesAndPreservesGenerationHistory) {
    HandleMap<std::shared_ptr<int>> values;
    const auto first = values.Add(std::make_shared<int>(1));
    const auto removed = values.Add(std::make_shared<int>(2));
    const auto last = values.Add(std::make_shared<int>(3));
    std::weak_ptr<int> lifetime = values.Get(first);
    ASSERT_TRUE(values.Remove(removed));
    values.Clear();
    values.Clear();
    EXPECT_EQ(values.Size(), 0u);
    EXPECT_TRUE(lifetime.expired());
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 3; ++i) (void)values.Add(std::make_shared<int>(i));
        EXPECT_EQ(values.Size(), 3u);
        for (auto stale : {first, removed, last}) {
            EXPECT_FALSE(values.Contains(stale));
            EXPECT_EQ(values.TryGet(stale), nullptr);
            EXPECT_FALSE(values.Remove(stale));
        }
        EXPECT_EQ(values.Size(), 3u);
        values.Clear();
    }
}

TEST(RenderingDiagnostics, FailureSnapshotSurvivesCleanupAndFreshAttemptClearsIt) {
    RenderingSystem renderer;
    EXPECT_EQ(renderer.GetDiagnostics().lifecycle, RenderingLifecycle::Uninitialized);
    EXPECT_FALSE(renderer.Initialize({}, false));
    const auto snapshot = renderer.GetDiagnostics();
    ASSERT_EQ(snapshot.lifecycle, RenderingLifecycle::Failed);
    ASSERT_TRUE(snapshot.failure);
    EXPECT_EQ(snapshot.failure->operation, "ValidateSurface");
    EXPECT_FALSE(snapshot.failure->detail.empty());
    int resizeCalls = 0;
    EXPECT_FALSE(renderer.Reinitialize({}, [&] { ++resizeCalls; }));
    renderer.RenderFrame({}, [&](auto&, const auto&) { ADD_FAILURE() << "Failed renderer ran an overlay"; });
    EXPECT_EQ(resizeCalls, 0);
    renderer.Shutdown();
    renderer.Shutdown();
    EXPECT_EQ(renderer.GetDiagnostics().failure->detail, snapshot.failure->detail);
    // A non-null window with zero extent needs no real HWND and must do no GPU work.
    EXPECT_FALSE(renderer.Initialize({reinterpret_cast<void*>(1), 0, 32}, false));
    EXPECT_EQ(renderer.GetDiagnostics().lifecycle, RenderingLifecycle::SurfaceUnavailable);
    EXPECT_FALSE(renderer.GetDiagnostics().failure);
    EXPECT_EQ(snapshot.lifecycle, RenderingLifecycle::Failed);
    EXPECT_EQ(snapshot.failure->operation, "ValidateSurface");
    EXPECT_EQ(renderer.GetRenderingContext().GetDevice(), nullptr);
}

TEST(RenderingDiagnostics, DebugSelectionValidatesAndResetsOnlyAtFreshInitialize) {
    RenderingSystem renderer;
    EXPECT_EQ(renderer.GetDebugView(), RenderDebugView::Final);
    for (auto mode : {RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Depth, RenderDebugView::Final}) {
        EXPECT_TRUE(renderer.SetDebugView(mode));
        EXPECT_FALSE(renderer.SetDebugView(static_cast<RenderDebugView>(99)));
        EXPECT_EQ(renderer.GetDebugView(), mode);
    }
    ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Depth));
    renderer.Shutdown();
    EXPECT_EQ(renderer.GetDebugView(), RenderDebugView::Depth);
    EXPECT_FALSE(renderer.Initialize({reinterpret_cast<void*>(1), 0, 0}, false));
    EXPECT_EQ(renderer.GetDebugView(), RenderDebugView::Final);
}

TEST(RenderingDiagnostics, SubsystemPublishesFailedStartupAndClearsDebugOnEveryExit) {
    Engine engine;
    auto* rendering = engine.AddSubsystem<RenderingSubsystem>();
    ASSERT_NE(rendering, nullptr);
    EXPECT_FALSE(engine.Initialize());
    const auto snapshot = rendering->GetDiagnostics();
    ASSERT_EQ(snapshot.lifecycle, RenderingLifecycle::Failed);
    ASSERT_TRUE(snapshot.failure);
    rendering->GetWorld().debugDraw.Line({0, 0, 0}, {1, 0, 0}, {1, 1, 1, 1});
    ASSERT_FALSE(rendering->GetWorld().debugDraw.GetLines().empty());
    rendering->Render({});
    EXPECT_TRUE(rendering->GetWorld().debugDraw.GetLines().empty());
    engine.Shutdown();
    EXPECT_EQ(rendering->GetDiagnostics().failure->operation, snapshot.failure->operation);
}

TEST(RenderingDiagnostics, TemporaryShaderFailureOwnsCompilerAndVariantDetails) {
    RenderingFailure snapshot;
    {
        TemporaryShader shader;
        try {
            (void)ShaderContext::CompileCpu(shader.path, ShaderType::Pixel,
                {.entryPoint = "PSMain", .defines = {{"DIAGNOSTICS_FIXTURE", "7"}}});
            FAIL() << "Invalid shader compiled";
        } catch (const RenderingException& error) {
            snapshot = error.GetFailure();
            EXPECT_EQ(snapshot.path, shader.path);
            ASSERT_TRUE(snapshot.shader);
            EXPECT_EQ(snapshot.shader->stage, "pixel");
            EXPECT_EQ(snapshot.shader->entryPoint, "PSMain");
            EXPECT_EQ(snapshot.shader->target, "ps_5_0");
            ASSERT_EQ(snapshot.shader->defines.size(), 1u);
            EXPECT_EQ(snapshot.shader->defines[0].second, "7");
            EXPECT_NE(snapshot.detail.find("definitely_not_declared"), std::string::npos);
            EXPECT_TRUE(snapshot.nativeError);
        }
    }
    EXPECT_FALSE(snapshot.summary.empty());
    EXPECT_NE(snapshot.detail.find("definitely_not_declared"), std::string::npos);
}

TEST(RenderingDiagnostics, EngineZeroExtentStartupRetainsUnavailableSnapshot) {
    EngineConfig config;
    config.surface = {reinterpret_cast<void*>(1), 0, 32};
    Engine engine(config);
    auto* rendering = engine.AddSubsystem<RenderingSubsystem>();
    ASSERT_NE(rendering, nullptr);
    EXPECT_FALSE(engine.Initialize());
    EXPECT_EQ(rendering->GetDiagnostics().lifecycle, RenderingLifecycle::SurfaceUnavailable);
    EXPECT_FALSE(rendering->GetDiagnostics().failure);
    EXPECT_EQ(rendering->GetContext().GetDevice(), nullptr);
}

TEST(RenderingDiagnostics, MissingTemporaryShaderReportsOriginalPath) {
    TemporaryShader shader;
    const auto missing = shader.directory / "missing.hlsl";
    try {
        (void)ShaderContext::CompileCpu(missing, ShaderType::Vertex);
        FAIL() << "Missing shader compiled";
    } catch (const RenderingException& error) {
        EXPECT_EQ(error.GetFailure().path, missing);
        EXPECT_FALSE(error.GetFailure().detail.empty());
        ASSERT_TRUE(error.GetFailure().shader);
        EXPECT_EQ(error.GetFailure().shader->entryPoint, "VSMain");
    }
}

class DiagnosticsWarp : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    RenderingResourceManager manager;
    void SetUp() override {
        ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        manager.Initialize(device.Get());
    }
    void TearDown() override { context->ClearState(); manager.Shutdown(); }
};

TEST_F(DiagnosticsWarp, InvalidResourceAfterSharedInitializationUnwindsAndCanRestart) {
    RenderingResources resources(32, 32);
    ASSERT_NO_THROW(resources.Initialize(manager));
    EXPECT_TRUE(resources.frameCBuffer.GetHandle());
    EXPECT_TRUE(resources.defaultAlbedoTexture_);
    EXPECT_THROW((void)manager.CreateBuffer({.type = BufferType::Constant, .size = 0}, nullptr), RenderingException);
    resources.Destroy(manager);
    resources.Destroy(manager);
    EXPECT_FALSE(resources.frameCBuffer.GetHandle());
    EXPECT_FALSE(resources.defaultAlbedoTexture_);
    manager.Shutdown();
    manager.Initialize(device.Get());
    ASSERT_NO_THROW(resources.Initialize(manager));
    resources.Destroy(manager);
}

TEST_F(DiagnosticsWarp, ResourceHandlesFromPreviousDeviceSessionCannotAliasOrDestroyNewResources) {
    const auto compiled = ShaderContext::CompileSource(
        "float4 PSMain() : SV_TARGET { return float4(1, 0, 0, 1); }",
        "diagnostics-restart.hlsl", ShaderType::Pixel);
    struct Resources {
        BufferHandle buffer;
        TextureHandle texture;
        ShaderHandle shader;
        MeshHandle mesh;
        MaterialHandle material;
        MaterialInstanceHandle instance;
    };
    const auto create = [&] {
        Resources result;
        result.buffer = manager.CreateBuffer({.type = BufferType::Constant, .size = 16}, nullptr);
        result.texture = manager.CreateTexture({.width = 1, .height = 1,
            .format = DXGI_FORMAT_R8G8B8A8_UNORM, .usage = TextureUsage::ShaderResource});
        result.shader = manager.RegisterCompiledShader(compiled);
        const std::array<Vector3, 3> vertices{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
        const std::array<uint32_t, 3> indices{0, 1, 2};
        const auto vb = manager.CreateVertexBuffer<Vector3>(vertices);
        const auto ib = manager.CreateIndexBuffer<uint32_t>(indices);
        VertexLayout layout;
        layout.Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0);
        result.mesh = manager.CreateMesh(vb, ib, std::move(layout), 3);
        result.material = manager.CreateMaterial({.defaults = {.albedoTexture = result.texture}});
        result.instance = manager.CreateMaterialInstance(result.material);
        return result;
    };
    const auto old = create();
    ASSERT_TRUE(old.material);
    ASSERT_TRUE(old.instance);
    manager.Shutdown();
    manager.Shutdown();
    manager.Initialize(device.Get());
    const auto current = create();
    EXPECT_NE(old.texture, current.texture);
    EXPECT_NE(old.shader, current.shader);
    EXPECT_NE(old.mesh, current.mesh);
    EXPECT_NE(old.material, current.material);
    EXPECT_NE(old.instance, current.instance);
    EXPECT_EQ(manager.TryGetMaterial(old.material), nullptr);
    EXPECT_EQ(manager.TryGetMaterialInstance(old.instance), nullptr);
    EXPECT_FALSE(manager.DestroyBuffer(old.buffer));
    EXPECT_FALSE(manager.DestroyTexture(old.texture));
    EXPECT_FALSE(manager.DestroyShader(old.shader));
    EXPECT_FALSE(manager.DestroyMesh(old.mesh));
    EXPECT_FALSE(manager.DestroyMaterialInstance(old.instance));
    EXPECT_FALSE(manager.DestroyMaterial(old.material));
    EXPECT_TRUE(manager.DestroyBuffer(current.buffer));
    EXPECT_TRUE(manager.DestroyTexture(current.texture));
    EXPECT_TRUE(manager.DestroyShader(current.shader));
    EXPECT_TRUE(manager.DestroyMesh(current.mesh));
    EXPECT_TRUE(manager.DestroyMaterialInstance(current.instance));
    EXPECT_TRUE(manager.DestroyMaterial(current.material));
}

TEST_F(DiagnosticsWarp, GBufferResizeFailureDoesNotPublishPartialTargets) {
    RenderingContext unusedContext;
    RenderingResources resources(16, 16);
    GBufferPass pass;
    ASSERT_TRUE(pass.Reinitialize(manager, unusedContext, resources));
    ASSERT_TRUE(resources.gBuffer.albedoTexture);
    // WARP may accept extents beyond the documented feature-level limit. Zero
    // width is unconditionally invalid for a texture, independent of the driver.
    resources.screenWith = 0;
    EXPECT_THROW((void)pass.Reinitialize(manager, unusedContext, resources), RenderingException);
    EXPECT_FALSE(resources.gBuffer.albedoTexture);
    EXPECT_FALSE(resources.gBuffer.normalTexture);
    EXPECT_FALSE(resources.gBuffer.depthTexture);
    pass.Destroy(manager, unusedContext, resources);
    pass.Destroy(manager, unusedContext, resources);
    resources.screenWith = 16;
    ASSERT_TRUE(pass.Reinitialize(manager, unusedContext, resources));
    pass.Destroy(manager, unusedContext, resources);
}

TEST_F(DiagnosticsWarp, PassStartupFailureAfterShaderAcquisitionCanBeDestroyedAndRetried) {
    RenderingContext unusedContext;
    RenderingResources resources(0, 16);
    GBufferPass pass;
    // Required shader compilation succeeds before this invalid texture extent is
    // encountered. Initialize must roll those acquired shaders back itself.
    try {
        pass.Initialize(manager, unusedContext, resources);
        FAIL() << "Zero-width G-buffer unexpectedly initialized";
    } catch (const RenderingException& error) {
        EXPECT_EQ(error.GetFailure().operation, "CreateTexture2D");
        EXPECT_TRUE(error.GetFailure().nativeError);
    }
    EXPECT_FALSE(resources.gBuffer.albedoTexture);
    pass.Destroy(manager, unusedContext, resources);
    pass.Destroy(manager, unusedContext, resources);
    resources.screenWith = 16;
    ASSERT_NO_THROW(pass.Initialize(manager, unusedContext, resources));
    EXPECT_TRUE(resources.gBuffer.depthTexture);
    pass.Destroy(manager, unusedContext, resources);
}

TEST_F(DiagnosticsWarp, FailedInputLayoutIsNotCachedAndCorrectedLayoutWorks) {
    const auto compiled = ShaderContext::CompileSource(
        "float4 VSMain(float3 p : POSITION) : SV_POSITION { return float4(p, 1); }",
        "diagnostics-layout.hlsl", ShaderType::Vertex);
    InputLayoutCache cache;
    const auto shader = ShaderContext::FinalizeOnDevice(compiled, device.Get(), cache);
    VertexLayout empty;
    const auto missing = cache.RegisterMeshLayout(std::move(empty));
    EXPECT_THROW((void)cache.GetInputLayout(device.Get(), missing, shader.layoutHandle), RenderingException);
    EXPECT_THROW((void)cache.GetInputLayout(device.Get(), missing, shader.layoutHandle), RenderingException);
    VertexLayout valid;
    valid.Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0);
    const auto mesh = cache.RegisterMeshLayout(std::move(valid));
    EXPECT_NE(cache.GetInputLayout(device.Get(), mesh, shader.layoutHandle), nullptr);
}

TEST_F(DiagnosticsWarp, LayoutCacheClearRejectsPreviousSessionHandlesAfterReregistration) {
    const auto compiled = ShaderContext::CompileSource(
        "float4 VSMain(float3 p : POSITION) : SV_POSITION { return float4(p, 1); }",
        "diagnostics-layout-restart.hlsl", ShaderType::Vertex);
    InputLayoutCache cache;
    const auto registerMesh = [&] {
        VertexLayout layout;
        layout.Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0);
        return cache.RegisterMeshLayout(std::move(layout));
    };
    const auto oldShader = ShaderContext::FinalizeOnDevice(compiled, device.Get(), cache);
    const auto oldMesh = registerMesh();
    ASSERT_NE(cache.GetInputLayout(device.Get(), oldMesh, oldShader.layoutHandle), nullptr);
    cache.Clear();
    cache.Clear();
    const auto shader = ShaderContext::FinalizeOnDevice(compiled, device.Get(), cache);
    const auto mesh = registerMesh();
    EXPECT_NE(oldShader.layoutHandle, shader.layoutHandle);
    EXPECT_NE(oldMesh, mesh);
    EXPECT_THROW((void)cache.GetInputLayout(device.Get(), oldMesh, oldShader.layoutHandle), RenderingException);
    EXPECT_THROW((void)cache.GetInputLayout(device.Get(), oldMesh, shader.layoutHandle), RenderingException);
    EXPECT_THROW((void)cache.GetInputLayout(device.Get(), mesh, oldShader.layoutHandle), RenderingException);
    EXPECT_NE(cache.GetInputLayout(device.Get(), mesh, shader.layoutHandle), nullptr);
}

// These boundaries require the production swap chain. The resource tests above
// use WARP; no test-only device injection is needed by the renderer itself.
class DiagnosticsNative : public ::testing::Test {
protected:
    HWND window = nullptr;
    RenderingSystem renderer;
    NativeSurface surface;
    void SetUp() override {
        ComPtr<ID3D11Device> probe;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &probe, nullptr, nullptr)))
            GTEST_SKIP() << "No hardware D3D11 device for production swap-chain boundary";
        window = CreateWindowExW(0, L"STATIC", L"Rendering diagnostics", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        surface = {window, 32, 32};
        ASSERT_TRUE(renderer.Initialize(surface, false));
        auto& world = renderer.GetWorld();
        world.camera.fov = DirectX::XM_PIDIV4;
        world.camera.aspectRatio = 1.0f;
        world.camera.nearPlane = 0.1f;
        world.camera.farPlane = 100.0f;
        world.directionalLight = {};
        world.directionalLight.direction = {0, -1, 0};
        world.directionalLight.type = LightInfo::Directional;
    }
    void TearDown() override { renderer.Shutdown(); if (window) DestroyWindow(window); }
};

TEST_F(DiagnosticsNative, ResizeCallbackShaderFailureLatchesFullFirstFailure) {
    TemporaryShader shader;
    ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Depth));
    int callbacks = 0;
    EXPECT_FALSE(renderer.Reinitialize(surface, [&] {
        ++callbacks;
        (void)ShaderContext::CompileCpu(shader.path, ShaderType::Pixel);
    }));
    const auto first = renderer.GetDiagnostics();
    ASSERT_EQ(first.lifecycle, RenderingLifecycle::Failed);
    ASSERT_TRUE(first.failure);
    EXPECT_EQ(first.failure->path, shader.path);
    EXPECT_NE(first.failure->detail.find("definitely_not_declared"), std::string::npos);
    EXPECT_FALSE(renderer.Reinitialize(surface, [&] { ++callbacks; }));
    renderer.RenderFrame({}, [&](auto&, const auto&) { ++callbacks; });
    EXPECT_EQ(callbacks, 1);
    // Runtime failure keeps the device owned until workers can be quiesced.
    EXPECT_NE(renderer.GetRenderingContext().GetDevice(), nullptr);
    renderer.Shutdown();
    EXPECT_EQ(renderer.GetDiagnostics().failure->detail, first.failure->detail);
    EXPECT_EQ(renderer.GetDebugView(), RenderDebugView::Depth);
    ASSERT_TRUE(renderer.Initialize(surface, false));
    EXPECT_EQ(renderer.GetDebugView(), RenderDebugView::Final);
    EXPECT_FALSE(renderer.GetDiagnostics().failure);
}

TEST_F(DiagnosticsNative, FrameMapFailureStopsRenderingAndPreservesNativeCode) {
    const auto handle = renderer.GetResourceManager().CreateBuffer(
        {.type = BufferType::Constant, .usage = BufferUsage::Default, .size = 16}, nullptr);
    int overlays = 0;
    renderer.RenderFrame({}, [&](RenderingContext& context, const auto&) {
        ++overlays;
        context.UpdateCBuffer(handle, DirectX::SimpleMath::Vector4{});
        ADD_FAILURE() << "Map of a non-dynamic buffer unexpectedly succeeded";
    });
    const auto snapshot = renderer.GetDiagnostics();
    ASSERT_EQ(snapshot.lifecycle, RenderingLifecycle::Failed);
    ASSERT_TRUE(snapshot.failure);
    EXPECT_EQ(snapshot.failure->operation, "Map");
    ASSERT_TRUE(snapshot.failure->nativeError);
    EXPECT_TRUE(FAILED(static_cast<HRESULT>(*snapshot.failure->nativeError)));
    renderer.RenderFrame({}, [&](auto&, const auto&) { ++overlays; });
    EXPECT_EQ(overlays, 1);
}

TEST_F(DiagnosticsNative, SuspendRestorePreservesModeAndDoesNoOverlayWork) {
    ASSERT_TRUE(renderer.SetDebugView(RenderDebugView::Normals));
    auto minimized = surface;
    minimized.width = 0;
    int callbacks = 0;
    EXPECT_FALSE(renderer.Reinitialize(minimized, [&] { ++callbacks; }));
    EXPECT_EQ(renderer.GetDiagnostics().lifecycle, RenderingLifecycle::SurfaceUnavailable);
    renderer.RenderFrame({}, [&](auto&, const auto&) { ++callbacks; });
    EXPECT_EQ(callbacks, 0);
    ASSERT_TRUE(renderer.Reinitialize(surface));
    EXPECT_EQ(renderer.GetDiagnostics().lifecycle, RenderingLifecycle::Ready);
    EXPECT_EQ(renderer.GetDebugView(), RenderDebugView::Normals);
    EXPECT_FALSE(renderer.GetDiagnostics().failure);
}

TEST_F(DiagnosticsNative, EngineShutdownQuiescesBothAssetQueuesAndEvictsGpuRecordsForRestart) {
    renderer.Shutdown();
    EngineConfig config;
    config.surface = surface;
    Engine engine(config);
    auto* rendering = engine.AddSubsystem<RenderingSubsystem>();
    ASSERT_NE(rendering, nullptr);
    ASSERT_TRUE(engine.Initialize());
    constexpr auto source = "float4 PSMain() : SV_TARGET { return float4(1, 0, 0, 1); }";
    auto& global = engine.GetAssets();
    auto& scene = engine.GetSceneAssets();
    ASSERT_TRUE(global.LoadShaderSource("game/diagnostics-global.hlsl", source, ShaderType::Pixel));
    ASSERT_TRUE(scene.LoadShaderSource("game/diagnostics-scene.hlsl", source, ShaderType::Pixel));
    const auto globalHandle = global.RequestShaderSource("game/diagnostics-global.hlsl", source, ShaderType::Pixel);
    const auto sceneHandle = scene.RequestShaderSource("game/diagnostics-scene.hlsl", source, ShaderType::Pixel);
    ASSERT_TRUE(global.IsReady(globalHandle));
    ASSERT_TRUE(scene.IsReady(sceneHandle));
    // Leave both queues with work when shutdown starts. Workers must finish before
    // the device disappears, and no Ready GPU record may survive that device.
    (void)global.RequestShaderSource("game/diagnostics-pending-global.hlsl", source, ShaderType::Pixel);
    (void)scene.RequestShaderSource("game/diagnostics-pending-scene.hlsl", source, ShaderType::Pixel);
    engine.Shutdown();
    EXPECT_EQ(rendering->GetContext().GetDevice(), nullptr);
    EXPECT_FALSE(global.IsReady(globalHandle));
    EXPECT_FALSE(scene.IsReady(sceneHandle));
    ASSERT_TRUE(engine.Initialize());
    EXPECT_FALSE(global.IsReady(globalHandle));
    EXPECT_FALSE(scene.IsReady(sceneHandle));
    ASSERT_TRUE(global.LoadShaderSource("game/diagnostics-global.hlsl", source, ShaderType::Pixel));
    ASSERT_TRUE(scene.LoadShaderSource("game/diagnostics-scene.hlsl", source, ShaderType::Pixel));
    engine.Shutdown();
}
} // namespace
