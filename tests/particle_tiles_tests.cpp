#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include "rendering/particles/ParticleTiles.hpp"

#include <DirectXPackedVector.h>
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace engine;
using Particle = ParticleSimulation::Particle;
using Color = std::array<float, 4>;
using Half = DirectX::PackedVector::HALF;
using HalfColor = std::array<Half, 4>;

void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + ": HRESULT " + std::to_string(hr));
}

struct Image {
    UINT width;
    std::vector<Color> pixels;
    ParticleTiles::Stats stats;
    const Color& At(UINT x, UINT y) const { return pixels.at(y * width + x); }
};

// DefaultParticleHooks returns straight tint and (1 - |uv|^2) * (1 - age/lifetime).
Particle MakeParticle(float x, float y, float distance, float radius, float age,
                      std::array<float, 3> tint, UINT seed) {
    return {{x, y, -distance, age}, {0, 0, 0, 1}, {tint[0], tint[1], tint[2], radius}, {0, 2, seed, 0}};
}

class ParticleTilesWarp : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    std::array<ComPtr<ID3D11ComputeShader>, 4> shaders;
    const std::filesystem::path previousRoot = AssetPathResolver::GetGameRoot();

    void SetUp() override {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                                     D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
            GTEST_SKIP() << "D3D11 FL11 WARP unavailable";
        }
        // The source test directory contains no project shader overrides. This goes through
        // the real sri/project include resolver even if the executable sits beside example_game.
        const auto root = std::filesystem::path(SRI_SOURCE_ROOT);
        AssetPathResolver::SetGameRoot(root / "tests");
        const char* defines[]{"PARTICLE_TILES_BUILD", "PARTICLE_TILES_MERGE", "PARTICLE_TILES_BIN", "PARTICLE_TILES_SHADE"};
        for (size_t i = 0; i < shaders.size(); ++i) {
            const auto cs = ShaderContext::CompileCpu(root / "engine/shaders/ParticleTiles.hlsl", ShaderType::Compute,
                                                       {.defines = {{defines[i], "1"}}});
            Check(device->CreateComputeShader(cs.blob->GetBufferPointer(), cs.blob->GetBufferSize(), nullptr,
                                              &shaders[i]), defines[i]);
        }
    }

    void TearDown() override { AssetPathResolver::SetGameRoot(previousRoot); }

    template<class T>
    ComPtr<ID3D11ShaderResourceView> Structured(std::span<const T> data) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(data.size_bytes());
        desc.StructureByteStride = sizeof(T);
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&desc, &init, &buffer), "create structured buffer");
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(buffer.Get(), nullptr, &srv), "create structured SRV");
        return srv;
    }

    Image Render(UINT width, UINT height, const ParticleTiles::Camera& camera, const std::vector<Particle>& pool,
                 const std::vector<UINT>& active, const std::vector<float>& depth) {
        ParticleTiles tiles;
        Check(tiles.Initialize(device.Get(), {shaders[0].Get(), shaders[1].Get(), shaders[2].Get(), shaders[3].Get()},
                               static_cast<UINT>(pool.size()), width, height), "initialize tiles");
        EXPECT_EQ(tiles.CoarsePixels(), 64u);
        const UINT count = static_cast<UINT>(active.size());
        auto particles = Structured<Particle>(pool);
        auto indices = Structured<UINT>(active);
        auto live = Structured<UINT>({&count, 1});
        const UINT dispatch[]{(count + 63) / 64, 1, 1};
        D3D11_BUFFER_DESC argsDesc{};
        argsDesc.ByteWidth = sizeof(dispatch);
        argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        D3D11_SUBRESOURCE_DATA argsInit{dispatch, 0, 0};
        ComPtr<ID3D11Buffer> args;
        Check(device->CreateBuffer(&argsDesc, &argsInit, &args), "create indirect args");

        D3D11_TEXTURE2D_DESC tex{};
        tex.Width = width;
        tex.Height = height;
        tex.MipLevels = tex.ArraySize = tex.SampleDesc.Count = 1;
        tex.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tex.Format = DXGI_FORMAT_R32_FLOAT;
        D3D11_SUBRESOURCE_DATA depthInit{depth.data(), width * UINT(sizeof(float)), 0};
        ComPtr<ID3D11Texture2D> z, lit, staging;
        ComPtr<ID3D11ShaderResourceView> depthSrv, litSrv;
        Check(device->CreateTexture2D(&tex, &depthInit, &z), "create depth");
        Check(device->CreateShaderResourceView(z.Get(), nullptr, &depthSrv), "create depth SRV");
        tex.BindFlags |= D3D11_BIND_RENDER_TARGET;
        tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // Nonzero background tests the destination term even when no particles contribute.
        const Half quarter = DirectX::PackedVector::XMConvertFloatToHalf(0.25f);
        const std::vector<HalfColor> initial(width * height, HalfColor{quarter, quarter, quarter, quarter});
        D3D11_SUBRESOURCE_DATA litInit{initial.data(), width * UINT(sizeof(HalfColor)), 0};
        Check(device->CreateTexture2D(&tex, &litInit, &lit), "create lit target");
        Check(device->CreateShaderResourceView(lit.Get(), nullptr, &litSrv), "create lit SRV");
        Check(tiles.Render({particles.Get(), indices.Get(), live.Get(), args.Get()}, camera, depthSrv.Get(), litSrv.Get()),
              "render tiles");

        tex.BindFlags = 0;
        tex.Usage = D3D11_USAGE_STAGING;
        tex.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&tex, nullptr, &staging), "create readback");
        Image image{width, std::vector<Color>(width * height), {}};
        ctx->CopyResource(staging.Get(), lit.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // Blocking *test* readback follows Render's telemetry event in the immediate queue.
        Check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map lit pixels");
        for (UINT y = 0; y < height; ++y) {
            const auto row = reinterpret_cast<const Half*>(static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < width; ++x) for (UINT c = 0; c < 4; ++c)
                image.pixels[y * width + x][c] = DirectX::PackedVector::XMConvertHalfToFloat(row[4 * x + c]);
        }
        ctx->Unmap(staging.Get(), 0);
        image.stats = tiles.Latest();
        EXPECT_EQ(image.stats.frame, 1u);
        return image;
    }
};

