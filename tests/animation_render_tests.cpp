#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderExtractionSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scene/SceneQueries.hpp>

#include <d3d11sdklayers.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {
using namespace engine;
using DirectX::SimpleMath::Vector4;

constexpr std::uint32_t Width = 640, Height = 480;
constexpr const char* BotMesh = "game/models/TestBot/model.skinned-mesh.json";
constexpr const char* BotSkeleton = "game/models/TestBot/model.skeleton.json";
constexpr const char* IdleClip = "game/models/TestBot/idle.animation.json";
constexpr const char* WalkClip = "game/models/TestBot/walk.animation.json";
constexpr const char* DefaultMaterial = "engine/primitives/default.material.json";
constexpr const char* DefaultInstance = "engine/primitives/default.material-instance.json";
constexpr std::array BotIds{"b52e7e3e-920c-48b7-b782-8f403e062004", "b52e7e3e-920c-48b7-b782-8f403e062005"};

struct AnimationRenderFixtureFiles {
    std::filesystem::path previousRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path gameRoot = std::filesystem::path(SRI_SOURCE_ROOT) / "example_game";
    std::filesystem::path emptyScene = std::filesystem::temp_directory_path() / ("sri-animation-render-" + NewUuid() + ".scene.json");
    HWND window = nullptr;

    AnimationRenderFixtureFiles() {
        AssetPathResolver::SetGameRoot(gameRoot);
        window = CreateWindowExW(0, L"STATIC", L"Animation render integration", WS_OVERLAPPEDWINDOW,
                                 0, 0, Width, Height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        std::ofstream(emptyScene, std::ios::binary) << R"({"version":1,"entities":[]})";
    }
    ~AnimationRenderFixtureFiles() {
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        std::filesystem::remove(emptyScene, ignored);
    }
};

void ExpectNoDebugErrors(ID3D11InfoQueue* queue) {
    if (!queue) return;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        ASSERT_TRUE(SUCCEEDED(queue->GetMessage(i, nullptr, &size)));
        std::vector<std::byte> bytes(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        ASSERT_TRUE(SUCCEEDED(queue->GetMessage(i, message, &size)));
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR) << message->pDescription;
    }
    queue->ClearStoredMessages();
}

void LoadFixtureAssets(Engine& host) {
    // Complete the same requests issued by scene population before sampling or
    // extraction. In particular, never mix host GetAssets() handles into a scene.
    auto& assets = host.GetSceneAssets();
    ASSERT_TRUE(assets.LoadMesh("engine/primitives/plane/model.obj"));
    ASSERT_TRUE(assets.LoadMaterial(DefaultMaterial));
    ASSERT_TRUE(assets.LoadMaterialInstance(DefaultInstance));
    ASSERT_TRUE(assets.LoadSkinnedMesh(BotMesh));
    ASSERT_TRUE(assets.LoadSkeleton(BotSkeleton));
    ASSERT_TRUE(assets.LoadAnimationClip(IdleClip));
    ASSERT_TRUE(assets.LoadAnimationClip(WalkClip));
    assets.Update({});
}

bool MatricesDiffer(const std::vector<Matrix>& a, const std::vector<Matrix>& b) {
    if (a.size() != b.size()) return true;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::abs(a[i].m[r][c] - b[i].m[r][c]) > 1e-4f) return true;
    return false;
}

void ExpectMatrixNear(const Matrix& a, const Matrix& b) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            EXPECT_TRUE(std::isfinite(a.m[r][c]));
            EXPECT_NEAR(a.m[r][c], b.m[r][c], 3e-4f);
        }
}

std::vector<RenderItem> EntityItems(const RenderWorld& world, const Matrix& transform) {
    std::vector<RenderItem> items;
    for (const auto& item : world.sceneItems)
        if (item.skinning && item.worldMatrix == transform) items.push_back(item);
    return items;
}

