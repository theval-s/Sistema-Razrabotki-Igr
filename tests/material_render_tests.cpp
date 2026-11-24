#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/RenderingSystem.hpp>
#include <engine/rendering/UglyUtils.hpp>
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace engine;
using Pixel = std::array<float, 4>;

std::filesystem::path ShaderPath() {
    return AssetPathResolver::GetRoot() / "engine/shaders/MaterialTemplate.hlsl";
}

TEST(MaterialSurfaceABI, TemplateReflectsTheCpuLayoutAndAllFourOutputs) {
    const auto shader = ShaderContext::CompileCpu(ShaderPath(), ShaderType::Pixel,
        {.defines = {{"MATERIAL_GBUFFER_PS", "1"}}});
    D3D11_SHADER_INPUT_BIND_DESC binding{};
    ASSERT_HRESULT_SUCCEEDED(shader.reflection->GetResourceBindingDescByName("EngineMaterial", &binding));
    EXPECT_EQ(binding.BindPoint, 2u);
    auto* buffer = shader.reflection->GetConstantBufferByName("EngineMaterial");
    D3D11_SHADER_BUFFER_DESC desc{};
    ASSERT_HRESULT_SUCCEEDED(buffer->GetDesc(&desc));
    EXPECT_EQ(desc.Size, sizeof(MaterialBufferData));
    auto* type = buffer->GetVariableByName("Material")->GetType();
    constexpr std::array names{"specularColor", "shininess", "emission", "lightingWeight"};
    constexpr std::array offsets{offsetof(MaterialBufferData, specularColor), offsetof(MaterialBufferData, shininess),
        offsetof(MaterialBufferData, emission), offsetof(MaterialBufferData, lightingWeight)};
    for (size_t i = 0; i < names.size(); ++i) {
        D3D11_SHADER_TYPE_DESC member{};
        ASSERT_HRESULT_SUCCEEDED(type->GetMemberTypeByName(names[i])->GetDesc(&member));
        EXPECT_EQ(member.Offset, offsets[i]);
    }
    ASSERT_EQ(shader.desc.OutputParameters, 4u);
    for (UINT i = 0; i < 4; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC output{};
        ASSERT_HRESULT_SUCCEEDED(shader.reflection->GetOutputParameterDesc(i, &output));
        EXPECT_EQ(output.SystemValueType, D3D_NAME_TARGET);
        EXPECT_EQ(output.SemanticIndex, i);
        EXPECT_EQ(output.ComponentType, D3D_REGISTER_COMPONENT_FLOAT32);
        EXPECT_EQ(output.Mask, 15);
    }
    ASSERT_HRESULT_SUCCEEDED(shader.reflection->GetResourceBindingDescByName("emissionTexture", &binding));
    EXPECT_EQ(binding.BindPoint, 1u);
}

