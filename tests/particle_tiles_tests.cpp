#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include "rendering/particles/ParticleTiles.hpp"
#include "rendering/particles/ParticleTextures.hpp"
#include "rendering/particles/ParticleShadows.hpp"
#include <engine/rendering/RenderingResources.hpp>

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
struct TestStyle {
    float accelerationDrag[4]{}, sizeOpacity[4]{1,1,1,1}, colorStartFade[4]{1,1,1,0},
        colorEndSoft[4]{1,1,1,0}, variationDensity[4]{0,0,1,0};
    UINT flags[4]{0,0,1,0};
};
static_assert(sizeof(TestStyle)==96);

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
                 const std::vector<UINT>& active, const std::vector<float>& depth,
                 ID3D11ShaderResourceView* styles=nullptr, ID3D11ShaderResourceView* textures=nullptr) {
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
        Check(tiles.Render({particles.Get(), indices.Get(), live.Get(), args.Get(),UINT32_MAX,styles}, camera, depthSrv.Get(), litSrv.Get(),textures),
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

    void InitTextures(ParticleTextures& textures) {
        auto cs=ShaderContext::CompileCpu(std::filesystem::path(SRI_SOURCE_ROOT)/"engine/shaders/ParticleTextures.hlsl",ShaderType::Compute,{});
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(cs.blob->GetBufferPointer(),cs.blob->GetBufferSize(),nullptr,&shader),"resample shader");
        Check(textures.Initialize(device.Get(),shader.Get()),"texture array");
    }
    ComPtr<ID3D11ShaderResourceView> SolidTexture(UINT rgba, bool srgb=false) {
        D3D11_TEXTURE2D_DESC t{}; t.Width=3; t.Height=2; t.MipLevels=t.ArraySize=t.SampleDesc.Count=1;
        t.Format=srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
        t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        UINT pixels[6]{rgba,rgba,rgba,rgba,rgba,rgba}; D3D11_SUBRESOURCE_DATA data{pixels,12,0};
        ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateTexture2D(&t,&data,&texture),"source texture");
        Check(device->CreateShaderResourceView(texture.Get(),nullptr,&srv),"source view"); return srv;
    }
    float ReadArray(ID3D11ShaderResourceView* srv, UINT slice, UINT x, UINT y, UINT channel=0) {
        ComPtr<ID3D11Resource> resource; srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture,staging; Check(resource.As(&texture),"array resource");
        D3D11_TEXTURE2D_DESC t{}; texture->GetDesc(&t); UINT channels=t.Format==DXGI_FORMAT_R16_FLOAT?1:4;
        t.BindFlags=0; t.Usage=D3D11_USAGE_STAGING; t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&t,nullptr,&staging),"array staging"); ctx->CopyResource(staging.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE m{}; Check(ctx->Map(staging.Get(),slice,D3D11_MAP_READ,0,&m),"array map");
        auto row=reinterpret_cast<const Half*>(static_cast<const std::byte*>(m.pData)+y*m.RowPitch);
        float value=DirectX::PackedVector::XMConvertHalfToFloat(row[x*channels+channel]); ctx->Unmap(staging.Get(),slice); return value;
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

TEST_F(ParticleTilesWarp, ResamplingDecodesSrgbAndCacheExhaustionNeverAliases) {
    ParticleTextures textures; InitTextures(textures);
    auto source=SolidTexture(0xff808080,true); UINT layer;
    Check(textures.Resolve(source.Get(),layer),"resolve source"); EXPECT_EQ(layer,1u);
    EXPECT_NEAR(ReadArray(textures.View(),layer,128,128),.21586f,.001f);
    EXPECT_NEAR(ReadArray(textures.View(),layer,128,128,3),1,.001f);
    UINT same; Check(textures.Resolve(source.Get(),same),"cached source"); EXPECT_EQ(same,layer);
    EXPECT_GT(ReadArray(textures.View(),0,128,128,3),.99f);
    EXPECT_EQ(ReadArray(textures.View(),0,0,0,3),0);
    for(UINT i=2;i<64;++i) { auto next=SolidTexture(0xffffffff); Check(textures.Resolve(next.Get(),layer),"fill cache"); EXPECT_EQ(layer,i); }
    auto excess=SolidTexture(0xffffffff); EXPECT_EQ(textures.Resolve(excess.Get(),layer),HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS));
    EXPECT_EQ(layer,UINT32_MAX);
}