void SelectExtractedBots(RenderWorld& world) {
    world.selectedSceneItems.clear();
    for (const auto& item : world.sceneItems)
        if (item.skinning) world.selectedSceneItems.push_back(item);
}

void ExpectPosedQueries(Engine& host, entt::entity entity) {
    const auto* renderer = host.GetScene().TryGetComponent<SkinnedMeshRenderer>(entity);
    ASSERT_NE(renderer, nullptr);
    ASSERT_TRUE(renderer->posedBounds);
    const auto& bounds = *renderer->posedBounds;
    const auto center = Vector3::Transform((bounds.min + bounds.max) * 0.5f, host.GetScene().GetWorldMatrix(entity));
    const auto sphereHits = QuerySphere(host.GetScene(), host.GetSceneAssets(), center, 0.01f);
    EXPECT_NE(std::find(sphereHits.begin(), sphereHits.end(), entity), sphereHits.end());
    const auto rayHits = RaycastAll(host.GetScene(), host.GetSceneAssets(), center + Vector3(0, 0, 20),
                                   Vector3(0, 0, -1), 40);
    EXPECT_NE(std::find_if(rayHits.begin(), rayHits.end(), [&](const RayHit& hit) {
        return hit.entity == entity && std::isfinite(hit.distance);
    }), rayHits.end());
}

struct Image {
    UINT width = 0, height = 0, bytesPerPixel = 0;
    std::vector<std::uint8_t> pixels;
};

void ReadTexture(RenderingContext& context, ID3D11Texture2D* source, Image& image) {
    ASSERT_NE(source, nullptr);
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    ASSERT_EQ(desc.SampleDesc.Count, 1u);
    if (desc.Format == DXGI_FORMAT_R8G8_UNORM) image.bytesPerPixel = 2;
    else {
        ASSERT_TRUE(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
        image.bytesPerPixel = 4;
    }
    image.width = desc.Width;
    image.height = desc.Height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(context.GetDevice()->CreateTexture2D(&desc, nullptr, &staging)));
    auto* dc = context.GetDeviceContext();
    dc->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto rowBytes = image.width * image.bytesPerPixel;
    image.pixels.resize(static_cast<std::size_t>(rowBytes) * image.height);
    for (UINT y = 0; y < image.height; ++y)
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * rowBytes,
                    static_cast<const std::byte*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, rowBytes);
    dc->Unmap(staging.Get(), 0);
}

struct FrameImage { Image color, selection; };

void RenderAndRead(RenderingSubsystem& rendering, const FrameContext& frame, bool selected, FrameImage& image) {
    bool captured = false;
    // Capture before Present (the swap chain uses FLIP_DISCARD). The public
    // overlay hook also lets us inspect the actual selection texture. Present
    // deliberately unbinds its inputs; residual PS state is not its public API.
    rendering.GetRendering().RenderFrame(frame, [&](RenderingContext& context, const FrameContext&) {
        ComPtr<ID3D11RenderTargetView> target;
        context.GetDeviceContext()->OMGetRenderTargets(1, &target, nullptr);
        ASSERT_TRUE(target);
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        ASSERT_TRUE(SUCCEEDED(resource.As(&texture)));
        ASSERT_NO_FATAL_FAILURE(ReadTexture(context, texture.Get(), image.color));
        if (selected) {
            const auto handle = rendering.GetRendering().GetResources().selectionMask;
            ASSERT_TRUE(handle);
            context.SetShaderResources(BindSlots::Texture::Pass0, handle);
            ComPtr<ID3D11ShaderResourceView> mask;
            context.GetDeviceContext()->PSGetShaderResources(16, 1, &mask); // Texture::Pass0
            ID3D11ShaderResourceView* empty = nullptr;
            context.GetDeviceContext()->PSSetShaderResources(16, 1, &empty);
            ASSERT_TRUE(mask) << "selection texture is unavailable";
            mask->GetResource(resource.ReleaseAndGetAddressOf());
            texture.Reset();
            ASSERT_TRUE(SUCCEEDED(resource.As(&texture)));
            ASSERT_NO_FATAL_FAILURE(ReadTexture(context, texture.Get(), image.selection));
        }
        captured = true;
    });
    ASSERT_TRUE(captured);
    ASSERT_EQ(image.color.width, Width);
    ASSERT_EQ(image.color.height, Height);
}