ParticleTiles::Camera Ortho(UINT width, UINT height) {
    ParticleTiles::Camera camera{};
    for (int i : {0, 5, 10, 15}) camera.view[i] = 1;
    camera.projection[0] = 2.f / width;
    camera.projection[5] = 2.f / height;
    camera.projection[10] = 0.125f;
    camera.projection[14] = camera.projection[15] = 1; // d = 1 - distance / 8
    return camera;
}

TEST_F(ParticleTilesWarp, DensePartialTileBlendsAllFarParticlesThenNearParticlesInSeedOrder) {
    constexpr UINT width = 9, height = 9, farCount = 4001;
    // All visible sprites centered at (8.5,8.5), radius 2: all four fine tiles intersect.
    auto nearBlue = MakeParticle(4, -4, 2, 2, .5f, {0, 0, 4}, 2);
    auto nearRed = MakeParticle(4, -4, 2, 2, .5f, {2, 0, 0}, 1);
    auto farGreen = MakeParticle(4, -4, 4, 2, 1.f - 1.f / 4096, {0, 1, 0}, 0);
    auto offscreen = nearRed;
    offscreen.positionAge[0] = -100;
    // Slot zero is visible, but dead. The live list starts with the two nearest in
    // reverse tie order; sorting individual 256-key blocks cannot produce the right color.
    std::vector<Particle> pool{nearRed, nearBlue, nearRed};
    std::vector<UINT> active{1, 2};
    for (UINT i = 0; i < farCount; ++i) {
        farGreen.meta[2] = farCount - i;
        pool.push_back(farGreen);
        active.push_back(static_cast<UINT>(pool.size() - 1));
        if (i % 4 == 0) {
            pool.push_back(offscreen);
            active.push_back(static_cast<UINT>(pool.size() - 1));
        }
    }
    const auto image = Render(width, height, Ortho(width, height), pool, active, std::vector<float>(width * height));
    EXPECT_EQ(image.stats.peakOccupancy, farCount + 2);
    EXPECT_EQ(image.stats.batchingTiles, 4u);
    // Closed-form transmittance through far green, then half opaque red, then half opaque HDR blue.
    const double t = std::pow(1.0 - 1.0 / 4096, farCount);
    const double background = .25 * t * .5 * .5;
    const auto& pixel = image.At(8, 8);
    EXPECT_NEAR(pixel[0], background + .5, .001);
    EXPECT_NEAR(pixel[1], background + (1 - t) * .25, .0002);
    EXPECT_NEAR(pixel[2], background + 2, .002);
    EXPECT_NEAR(pixel[3], 1 - .75 * t * .25, .001);
    EXPECT_EQ(image.At(0, 0), (Color{.25f, .25f, .25f, .25f}));
}

TEST_F(ParticleTilesWarp, ReverseZSoftIntersectionSpansCoarseBoundaryAndPartialEdge) {
    constexpr UINT width = 66, height = 9;
    auto camera = Ortho(width, height);
    camera.projection[0] *= 2;
    camera.projection[5] *= 2;
    camera.projection[10] = camera.projection[15] = 0;
    camera.projection[11] = -1; // infinite reverse-Z perspective: d = 1 / distance
    const auto sprite = MakeParticle(31, -3.5f, 2, 2, .5f, {2, 1, 0}, 7);
    // Center (64,8), projected radii (2,2). Four equidistant pixel centers in four
    // fine tiles, split across two coarse regions. The final row is a partial tile.
    std::vector<float> depth(width * height);
    depth[8 * width + 63] = 1.f / 2.5f; // 0.5 world units behind particle -> 1/4 opacity multiplier
    depth[8 * width + 64] = 1.f;        // geometry closer than the particle
    const auto image = Render(width, height, camera, {sprite}, {0}, depth);
    EXPECT_EQ(image.stats.peakOccupancy, 1u);
    EXPECT_EQ(image.stats.batchingTiles, 0u);
    const float alpha = .5f * (1 - 2 * .25f * .25f); // uv=(+/-0.25,+/-0.25)
    for (UINT x : {63u, 64u}) EXPECT_NEAR(image.At(x, 7)[0], .25f + 1.75f * alpha, .001);
    EXPECT_NEAR(image.At(63, 8)[0], .25f + 1.75f * alpha * .25f, .0005);
    EXPECT_EQ(image.At(64, 8), (Color{.25f, .25f, .25f, .25f}));
}
} // namespace