TEST_F(ParticleTilesWarp, TexturedSquareCornersAndMixedBlendOrderingUseSharedLifetimeStyle) {
    ParticleTextures textures; InitTextures(textures); auto white=SolidTexture(0xffffffff); UINT layer;
    Check(textures.Resolve(white.Get(),layer),"white layer");
    std::array<TestStyle,2> styles{};
    for(auto& s:styles) { s.flags[0]=layer; s.sizeOpacity[2]=s.sizeOpacity[3]=.5f; }
    styles[1].flags[1]=1;
    auto style=Structured<TestStyle>(styles);
    auto nearParticle=MakeParticle(0,0,2,4,.5f,{1,0,0},1); nearParticle.meta[1]=0;
    auto farParticle=nearParticle; farParticle.positionAge[2]=-4; farParticle.meta[0]=1; farParticle.colorSize[0]=0; farParticle.colorSize[2]=1;
    auto image=Render(9,9,Ortho(9,9),{nearParticle,farParticle},{0,1},std::vector<float>(81),style.Get(),textures.View());
    // uv=(.75,.75): outside old disc, inside square. Far blue additive is attenuated by near red alpha.
    EXPECT_NEAR(image.At(7,7)[0],.625f,.001f);
    EXPECT_NEAR(image.At(7,7)[2],.375f,.001f);
}

TEST_F(ParticleTilesWarp, CascadesMultiplyAllCastersAndClipAtNearestReverseZReceiver) {
    auto path=std::filesystem::path(SRI_SOURCE_ROOT)/"engine/shaders/ParticleShadows.hlsl";
    auto vs=ShaderContext::CompileCpu(path,ShaderType::Vertex,{}), ps=ShaderContext::CompileCpu(path,ShaderType::Pixel,{});
    ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
    Check(device->CreateVertexShader(vs.blob->GetBufferPointer(),vs.blob->GetBufferSize(),nullptr,&vertex),"shadow VS");
    Check(device->CreatePixelShader(ps.blob->GetBufferPointer(),ps.blob->GetBufferSize(),nullptr,&pixel),"shadow PS");
    ParticleShadows shadows; Check(shadows.Initialize(device.Get(),{vertex.Get(),pixel.Get()},128),"shadow helper");
    ParticleTextures textures; InitTextures(textures); auto white=SolidTexture(0xffffffff); UINT layer;
    Check(textures.Resolve(white.Get(),layer),"white layer");
    TestStyle style; style.flags[0]=layer; style.sizeOpacity[2]=style.sizeOpacity[3]=.01f;
    auto styles=Structured<TestStyle>({&style,1});
    std::vector<Particle> particles(100,MakeParticle(0,0,1,1,.5f,{1,1,1},1));
    std::vector<UINT> ids(100); for(UINT i=0;i<100;++i) { ids[i]=i; particles[i].meta[1]=0; }
    auto pool=Structured<Particle>(particles), indices=Structured<UINT>(ids); UINT count=100;
    auto live=Structured<UINT>({&count,1});
    CascadeInfo cascades;
    for(auto& matrix:cascades.bufferData.viewProjections) { matrix=Matrix::Identity; matrix._33=.25f; matrix._43=.75f; }
    constexpr UINT size=ParticleShadowMapSize, center=size/2;
    // Non-integral scaling exercises floor/ceil receiver footprint bounds.
    constexpr UINT opaqueSize=size*2+1, receiverTexel=opaqueSize/2;
    D3D11_TEXTURE2D_DESC t{}; t.Width=t.Height=opaqueSize; t.ArraySize=4; t.MipLevels=t.SampleDesc.Count=1;
    t.Format=DXGI_FORMAT_R32_FLOAT; t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::vector<float> depth(opaqueSize*opaqueSize,.25f); // world z=-1 -> reverse depth .5
    // This receiver overlaps both neighboring low-res texels at the fractional boundary.
    depth[receiverTexel*opaqueSize+receiverTexel]=.75f;
    std::array<D3D11_SUBRESOURCE_DATA,4> data; for(auto& d:data) d={depth.data(),opaqueSize*4,0};
    ComPtr<ID3D11Texture2D> opaque; ComPtr<ID3D11ShaderResourceView> opaqueView;
    Check(device->CreateTexture2D(&t,data.data(),&opaque),"opaque cascades");
    Check(device->CreateShaderResourceView(opaque.Get(),nullptr,&opaqueView),"opaque view");
    ParticleSimulation::Views views{pool.Get(),indices.Get(),live.Get(),nullptr,100,styles.Get()};
    Check(shadows.Render(views,textures.View(),cascades,opaqueView.Get()),"render shadows");
    for(UINT slice=0;slice<4;++slice) {
        EXPECT_NEAR(ReadArray(shadows.View(),slice,center-2,center-2),std::pow(.99f,100),.015f);
        EXPECT_EQ(ReadArray(shadows.View(),slice,center,center),1);
        EXPECT_EQ(ReadArray(shadows.View(),slice,center-1,center-1),1);
    }
    Check(shadows.Render({},textures.View(),cascades,opaqueView.Get()),"empty shadows");
    EXPECT_EQ(ReadArray(shadows.View(),0,center-2,center-2),1);
    // Custom hook opacity is shared with color; density squares transmittance, no camera soft fade.
    Particle custom=MakeParticle(0,0,1,1,.5f,{1,1,1},1);
    auto customPool=Structured<Particle>({&custom,1}); count=1; live=Structured<UINT>({&count,1});
    style.sizeOpacity[2]=style.sizeOpacity[3]=.5f; style.variationDensity[2]=2; style.colorEndSoft[3]=1000;
    styles=Structured<TestStyle>({&style,1}); views={customPool.Get(),indices.Get(),live.Get(),nullptr,1,styles.Get()};
    Check(shadows.Render(views,textures.View(),cascades,opaqueView.Get()),"hook shadow");
    EXPECT_NEAR(ReadArray(shadows.View(),0,center-2,center-2),.5625f,.002f);

    // Explicit texture alpha modulates the hook in both camera and light views.
    // A transparent colored image must neither show a fallback circle nor cast a shadow.
    auto transparentRed=SolidTexture(0x000000ff); UINT transparentLayer;
    Check(textures.Resolve(transparentRed.Get(),transparentLayer),"transparent custom layer");
    style.flags[0]=transparentLayer;
    styles=Structured<TestStyle>({&style,1}); views.settings=styles.Get();
    Check(shadows.Render(views,textures.View(),cascades,opaqueView.Get()),"transparent hook shadow");
    EXPECT_EQ(ReadArray(shadows.View(),0,center-2,center-2),1);
    auto image=Render(9,9,Ortho(9,9),{custom},{0},std::vector<float>(81),styles.Get(),textures.View());
    EXPECT_EQ(image.At(4,4),(Color{.25f,.25f,.25f,.25f}));

    // Half-opacity texture, half-opacity hook, half-opacity style: density two
    // gives (1 - .5 * .5 * (128/255))^2 transmission near the billboard center.
    auto halfRed=SolidTexture(0x800000ff); UINT halfLayer;
    Check(textures.Resolve(halfRed.Get(),halfLayer),"half-opacity custom layer");
    style.flags[0]=halfLayer;
    styles=Structured<TestStyle>({&style,1}); views.settings=styles.Get();
    Check(shadows.Render(views,textures.View(),cascades,opaqueView.Get()),"textured hook shadow");
    EXPECT_NEAR(ReadArray(shadows.View(),0,center-2,center-2),std::pow(1-.25f*(128.f/255),2),.002f);
    image=Render(9,9,Ortho(9,9),{custom},{0},std::vector<float>(81),styles.Get(),textures.View());
    EXPECT_NEAR(image.At(4,4)[0],.25f+.75f*.25f*(128.f/255),.001f);
    EXPECT_NEAR(image.At(4,4)[1],.25f*(1-.25f*(128.f/255)),.001f);

    style.flags[2]=0; styles=Structured<TestStyle>({&style,1}); views.settings=styles.Get();
    Check(shadows.Render(views,textures.View(),cascades,opaqueView.Get()),"noncasting shadow");
    EXPECT_EQ(ReadArray(shadows.View(),0,center-2,center-2),1);
}