std::size_t SelectedPixels(const Image& image) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < image.pixels.size(); i += 2)
        count += image.pixels[i] != 0 || image.pixels[i + 1] != 0;
    return count;
}

void ReadBuffer(RenderingContext& context, ID3D11Buffer* source, std::vector<std::byte>& bytes) {
    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    ASSERT_TRUE(SUCCEEDED(context.GetDevice()->CreateBuffer(&desc, nullptr, &staging)));
    context.GetDeviceContext()->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(context.GetDeviceContext()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    bytes.resize(desc.ByteWidth);
    std::memcpy(bytes.data(), mapped.pData, bytes.size());
    context.GetDeviceContext()->Unmap(staging.Get(), 0);
}

void ExpectImportedVertexShaderOutput(RenderingSubsystem& rendering, const RenderItem& item) {
    // Execute the real default GBuffer VS with the real uploaded 60-byte vertices.
    // Stream output makes row/column transposes and wrong normal transforms fail
    // numerically, even when a lit screenshot might still look plausible.
    auto& context = rendering.GetContext();
    auto& manager = rendering.GetResourceManager();
    const auto path = AssetPathResolver::GetRoot() / "engine/shaders/MaterialTemplate.hlsl";
    const auto vs = ShaderContext::CompileCpu(path, ShaderType::Vertex,
        {.defines = {{"MATERIAL_GBUFFER_VS", "1"}, {"ENGINE_SKINNED", "1"}}});
    const auto gs = ShaderContext::CompileSource(R"(
#include "ShaderTypes.hlsl"
[maxvertexcount(3)]
void GSMain(triangle GBufferInput vertices[3], inout TriangleStream<GBufferInput> output) {
    for (uint i = 0; i < 3; ++i) output.Append(vertices[i]);
})", path, ShaderType::Geometry);
    const auto shader = manager.RegisterCompiledShader(vs);
    ASSERT_TRUE(shader);
    context.ClearState();
    context.SetShader(shader);
    context.BindMesh(item.mesh);
    auto* dc = context.GetDeviceContext();
    ComPtr<ID3D11Buffer> vertexBuffer, indexBuffer;
    UINT stride = 0, vertexOffset = 0, indexOffset = 0;
    DXGI_FORMAT indexFormat{};
    dc->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);
    dc->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
    ASSERT_TRUE(vertexBuffer && indexBuffer);
    ASSERT_EQ(stride, 60u);
    ASSERT_EQ(stride, sizeof(SkinnedVertex));
    ASSERT_EQ(vertexOffset, 0u);
    ASSERT_EQ(indexOffset, 0u);
    ASSERT_EQ(indexFormat, DXGI_FORMAT_R32_UINT);
    std::vector<std::byte> vertexBytes, indexBytes;
    ASSERT_NO_FATAL_FAILURE(ReadBuffer(context, vertexBuffer.Get(), vertexBytes));
    ASSERT_NO_FATAL_FAILURE(ReadBuffer(context, indexBuffer.Get(), indexBytes));
    ASSERT_GE(indexBytes.size(), 3 * sizeof(std::uint32_t));
    std::array<std::uint32_t, 3> indices;
    std::memcpy(indices.data(), indexBytes.data(), sizeof(indices));

    const D3D11_SO_DECLARATION_ENTRY declarations[] = {
        {0, "NORMAL", 0, 0, 3, 0}, {0, "TEXCOORD", 1, 0, 3, 0}
    };
    struct Output { Vector3 normal, position; };
    static_assert(sizeof(Output) == 24);
    const UINT outputStride = sizeof(Output);
    ComPtr<ID3D11GeometryShader> streamShader;
    ASSERT_TRUE(SUCCEEDED(context.GetDevice()->CreateGeometryShaderWithStreamOutput(
        gs.blob->GetBufferPointer(), gs.blob->GetBufferSize(), declarations, 2, &outputStride, 1,
        D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &streamShader)));
    D3D11_BUFFER_DESC outputDesc{};
    outputDesc.ByteWidth = 3 * sizeof(Output);
    outputDesc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> output;
    ASSERT_TRUE(SUCCEEDED(context.GetDevice()->CreateBuffer(&outputDesc, nullptr, &output)));
    const auto view = manager.CreateConstantBuffer<ViewBufferData>();
    const auto object = manager.CreateConstantBuffer<ObjectBufferData>();
    const auto bones = manager.CreateConstantBuffer<SkinningBufferData>();
    ViewBufferData viewData{};
    viewData.viewMatrix = viewData.projectionMatrix = Matrix::Identity;
    context.UpdateCBuffer(view, viewData);
    const ObjectBufferData objectData{item.worldMatrix, item.worldMatrix.Invert().Transpose()};
    context.UpdateCBuffer(object, objectData);
    SkinningBufferData palette;
    palette.matrices.fill(Matrix::Identity);
    ASSERT_TRUE(item.skinning);
    ASSERT_LE(item.skinning->matrices.size(), MaxSkinningJoints);
    std::copy(item.skinning->matrices.begin(), item.skinning->matrices.end(), palette.matrices.begin());
    context.UpdateCBuffer(bones, palette);
    context.BindCBuffer(BindSlots::CBuffer::View, view);
    context.BindCBuffer(BindSlots::CBuffer::Object, object);
    context.BindCBuffer(BindSlots::CBuffer::Skinning, bones);
    dc->GSSetShader(streamShader.Get(), nullptr, 0);
    const UINT offset = 0;
    dc->SOSetTargets(1, output.GetAddressOf(), &offset);
    context.RawDrawIndexed(3);
    dc->SOSetTargets(0, nullptr, nullptr);
    std::vector<std::byte> result;
    ASSERT_NO_FATAL_FAILURE(ReadBuffer(context, output.Get(), result));
    for (std::size_t i = 0; i < indices.size(); ++i) {
        SCOPED_TRACE(::testing::Message() << "vertex " << indices[i]);
        ASSERT_LE((static_cast<std::size_t>(indices[i]) + 1) * stride, vertexBytes.size());
        SkinnedVertex vertex;
        std::memcpy(&vertex, vertexBytes.data() + static_cast<std::size_t>(indices[i]) * stride, sizeof(vertex));
        Matrix blended{};
        blended *= 0;
        float weightSum = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            ASSERT_LT(vertex.joints[j], item.skinning->matrices.size());
            ASSERT_TRUE(std::isfinite(vertex.weights[j]));
            ASSERT_GE(vertex.weights[j], 0);
            weightSum += vertex.weights[j];
            blended += palette.matrices[vertex.joints[j]] * vertex.weights[j];
        }
        ASSERT_NEAR(weightSum, 1.0f, 1e-4f);
        blended *= 1.0f / weightSum;
        const auto expectedPosition = Vector4::Transform(Vector4::Transform(vertex.position, blended), item.worldMatrix);
        auto expectedNormal = Vector3::TransformNormal(vertex.normal, blended.Invert().Transpose());
        expectedNormal = Vector3::TransformNormal(expectedNormal, objectData.normalMatrix);
        expectedNormal.Normalize();
        Output actual;
        std::memcpy(&actual, result.data() + i * sizeof(Output), sizeof(actual));
        EXPECT_NEAR(actual.position.x, expectedPosition.x, 1e-3f);
        EXPECT_NEAR(actual.position.y, expectedPosition.y, 1e-3f);
        EXPECT_NEAR(actual.position.z, expectedPosition.z, 1e-3f);
        EXPECT_NEAR(actual.normal.x, expectedNormal.x, 1e-3f);
        EXPECT_NEAR(actual.normal.y, expectedNormal.y, 1e-3f);
        EXPECT_NEAR(actual.normal.z, expectedNormal.z, 1e-3f);
        EXPECT_NEAR(actual.normal.LengthSquared(), 1.0f, 1e-3f);
    }
    context.ClearState();
    EXPECT_TRUE(manager.DestroyShader(shader));
    EXPECT_TRUE(manager.DestroyBuffer(view));
    EXPECT_TRUE(manager.DestroyBuffer(object));
    EXPECT_TRUE(manager.DestroyBuffer(bones));
}

