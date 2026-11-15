#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/MaterialDrawList.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>

namespace {
using namespace engine;

const auto ShaderPath() {
    return AssetPathResolver::GetRoot() / "engine/shaders/MaterialTemplate.hlsl";
}

TEST(RenderingSkinning, DefaultAndHookVariantsHaveMatchingPaletteAndVertexContracts) {
    constexpr auto hookSource = R"(
#include "MaterialTemplate.hlsl"
#if defined(MATERIAL_GBUFFER_VS)
void PatchGBufferVertex(inout MaterialVertex v) { v.position.xyz += v.normal; }
#elif defined(MATERIAL_SHADOW_VS)
void PatchShadowVertex(inout MaterialVertex v) { v.position.xyz += v.normal; }
#endif
)";
    for (const auto* stage : {"MATERIAL_GBUFFER_VS", "MATERIAL_SHADOW_VS"}) {
        for (bool hook : {false, true}) {
            for (bool skinned : {false, true}) {
                SCOPED_TRACE(::testing::Message() << stage << " hook=" << hook << " skinned=" << skinned);
                ShaderCompileOptions options{.defines = {{stage, "1"}}};
                if (hook) options.defines.push_back({"MATERIAL_HAS_HOOK", "1"});
                if (skinned) options.defines.push_back({"ENGINE_SKINNED", "1"});
                const auto compiled = hook
                    ? ShaderContext::CompileSource(hookSource, ShaderPath(), ShaderType::Vertex, options)
                    : ShaderContext::CompileCpu(ShaderPath(), ShaderType::Vertex, options);
                ASSERT_TRUE(compiled);
                D3D11_SHADER_INPUT_BIND_DESC binding{};
                const auto bound = compiled->reflection->GetResourceBindingDescByName("EngineSkinning", &binding);
                EXPECT_EQ(SUCCEEDED(bound), skinned);
                if (!skinned) continue;
                EXPECT_EQ(binding.BindPoint, 9u);
                D3D11_SHADER_BUFFER_DESC buffer{};
                ASSERT_TRUE(SUCCEEDED(compiled->reflection->GetConstantBufferByName("EngineSkinning")->GetDesc(&buffer)));
                EXPECT_EQ(buffer.Size, sizeof(SkinningBufferData));
                bool indices = false, weights = false;
                for (UINT i = 0; i < compiled->desc.InputParameters; ++i) {
                    D3D11_SIGNATURE_PARAMETER_DESC input{};
                    ASSERT_TRUE(SUCCEEDED(compiled->reflection->GetInputParameterDesc(i, &input)));
                    if (std::strcmp(input.SemanticName, "BLENDINDICES") == 0) {
                        indices = true;
                        EXPECT_EQ(input.ComponentType, D3D_REGISTER_COMPONENT_UINT32);
                        EXPECT_EQ(input.Mask, 15);
                    }
                    if (std::strcmp(input.SemanticName, "BLENDWEIGHT") == 0) {
                        weights = true;
                        EXPECT_EQ(input.ComponentType, D3D_REGISTER_COMPONENT_FLOAT32);
                        EXPECT_EQ(input.Mask, 15);
                    }
                }
                EXPECT_TRUE(indices);
                EXPECT_TRUE(weights);
            }
        }
    }
}

class SkinningWarpTest : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    void SetUp() override {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, nullptr, &context))) {
            GTEST_SKIP() << "D3D11 WARP unavailable";
        }
    }
};