TEST(MaterialSurfaceABI, RawShaderMigrationAndRuntimeValidationAreAtomic) {
    ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, nullptr))) GTEST_SKIP() << "D3D11 WARP unavailable";
    RenderingResourceManager manager;
    manager.Initialize(device.Get());
    const auto compile = [&](std::string_view source) {
        return manager.RegisterCompiledShader(ShaderContext::CompileSource(source, ShaderPath(), ShaderType::Pixel));
    };
    const auto old = compile(R"(
struct Output { float4 a:SV_Target0; float4 n:SV_Target1; float4 m:SV_Target2; };
Output PSMain() { Output o; o.a=1; o.n=1; o.m=0; return o; })");
    EXPECT_FALSE(manager.RegisterMaterial({.gBufferPassPS = old}, {}));
    EXPECT_TRUE(manager.DestroyShader(old)); // Rejection never takes ownership.

    const auto shortOutput = compile(R"(
struct Output { float4 a:SV_Target0; float4 n:SV_Target1; float4 m:SV_Target2; float3 e:SV_Target3; };
Output PSMain() { Output o; o.a=1; o.n=1; o.m=0; o.e=0; return o; })");
    EXPECT_FALSE(manager.RegisterMaterial({.gBufferPassPS = shortOutput}, {}));
    EXPECT_TRUE(manager.DestroyShader(shortOutput));

    const auto unwrittenAlpha = compile(R"(
struct Output { float4 a:SV_Target0; float4 n:SV_Target1; float4 m:SV_Target2; float4 e:SV_Target3; };
Output PSMain() { Output o; o.a=1; o.n=1; o.m=0; o.e.rgb=0; return o; })");
    EXPECT_FALSE(manager.RegisterMaterial({.gBufferPassPS = unwrittenAlpha}, {}));
    EXPECT_TRUE(manager.DestroyShader(unwrittenAlpha));

    const auto oldBuffer = compile(R"(
#include "ShaderTypes.hlsl"
struct OldMaterial { float3 specularColor; float shininess; };
cbuffer EngineMaterial : register(b2) { OldMaterial Material; };
GBufferOutput PSMain() {
    MaterialSurface s=(MaterialSurface)0;
    s.albedo=float4(Material.specularColor,1); s.shininess=Material.shininess;
    s.lightingWeight=1; return EncodeMaterialSurface(s);
})");
    EXPECT_FALSE(manager.RegisterMaterial({.gBufferPassPS = oldBuffer}, {}));
    EXPECT_TRUE(manager.DestroyShader(oldBuffer));

    const auto current = compile(R"(
#include "ShaderTypes.hlsl"
GBufferOutput PSMain() {
    MaterialSurface s=(MaterialSurface)0;
    s.albedo=1; s.emission=float3(4,2,1); return EncodeMaterialSurface(s);
})");
    const auto material = manager.RegisterMaterial({.gBufferPassPS = current}, {});
    ASSERT_TRUE(material); // b2 may legitimately be optimized away.
    const auto instance = manager.CreateMaterialInstance(material);
    ASSERT_TRUE(manager.SetMaterialInstanceOverrides(instance, {.emission = Vector3{8, 4, 2}, .lightingWeight = 0.25f}));
    const auto before = ResolveMaterialParameters(*manager.TryGetMaterial(material), *manager.TryGetMaterialInstance(instance));
    for (float invalid : {-1.0f, 1.01f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_FALSE(manager.SetMaterialInstanceOverrides(instance, {.emission = Vector3{2, 2, 2}, .lightingWeight = invalid}));
        auto parameters = before;
        parameters.parameters.lightingWeight = invalid;
        EXPECT_FALSE(manager.SetMaterialInstanceParameters(instance, parameters));
        EXPECT_FALSE(manager.RegisterMaterial({}, parameters));
    }
    for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        EXPECT_FALSE(manager.SetMaterialInstanceOverrides(instance, {.emission = Vector3{invalid, 0, 0}}));
    const auto after = ResolveMaterialParameters(*manager.TryGetMaterial(material), *manager.TryGetMaterialInstance(instance));
    EXPECT_EQ(after.parameters.emission, before.parameters.emission);
    EXPECT_EQ(after.parameters.lightingWeight, before.parameters.lightingWeight);
    EXPECT_TRUE(manager.DestroyMaterialInstance(instance));
    EXPECT_TRUE(manager.DestroyMaterial(material));
}

struct Image {
    UINT width{}, height{};
    std::vector<Pixel> pixels;
    const Pixel& At(UINT x, UINT y) const { return pixels.at(size_t(y) * width + x); }
    const Pixel& Center() const { return At(width / 2, height / 2); }
};

void ReadTexture(RenderingContext& context, ID3D11Texture2D* source, Image& image) {
    ASSERT_NE(source, nullptr);
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const auto format = desc.Format;
    ASSERT_TRUE(format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM);
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
            if (format == DXGI_FORMAT_R32_TYPELESS) std::memcpy(p.data(), row + x * 4, 4);
            else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                std::array<uint16_t, 4> half{};
                std::memcpy(half.data(), row + x * 8, 8);
                for (size_t c = 0; c < 4; ++c) p[c] = DirectX::PackedVector::XMConvertHalfToFloat(half[c]);
            } else {
                for (size_t c = 0; c < 4; ++c) p[c] = row[x * 4 + c] / 255.0f;
                if (format == DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(p[0], p[2]);
            }
            image.pixels[size_t(y) * image.width + x] = p;
        }
    }
    dc->Unmap(staging.Get(), 0);
}

void ExpectPixel(const Pixel& actual, const Pixel& expected) {
    for (size_t c = 0; c < 4; ++c) {
        EXPECT_TRUE(std::isfinite(actual[c]));
        EXPECT_NEAR(actual[c], expected[c], 0.015f) << "channel " << c;
    }
}