TEST(AnimationRenderIntegration, ImportedBotsAnimateSelectQueryAndReloadWithoutStaleDraws) {
    AnimationRenderFixtureFiles fixture;
    ASSERT_NE(fixture.window, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fixture.gameRoot / "scenes/animation.scene.json"));
    ASSERT_TRUE(std::filesystem::exists(fixture.emptyScene));
    {
        // Only device/swap-chain unavailability may skip. Engine/subsystem,
        // fixture import, shader compilation and rendering failures must fail.
        RenderingContext probe;
        if (!probe.Initialize({fixture.window, Width, Height}, true))
            GTEST_SKIP() << "D3D11 hardware/swap-chain initialization unavailable";
    }
    Engine host({.applicationName = "Animation render tests", .surface = {fixture.window, Width, Height},
                 .taskThreads = 2, .graphicsDebugLayer = true});
    auto* rendering = host.AddSubsystem<RenderingSubsystem>();
    auto* animation = host.AddSubsystem<AnimationSubsystem>();
    auto* extraction = host.AddSubsystem<RenderExtractionSubsystem>();
    ASSERT_TRUE(rendering && animation && extraction);
    ASSERT_TRUE(host.Initialize());
    ComPtr<ID3D11InfoQueue> infoQueue;
    rendering->GetContext().GetDevice()->QueryInterface(IID_PPV_ARGS(&infoQueue));
    ASSERT_NO_FATAL_FAILURE(ExpectNoDebugErrors(infoQueue.Get()));
    auto& world = rendering->GetWorld();
    auto& assets = host.GetSceneAssets();
    std::string error;
    ASSERT_TRUE(host.LoadScene(fixture.gameRoot / "scenes/animation.scene.json", error)) << error;
    ASSERT_NO_FATAL_FAILURE(LoadFixtureAssets(host));
    std::array<entt::entity, 2> bots;
    for (std::size_t i = 0; i < bots.size(); ++i) {
        bots[i] = host.GetScene().FindById(BotIds[i]);
        ASSERT_NE(bots[i], entt::entity{entt::null});
        ASSERT_NE(host.GetScene().TryGetComponent<SkinnedMeshRenderer>(bots[i]), nullptr);
        ASSERT_NE(host.GetScene().TryGetComponent<Animator>(bots[i]), nullptr);
    }
    const auto meshHandle = host.GetScene().TryGetComponent<SkinnedMeshRenderer>(bots[0])->mesh;
    const auto instanceHandle = host.GetScene().TryGetComponent<SkinnedMeshRenderer>(bots[0])->materialInstance;
    EXPECT_EQ(meshHandle, host.GetScene().TryGetComponent<SkinnedMeshRenderer>(bots[1])->mesh);
    EXPECT_EQ(instanceHandle, host.GetScene().TryGetComponent<SkinnedMeshRenderer>(bots[1])->materialInstance);
    const auto skeletonHandle = assets.RequestSkeleton(BotSkeleton);
    const auto idleHandle = assets.RequestAnimationClip(IdleClip);
    const auto walkHandle = assets.RequestAnimationClip(WalkClip);
    const auto materialHandle = assets.RequestMaterial(DefaultMaterial);
    const auto mesh = assets.Peek(meshHandle);
    const auto materialInstance = assets.Peek(instanceHandle);
    ASSERT_TRUE(mesh && materialInstance);
    ASSERT_TRUE(assets.Peek(materialHandle));
    ASSERT_EQ(mesh->sections.size(), 2u);
    const auto oldGpuInstance = materialInstance->instance;
    const auto oldGpuMaterial = assets.Peek(materialHandle)->material;
    const std::array oldGpuMeshes{mesh->sections[0].mesh, mesh->sections[1].mesh};
    const auto transform0 = host.GetScene().GetWorldMatrix(bots[0]);
    const auto transform1 = host.GetScene().GetWorldMatrix(bots[1]);
    EXPECT_NE(transform0, transform1);
    FrameContext frame;
    animation->LateUpdate(frame);
    extraction->LateUpdate(frame);
    const auto initialPose0 = animation->GetPose(bots[0]);
    const auto initialPose1 = animation->GetPose(bots[1]);
    ASSERT_TRUE(initialPose0 && initialPose1);
    for (const auto bot : bots) {
        const auto state = animation->GetPlaybackState(bot);
        ASSERT_TRUE(state);
        EXPECT_TRUE(state->ready) << state->error;
        EXPECT_TRUE(state->playing);
        EXPECT_TRUE(state->error.empty()) << state->error;
        EXPECT_DOUBLE_EQ(state->timeSeconds, 0);
    }
    EXPECT_NE(initialPose0.get(), initialPose1.get());
    const auto savedPose0 = initialPose0->jointModels;
    const auto savedPose1 = initialPose1->jointModels;
    const auto initial0 = EntityItems(world, transform0);
    const auto initial1 = EntityItems(world, transform1);
    ASSERT_EQ(initial0.size(), 2u);
    ASSERT_EQ(initial1.size(), 2u);
    std::array<std::vector<Matrix>, 2> savedPalettes;
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(initial0[i].mesh, mesh->sections[i].mesh);
        EXPECT_EQ(initial1[i].mesh, initial0[i].mesh);
        ASSERT_TRUE(initial0[i].skinning && initial1[i].skinning);
        EXPECT_NE(initial0[i].skinning.get(), initial1[i].skinning.get());
        ASSERT_FALSE(initial0[i].skinning->matrices.empty());
        savedPalettes[i] = initial0[i].skinning->matrices;
    }
    SelectExtractedBots(world);
    ASSERT_EQ(world.selectedSceneItems.size(), 4u);
    FrameImage initialImage;
    ASSERT_NO_FATAL_FAILURE(RenderAndRead(*rendering, frame, true, initialImage));
    EXPECT_GT(SelectedPixels(initialImage.selection), 20u);

    frame.frameIndex = 1;
    frame.deltaSeconds = frame.simulationTimeSeconds = 0.25;
    animation->LateUpdate(frame);
    extraction->LateUpdate(frame);
    const auto pose0 = animation->GetPose(bots[0]);
    const auto pose1 = animation->GetPose(bots[1]);
    ASSERT_TRUE(pose0 && pose1);
    for (const auto bot : bots) {
        const auto state = animation->GetPlaybackState(bot);
        ASSERT_TRUE(state);
        EXPECT_NEAR(state->timeSeconds, 0.25, 1e-6);
    }
    EXPECT_TRUE(MatricesDiffer(savedPose0, pose0->jointModels));
    EXPECT_TRUE(MatricesDiffer(savedPose1, pose1->jointModels));
    EXPECT_TRUE(MatricesDiffer(pose0->jointModels, pose1->jointModels));
    EXPECT_EQ(initialPose0->jointModels, savedPose0);
    EXPECT_EQ(initialPose1->jointModels, savedPose1);
    const auto posed0 = EntityItems(world, transform0);
    const auto posed1 = EntityItems(world, transform1);
    ASSERT_EQ(posed0.size(), 2u);
    ASSERT_EQ(posed1.size(), 2u);
    bool paletteChanged = false, differentInstances = false;
    for (std::size_t section = 0; section < 2; ++section) {
        EXPECT_EQ(posed0[section].mesh, initial0[section].mesh);
        EXPECT_EQ(posed1[section].mesh, initial1[section].mesh);
        ASSERT_TRUE(posed0[section].skinning && posed1[section].skinning);
        EXPECT_NE(posed0[section].skinning.get(), initial0[section].skinning.get());
        EXPECT_EQ(initial0[section].skinning->matrices, savedPalettes[section]);
        ASSERT_TRUE(mesh->sections[section].binding);
        const auto& binding = *mesh->sections[section].binding;
        ASSERT_EQ(posed0[section].skinning->matrices.size(), binding.joints.size());
        for (std::size_t joint = 0; joint < binding.joints.size(); ++joint) {
            ASSERT_LT(binding.joints[joint], pose0->jointModels.size());
            ExpectMatrixNear(posed0[section].skinning->matrices[joint],
                             binding.inverseBind[joint] * pose0->jointModels[binding.joints[joint]]);
        }
        paletteChanged |= MatricesDiffer(savedPalettes[section], posed0[section].skinning->matrices);
        differentInstances |= MatricesDiffer(posed0[section].skinning->matrices, posed1[section].skinning->matrices);
        ASSERT_NO_FATAL_FAILURE(ExpectImportedVertexShaderOutput(*rendering, posed0[section]));
        ASSERT_NO_FATAL_FAILURE(ExpectImportedVertexShaderOutput(*rendering, posed1[section]));
    }
    EXPECT_TRUE(paletteChanged);
    EXPECT_TRUE(differentInstances);
    for (const auto bot : bots) ASSERT_NO_FATAL_FAILURE(ExpectPosedQueries(host, bot));
    SelectExtractedBots(world);
    FrameImage posedImage;
    ASSERT_NO_FATAL_FAILURE(RenderAndRead(*rendering, frame, true, posedImage));
    EXPECT_GT(SelectedPixels(posedImage.selection), 20u);
    EXPECT_NE(initialImage.selection.pixels, posedImage.selection.pixels) << "animation did not change the GPU silhouette";
    EXPECT_NE(initialImage.color.pixels, posedImage.color.pixels) << "animation did not change rendered pixels";
    // Each bot must independently contribute selection pixels; a single working
    // instance must not hide a missing/incorrectly shared palette on the other.
    for (const auto* items : {&posed0, &posed1}) {
        world.selectedSceneItems = *items;
        FrameImage selectedImage;
        ASSERT_NO_FATAL_FAILURE(RenderAndRead(*rendering, frame, true, selectedImage));
        EXPECT_GT(SelectedPixels(selectedImage.selection), 10u);
    }
    world.selectedSceneItems.clear();
    FrameImage withoutSelection;
    ASSERT_NO_FATAL_FAILURE(RenderAndRead(*rendering, frame, false, withoutSelection));
    EXPECT_NE(posedImage.color.pixels, withoutSelection.color.pixels) << "selected silhouettes did not produce outlines";
    SelectExtractedBots(world);
    ASSERT_NO_FATAL_FAILURE(ExpectNoDebugErrors(infoQueue.Get()));

    ASSERT_TRUE(host.LoadScene(fixture.gameRoot / "scenes/animation.scene.json", error)) << error;
    EXPECT_TRUE(world.sceneItems.empty());
    EXPECT_FALSE(animation->GetPose(bots[0]));
    EXPECT_FALSE(animation->GetPlaybackState(bots[0]));
    EXPECT_FALSE(assets.Peek(meshHandle));
    EXPECT_FALSE(assets.Peek(instanceHandle));
    EXPECT_FALSE(assets.Peek(skeletonHandle));
    EXPECT_FALSE(assets.Peek(idleHandle));
    EXPECT_FALSE(assets.Peek(walkHandle));
    EXPECT_FALSE(assets.Peek(materialHandle));
    EXPECT_EQ(rendering->GetResourceManager().TryGetMaterialInstance(oldGpuInstance), nullptr);
    EXPECT_EQ(rendering->GetResourceManager().TryGetMaterial(oldGpuMaterial), nullptr);
    for (const auto gpuMesh : oldGpuMeshes) EXPECT_FALSE(rendering->GetResourceManager().DestroyMesh(gpuMesh));
    ASSERT_TRUE(world.selectedSceneItems.empty())
        << "scene replacement retained selection draws referencing evicted GPU assets";
    // Holding immutable CPU snapshots must not keep the scene's handles alive.
    EXPECT_EQ(initialPose0->jointModels, savedPose0);
    for (std::size_t i = 0; i < 2; ++i) EXPECT_EQ(initial0[i].skinning->matrices, savedPalettes[i]);
    ASSERT_NO_FATAL_FAILURE(LoadFixtureAssets(host));
    EXPECT_NE(assets.RequestSkinnedMesh(BotMesh), meshHandle);
    EXPECT_NE(assets.RequestSkeleton(BotSkeleton), skeletonHandle);
    EXPECT_NE(assets.RequestAnimationClip(IdleClip), idleHandle);
    EXPECT_NE(assets.RequestAnimationClip(WalkClip), walkHandle);
    EXPECT_NE(assets.RequestMaterial(DefaultMaterial), materialHandle);
    const auto reloadedBot = host.GetScene().FindById(BotIds[0]);
    ASSERT_NE(reloadedBot, entt::entity{entt::null});
    const auto* reloadedRenderer = host.GetScene().TryGetComponent<SkinnedMeshRenderer>(reloadedBot);
    ASSERT_NE(reloadedRenderer, nullptr);
    EXPECT_NE(reloadedRenderer->materialInstance, instanceHandle);
    EXPECT_FALSE(reloadedRenderer->posedBounds);
    frame.deltaSeconds = frame.simulationTimeSeconds = 0;
    animation->LateUpdate(frame);
    extraction->LateUpdate(frame);
    ASSERT_TRUE(animation->GetPose(reloadedBot));
    EXPECT_FALSE(MatricesDiffer(animation->GetPose(reloadedBot)->jointModels, savedPose0));
    animation->ResetSimulation();
    EXPECT_FALSE(animation->GetPose(reloadedBot));
    EXPECT_FALSE(animation->GetPlaybackState(reloadedBot));
    EXPECT_FALSE(reloadedRenderer->posedBounds);
    animation->LateUpdate(frame);
    extraction->LateUpdate(frame);
    SelectExtractedBots(world);
    FrameImage reloadedImage;
    ASSERT_NO_FATAL_FAILURE(RenderAndRead(*rendering, frame, true, reloadedImage));
    EXPECT_EQ(reloadedImage.selection.pixels, initialImage.selection.pixels);
    ASSERT_NO_FATAL_FAILURE(ExpectNoDebugErrors(infoQueue.Get()));

    const auto reloadedMesh = reloadedRenderer->mesh;
    ASSERT_TRUE(host.LoadScene(fixture.emptyScene, error)) << error;
    EXPECT_TRUE(world.sceneItems.empty());
    EXPECT_FALSE(assets.Peek(reloadedMesh));
    EXPECT_FALSE(animation->GetPose(reloadedBot));
    EXPECT_FALSE(animation->GetPlaybackState(reloadedBot));
    ASSERT_TRUE(world.selectedSceneItems.empty()) << "empty-scene reset retained selection draws";
    animation->LateUpdate(frame);
    extraction->LateUpdate(frame);
    EXPECT_TRUE(world.sceneItems.empty());
    FrameImage emptyImage;
    ASSERT_NO_FATAL_FAILURE(RenderAndRead(*rendering, frame, false, emptyImage));
    EXPECT_NE(emptyImage.color.pixels, reloadedImage.color.pixels);
    ASSERT_NO_FATAL_FAILURE(ExpectNoDebugErrors(infoQueue.Get()));
    host.Shutdown();
}
} // namespace