TEST_F(SkinningWarpTest, DrawRoutingAndReplacementOwnBothVertexVariants) {
    RenderingResourceManager manager;
    manager.Initialize(device.Get());
    const auto compile = [&](const char* stage, bool skinned) {
        ShaderCompileOptions options{.defines = {{stage, "1"}}};
        if (skinned) options.defines.push_back({"ENGINE_SKINNED", "1"});
        const auto cpu = ShaderContext::CompileCpu(ShaderPath(), ShaderType::Vertex, options);
        return cpu ? manager.RegisterCompiledShader(*cpu) : ShaderHandle{};
    };
    const auto rigidVS = compile("MATERIAL_GBUFFER_VS", false);
    const auto skinVS = compile("MATERIAL_GBUFFER_VS", true);
    const auto shadowVS = compile("MATERIAL_SHADOW_VS", true);
    ASSERT_TRUE(rigidVS && skinVS && shadowVS);
    ShaderSet shaders{};
    shaders.gBufferPassVS = rigidVS;
    shaders.skinnedGBufferPassVS = skinVS;
    shaders.skinnedShadowPassVS = shadowVS;
    const auto material = manager.RegisterMaterial(shaders, {});
    ASSERT_TRUE(material);
    EXPECT_FALSE(manager.DestroyShader(skinVS));
    EXPECT_FALSE(manager.DestroyShader(shadowVS));
    EXPECT_FALSE(manager.RegisterMaterial(shaders, {}));
    const auto instance = manager.CreateMaterialInstance(material);
    ASSERT_TRUE(instance);
    RenderWorld world;
    world.renderItems.push_back({Matrix::Identity, {}, instance});
    world.sceneItems.push_back({Matrix::Identity, {}, instance, std::make_shared<SkinningPalette>()});
    const auto draws = BuildMaterialDrawList(world, manager, false, rigidVS, {}, skinVS);
    ASSERT_EQ(draws.size(), 2u);
    for (const auto& draw : draws) EXPECT_EQ(draw.vertexShader, draw.item->skinning ? skinVS : rigidVS);
    const auto shadows = BuildMaterialDrawList(world, manager, true, rigidVS, {}, skinVS);
    for (const auto& draw : shadows) EXPECT_EQ(draw.vertexShader, draw.item->skinning ? shadowVS : rigidVS);

    const auto replacement = manager.RegisterMaterial({}, {});
    ASSERT_TRUE(replacement);
    ASSERT_TRUE(manager.ReplaceMaterial(material, replacement));
    EXPECT_FALSE(manager.DestroyShader(skinVS)); // released by replacement
    EXPECT_FALSE(manager.DestroyShader(shadowVS));
    const auto defaults = BuildMaterialDrawList(world, manager, false, rigidVS, {}, skinVS);
    for (const auto& draw : defaults) EXPECT_EQ(draw.vertexShader, draw.item->skinning ? skinVS : rigidVS);
    ASSERT_TRUE(manager.DestroyMaterialInstance(instance));
    EXPECT_TRUE(manager.DestroyMaterial(material));
}

// Execute the shared HLSL helper on WARP, rather than duplicating its cofactor
// implementation in a CPU test. The pixel stage is just a 1-pixel test harness.
TEST_F(SkinningWarpTest, WeightedAffineNormalsMatchInverseTransposeAndDegeneratesStayFinite) {
    const auto vs = ShaderContext::CompileSource(R"(
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    return float4(id == 1 ? 3 : -1, id == 2 ? 3 : -1, 0, 1);
})", ShaderPath(), ShaderType::Vertex);
    const auto ps = ShaderContext::CompileSource(R"(
#include "ShaderTypes.hlsl"
#include "Skinning.hlsl"
float4 PSMain() : SV_TARGET {
    MaterialVertex v = {float4(1, 2, 3, 1), float3(1, 2, 3), float2(0, 0)};
    SkinVertex(v, uint4(0, 1, 0, 0), float4(0.25, 0.75, 0, 0));
    return float4(v.normal, 1);
})", ShaderPath(), ShaderType::Pixel, {.defines = {{"ENGINE_SKINNED", "1"}}});
    ASSERT_TRUE(vs && ps);
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vs->blob->GetBufferPointer(), vs->blob->GetBufferSize(),
                                                    nullptr, &vertexShader)));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(ps->blob->GetBufferPointer(), ps->blob->GetBufferSize(),
                                                   nullptr, &pixelShader)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)));
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    ComPtr<ID3D11RenderTargetView> rtv;
    ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(SkinningBufferData);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> palette;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&bufferDesc, nullptr, &palette)));
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> raster;
    ASSERT_TRUE(SUCCEEDED(device->CreateRasterizerState(&rasterDesc, &raster)));
    context->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(9, 1, palette.GetAddressOf());
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    Matrix shear = Matrix::Identity;
    shear._12 = 0.7f;
    shear._23 = -0.4f;
    const std::array transforms{
        Matrix::CreateScale(2, 3, 4) * shear,
        Matrix::CreateScale(-2, 3, 4) * shear,
        Matrix::CreateScale(0, 0, 0),
        Matrix::CreateScale(1, 0, 1)
    };
    for (size_t i = 0; i < transforms.size(); ++i) {
        SkinningBufferData data;
        data.matrices.fill(Matrix::Identity);
        data.matrices[0] = transforms[i];
        data.matrices[1] = transforms[i] * Matrix::CreateRotationZ(0.5f);
        context->UpdateSubresource(palette.Get(), 0, nullptr, &data, 0, 0);
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        DirectX::SimpleMath::Vector4 result;
        std::memcpy(&result, mapped.pData, sizeof(result));
        context->Unmap(staging.Get(), 0);
        EXPECT_TRUE(std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z));
        EXPECT_NEAR(result.x * result.x + result.y * result.y + result.z * result.z, 1.0f, 1e-5f);
        if (i < 2) {
            const auto blended = data.matrices[0] * 0.25f + data.matrices[1] * 0.75f;
            auto expected = Vector3::TransformNormal(Vector3(1, 2, 3), blended.Invert().Transpose());
            expected.Normalize();
            EXPECT_NEAR(result.x, expected.x, 1e-5f);
            EXPECT_NEAR(result.y, expected.y, 1e-5f);
            EXPECT_NEAR(result.z, expected.z, 1e-5f);
        }
    }
}
} // namespace