class MaterialRender : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    HWND window{};
    RenderingSystem renderer;
    MeshHandle mesh;
    MaterialHandle material;
    MaterialInstanceHandle instance;
    Image hdr, normal, depth, presented;

    void SetUp() override {
        AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "tests");
        window = CreateWindowExW(0, L"STATIC", L"Material GPU regression", WS_OVERLAPPEDWINDOW,
            0, 0, 96, 72, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        const bool initialized = renderer.Initialize({window, 96, 72}, true);
        const auto diagnostics = renderer.GetDiagnostics();
        if (!initialized && diagnostics.failure && diagnostics.failure->operation == "D3D11CreateDeviceAndSwapChain")
            GTEST_SKIP() << diagnostics.failure->summary;
        ASSERT_TRUE(initialized) << (diagnostics.failure ? diagnostics.failure->detail : "");
        auto& world = renderer.GetWorld();
        world.camera = FPSCamera{0.8f, 96.0f / 72, 0.1f, 100};
        world.camera.position = {0, 0, 0};
        world.directionalLight = {{0, 0, 0}, {0, 0, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};
        auto& manager = renderer.GetResourceManager();
        mesh = ugly_utils::FromRawMesh(manager, ugly_utils::CreateCube(2, 2, 2));
        material = manager.RegisterMaterial({}, {});
        instance = manager.CreateMaterialInstance(material);
        ASSERT_TRUE(instance);
        world.sceneItems = {{Matrix::CreateTranslation(0, 0, -4), mesh, instance}};
    }
    void TearDown() override {
        renderer.Shutdown();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
    }
    void Set(const MaterialInstance::Overrides& overrides) {
        ASSERT_TRUE(renderer.GetResourceManager().SetMaterialInstanceOverrides(instance, overrides));
    }
    void Render() {
        bool captured = false;
        renderer.RenderFrame({}, [&](RenderingContext& context, const FrameContext&) {
            const auto& gb = renderer.GetResources().gBuffer;
            const auto read = [&](TextureHandle handle, Image& image) {
                const auto* texture = context.GetResourceManager().TryGetTexture(handle);
                ASSERT_NE(texture, nullptr);
                ReadTexture(context, texture->GetTexture(), image);
            };
            ASSERT_NO_FATAL_FAILURE(read(gb.resultTexture, hdr));
            ASSERT_NO_FATAL_FAILURE(read(gb.normalTexture, normal));
            ASSERT_NO_FATAL_FAILURE(read(gb.depthTexture, depth));
            ComPtr<ID3D11RenderTargetView> target;
            context.GetDeviceContext()->OMGetRenderTargets(1, &target, nullptr);
            ASSERT_TRUE(target);
            ComPtr<ID3D11Resource> resource;
            target->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            ASSERT_HRESULT_SUCCEEDED(resource.As(&texture));
            ASSERT_NO_FATAL_FAILURE(ReadTexture(context, texture.Get(), presented));
            captured = true;
        });
        const auto diagnostics = renderer.GetDiagnostics();
        ASSERT_EQ(diagnostics.lifecycle, RenderingLifecycle::Ready)
            << (diagnostics.failure ? diagnostics.failure->detail : "");
        ASSERT_TRUE(captured);
        ComPtr<ID3D11InfoQueue> queue;
        if (SUCCEEDED(renderer.GetRenderingContext().GetDevice()->QueryInterface(IID_PPV_ARGS(&queue)))) {
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size{};
                ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, nullptr, &size));
                std::vector<std::byte> storage(size);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, message, &size));
                EXPECT_GT(message->Severity, D3D11_MESSAGE_SEVERITY_WARNING) << message->pDescription;
            }
            queue->ClearStoredMessages();
        }
    }
};

TEST_F(MaterialRender, UnlitEmissionAndPartialLightingSurviveEveryLightTypeAndTonemapping) {
    ASSERT_NO_FATAL_FAILURE(Render());
    ASSERT_GT(depth.Center()[0], 0);
    ExpectPixel(hdr.Center(), {0, 0, 0, 1});
    ASSERT_NO_FATAL_FAILURE(Set({.lightingWeight = 0.0f}));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {1, 1, 1, 1});
    ExpectPixel(presented.Center(), {0.5f, 0.5f, 0.5f, 1});
    EXPECT_EQ(normal.Center()[3], 0);
    auto& world = renderer.GetWorld();
    world.directionalLight.intensity = 2;
    world.lights = {
        {{0, 0, 0}, {}, {1, 0.5f, 0.25f}, 3, 10, LightInfo::Point},
        {{0, 0, 0}, {0, 0, -1}, {0.25f, 0.5f, 1}, 2, 10, LightInfo::Spot, 0.95f, 0.8f}
    };
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {1, 1, 1, 1});
    const Vector3 emission{4, 2, 0.5f};
    ASSERT_NO_FATAL_FAILURE(Set({.emission = emission, .lightingWeight = 0.0f}));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {5, 3, 1.5f, 1});
    for (size_t count = 0; count <= 2; ++count) {
        SCOPED_TRACE(count);
        const auto lights = world.lights;
        world.lights.resize(count);
        ASSERT_NO_FATAL_FAILURE(Set({}));
        ASSERT_NO_FATAL_FAILURE(Render());
        const auto lit = hdr.Center();
        EXPECT_GT(lit[0], 0.2f);
        ASSERT_NO_FATAL_FAILURE(Set({.emission = emission}));
        ASSERT_NO_FATAL_FAILURE(Render());
        ExpectPixel(hdr.Center(), {lit[0] + 4, lit[1] + 2, lit[2] + 0.5f, 1});
        ASSERT_NO_FATAL_FAILURE(Set({.emission = emission, .lightingWeight = 0.25f}));
        ASSERT_NO_FATAL_FAILURE(Render());
        ExpectPixel(hdr.Center(), {4.75f + lit[0] * 0.25f, 2.75f + lit[1] * 0.25f,
            1.25f + lit[2] * 0.25f, 1});
        EXPECT_EQ(normal.Center()[3], 0.25f);
        world.lights = lights;
    }
}