TEST_F(ParticleTilesWarp, RotatedBoundsMatchLifetimeSizeColorOpacityAndFadeIn) {
    ParticleTextures textures; InitTextures(textures); auto white=SolidTexture(0xffffffff); UINT layer;
    Check(textures.Resolve(white.Get(),layer),"white layer");
    TestStyle style; style.flags[0]=layer; style.flags[3]=1;
    style.sizeOpacity[0]=1; style.sizeOpacity[1]=3; style.sizeOpacity[2]=.8f; style.sizeOpacity[3]=.4f;
    style.colorStartFade[0]=1; style.colorStartFade[1]=style.colorStartFade[2]=0; style.colorStartFade[3]=1;
    style.colorEndSoft[0]=style.colorEndSoft[1]=0; style.colorEndSoft[2]=1;
    auto styles=Structured<TestStyle>({&style,1});
    auto p=MakeParticle(0,0,2,2,.5f,{1,1,1},536870912); p.meta[1]=0;
    auto image=Render(13,13,Ortho(13,13),{p},{0},std::vector<float>(169),styles.Get(),textures.View());
    EXPECT_NEAR(image.At(11,6)[0],.325f,.001f);
    EXPECT_NEAR(image.At(11,6)[1],.175f,.001f);
    EXPECT_NEAR(image.At(11,6)[2],.325f,.001f);
    EXPECT_EQ(image.At(12,6),(Color{.25f,.25f,.25f,.25f}));
}
} // namespace