TEST_F(MaterialRender, EmissionTextureWhiteFallbackAndResize) {
    const Pixel texel{0.25f, 0.5f, 1, 1};
    const D3D11_SUBRESOURCE_DATA data{texel.data(), sizeof(texel), 0};
    auto& manager = renderer.GetResourceManager();
    const auto texture = manager.CreateTexture({.width = 1, .height = 1, .format = DXGI_FORMAT_R32G32B32A32_FLOAT},
        std::span(&data, 1));
    ASSERT_NO_FATAL_FAILURE(Set({.emission = Vector3{8, 4, 2}, .emissionTexture = texture}));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {2, 2, 2, 1});
    ASSERT_TRUE(renderer.Reinitialize({window, 128, 80}));
    ASSERT_NO_FATAL_FAILURE(Render());
    EXPECT_EQ(hdr.width, 128u);
    ExpectPixel(hdr.Center(), {2, 2, 2, 1});
    ASSERT_NO_FATAL_FAILURE(Set({.emission = Vector3{8, 4, 2}, .emissionTexture = TextureHandle{}}));
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {8, 4, 2, 1});
}

TEST_F(MaterialRender, OrdinaryForegroundOverwritesHiddenEmissionAndEmptyFrameClears) {
    auto& world = renderer.GetWorld();
    auto& manager = renderer.GetResourceManager();
    ASSERT_NO_FATAL_FAILURE(Set({.emission = Vector3{8, 4, 2}}));
    const auto foreground = manager.CreateMaterialInstance(material);
    world.sceneItems = {
        {Matrix::CreateTranslation(0, 0, -6), mesh, instance},
        {Matrix::CreateTranslation(0, 0, -3), mesh, foreground}
    };
    ASSERT_NO_FATAL_FAILURE(Render());
    ASSERT_GT(depth.Center()[0], 0);
    ExpectPixel(hdr.Center(), {0, 0, 0, 1});
    world.sceneItems.pop_back();
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {8, 4, 2, 1});
    world.sceneItems.clear();
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.Center(), {0, 0, 0, 1});
    EXPECT_EQ(depth.Center()[0], 0);
}

TEST_F(MaterialRender, CustomSurfaceHookControlsEmissionAndLightingPerPixel) {
    auto& manager = renderer.GetResourceManager();
    const auto compiled = ShaderContext::CompileSource(R"(
#include "MaterialTemplate.hlsl"
void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) {
    surface.albedo = float4(0.25, 0.5, 0.75, 1);
    surface.emission = float3(4, 2, 1);
    surface.lightingWeight = input.pos.x < View.viewportSize.x * 0.5 ? 0 : 1;
    // Unlit pixels do not require a usable lighting normal.
    if (surface.lightingWeight == 0) surface.normal = 0;
})", ShaderPath(), ShaderType::Pixel,
        {.defines = {{"MATERIAL_GBUFFER_PS", "1"}, {"MATERIAL_HAS_HOOK", "1"}}});
    const auto shader = manager.RegisterCompiledShader(compiled);
    const auto custom = manager.RegisterMaterial({.gBufferPassPS = shader}, {});
    ASSERT_TRUE(custom);
    const auto customInstance = manager.CreateMaterialInstance(custom);
    renderer.GetWorld().sceneItems.front().materialInstance = customInstance;
    ASSERT_NO_FATAL_FAILURE(Render());
    ExpectPixel(hdr.At(43, 36), {4.25f, 2.5f, 1.75f, 1});
    ExpectPixel(hdr.At(53, 36), {4, 2, 1, 1});
    EXPECT_EQ(normal.At(43, 36)[3], 0);
    EXPECT_EQ(normal.At(53, 36)[3], 1);
    EXPECT_GT(depth.At(43, 36)[0], 0);
    EXPECT_GT(depth.At(53, 36)[0], 0);
}
} // namespace
