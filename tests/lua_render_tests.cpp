#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/PostProcessDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/PostProcessing.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace {
using namespace engine;

class LuaRenderTest : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("lua-render-" + NewUuid());
    HWND window{};
    Engine host{EngineConfig{.taskThreads = 1}};
    RenderingSubsystem* rendering{};
    LuaRuntime lua;
    std::string error;

    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        window = CreateWindowExW(0, L"STATIC", L"Lua Render tests", WS_OVERLAPPEDWINDOW,
            0, 0, 96, 72, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        rendering = host.AddSubsystem<RenderingSubsystem>();
        ASSERT_NE(rendering, nullptr);
        auto& renderer = rendering->GetRendering();
        host.SetSurface({window, 96, 72});
        const bool initialized = host.Initialize();
        const auto diagnostics = renderer.GetDiagnostics();
        if (!initialized && diagnostics.failure && diagnostics.failure->operation == "D3D11CreateDeviceAndSwapChain")
            GTEST_SKIP() << diagnostics.failure->summary;
        ASSERT_TRUE(initialized) << (diagnostics.failure ? diagnostics.failure->detail : "");
        renderer.GetWorld().camera = FPSCamera{0.8f, 96.f / 72.f, 0.1f, 100.f};
        renderer.GetWorld().directionalLight =
            {{0, 0, 0}, {0, -1, -1}, {1, 1, 1}, 0, 0, LightInfo::Directional};
        ASSERT_TRUE(lua.Initialize(root, host, error)) << error;
    }

    void TearDown() override {
        lua.Shutdown();
        host.Shutdown();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    ScriptInstanceDesc Script(std::string_view source) {
        ScriptInstanceDesc desc{root / (NewUuid() + ".lua"), NewUuid(), NewUuid()};
        EXPECT_TRUE(CreateNewFile(desc.source, source, error)) << error;
        return desc;
    }

    void Render() {
        rendering->GetRendering().RenderFrame(FrameContext{});
        const auto diagnostics = rendering->GetDiagnostics();
        EXPECT_FALSE(diagnostics.failure.has_value()) << (diagnostics.failure ? diagnostics.failure->detail : "");
    }
};

TEST_F(LuaRenderTest, ExecutesWithoutUiAndRevokesContextAndBorrowedTextures) {
    auto desc = Script(R"(
        local calls, oldContext, oldTexture = 0
        local target
        return {
            OnStart = function(self)
                target = Render.CreateRenderTarget{scale=0.5, format='RGBA16F'}
                self.pass = Render.AddFullscreenPass{
                    name='lifecycle', stage=Render.Stage.AfterTonemap,
                    execute=function(ctx, frame)
                        local w,h = ctx:GetTextureSize(target)
                        assert(w == 48 and h == 36)
                        if calls > 0 then
                            assert(not pcall(function() oldContext:GetTextureSize(target) end))
                            assert(not pcall(function() ctx:GetTextureSize(oldTexture) end))
                        end
                        local iw,ih = ctx:GetTextureSize(frame.inputColor)
                        assert(iw == frame.inputWidth and ih == frame.inputHeight)
                        ctx:ClearRenderTarget(frame.outputColor, {1,0,0,1})
                        oldContext, oldTexture = ctx, frame.inputColor
                        calls = calls + 1
                    end
                }
            end,
            OnUpdate = function(self)
                assert(calls == 2)
                assert(not pcall(function() oldContext:DrawFullscreen() end))
                self.pass:SetEnabled(false)
            end,
            OnDestroy = function(self)
                assert(not pcall(function() Render.CreateRenderTarget{scale=1} end))
            end
        }
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    Render();
    Render();
    ASSERT_TRUE(lua.UpdateInstance(id, 0, error)) << error;
    ASSERT_TRUE(lua.DestroyInstance(id, error)) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasPasses());
}

TEST_F(LuaRenderTest, CallbackFaultRetiresEveryPassOfOwner) {
    auto desc = Script(R"(
        return {OnStart=function(self)
            Render.AddFullscreenPass{name='fault', stage=Render.Stage.AfterTonemap, order=-1,
                execute=function() error('intentional render callback fault') end}
            Render.AddFullscreenPass{name='must not run', stage=Render.Stage.AfterTonemap,
                execute=function() error('retired callback executed') end}
        end}
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    Render();
    EXPECT_FALSE(lua.UpdateInstance(id, 0, error));
    EXPECT_NE(error.find("intentional render callback fault"), std::string::npos) << error;
    EXPECT_NE(error.find("Render.execute"), std::string::npos) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasPasses());
    lua.Shutdown();
    Render(); // Queued native callbacks must not hold a dead sol state.
}

TEST_F(LuaRenderTest, RemovingLaterPassDuringExecuteCancelsItsCallback) {
    auto desc = Script(R"(
        local later, calls = nil, 0
        return {
            OnStart=function()
                Render.AddFullscreenPass{name='remove later', stage=Render.Stage.AfterTonemap, order=-1,
                    execute=function()
                        calls = calls + 1
                        later:Remove()
                    end}
                later = Render.AddFullscreenPass{name='cancelled', stage=Render.Stage.AfterTonemap,
                    execute=function() error('removed callback executed') end}
            end,
            OnUpdate=function() assert(calls == 1) end
        }
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    Render();
    ASSERT_TRUE(lua.UpdateInstance(id, 0, error)) << error;
    ASSERT_TRUE(lua.DestroyInstance(id, error)) << error;
    Render();
}

TEST_F(LuaRenderTest, HandlesCannotBeUsedByAnotherOwnerOrAfterDestroy) {
    auto first = Script(R"(
        return {OnStart=function()
            _G.savedRenderPass = Render.AddFullscreenPass{
                name='owner', stage=Render.Stage.AfterUI, execute=function() end}
            -- Module globals are shared; use a module to transfer the handle.
            require('shared').pass = _G.savedRenderPass
        end}
    )");
    ASSERT_TRUE(CreateNewFile(root / "shared.lua", "return {}", error)) << error;
    auto second = Script(R"(
        return {
            OnStart=function()
                assert(not pcall(function() require('shared').pass:SetEnabled(false) end))
            end,
            OnUpdate=function()
                assert(not pcall(function() require('shared').pass:Remove() end))
            end
        }
    )");
    InstanceId a, b;
    ASSERT_TRUE(lua.CreateInstance(first, a, error)) << error;
    ASSERT_TRUE(lua.CreateInstance(second, b, error)) << error;
    ASSERT_TRUE(lua.StartInstance(a, error)) << error;
    ASSERT_TRUE(lua.StartInstance(b, error)) << error;
    ASSERT_TRUE(lua.DestroyInstance(a, error)) << error;
    ASSERT_TRUE(lua.UpdateInstance(b, 0, error)) << error;
    Render();
}

TEST_F(LuaRenderTest, CatalogShaderAndReflectedParameterShapesExecute) {
    const auto shaderFile = root / "parameters.hlsl";
    ASSERT_TRUE(CreateNewFile(shaderFile, R"(
        cbuffer Parameters : register(b5) {
            float4 Tint;
            row_major float2x3 Transform;
            float2 Samples[2];
            int Count;
            uint Index;
            bool Enabled;
        };
        Texture2D Extra : register(t16);
        SamplerState PointClamp : register(s0);
        float4 PSMain(float4 position : SV_Position) : SV_Target {
            return Tint * Extra.SampleLevel(PointClamp, float2(.5,.5), 0) +
                Transform[0][0] + Transform[1][2] + Samples[0].x +
                Samples[1].y + Count + Index + (Enabled ? 1 : 0);
        }
    )", error)) << error;
    AssetMetadata shaderMetadata;
    shaderMetadata.id = NewUuid();
    shaderMetadata.kind = CatalogAssetKind::Shader;
    ASSERT_TRUE(WriteAssetMetadata(shaderFile, shaderMetadata, error)) << error;
    PostProcessDefinition definition;
    definition.shader = {shaderMetadata.id, "game/parameters.hlsl"};
    std::string json;
    ASSERT_TRUE(SerializePostProcessDefinition(definition, json, error)) << error;
    const auto descriptorFile = root / "parameters.postprocess.json";
    ASSERT_TRUE(CreateNewFile(descriptorFile, json, error)) << error;
    AssetMetadata descriptorMetadata;
    descriptorMetadata.id = NewUuid();
    descriptorMetadata.kind = CatalogAssetKind::PostProcess;
    ASSERT_TRUE(WriteAssetMetadata(descriptorFile, descriptorMetadata, error)) << error;
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;

    auto desc = Script(R"(
        local calls = 0
        return {
            OnStart=function(self)
                local shader = Render.LoadShader('game/parameters.postprocess.json')
                local texture = Render.LoadTexture('engine/primitives/white/texture.png')
                local buffer = Render.CreateConstantBuffer{shader=shader, name='Parameters',
                    values={Tint={0,0,0,1}, Transform={{0,0,0},{0,0,0}},
                        Samples={{0,0},{0,0}}, Count=0, Index=0, Enabled=false}}
                assert(not pcall(function() buffer:Set{Transform={{1,2},{3,4,5,6}}} end))
                assert(not pcall(function() buffer:Set{Samples={{1},{2,3,4}}} end))
                assert(not pcall(function() buffer:Set('Count', 0.5) end))
                assert(not pcall(function() buffer:Set('Index', -1) end))
                assert(not pcall(function() buffer:Set('Enabled', 1) end))
                assert(not pcall(function() buffer:Set('Missing', 1) end))
                buffer:Set('Transform', {0,0,0,0,0,0})
                buffer:Set{Enabled=true, Tint={0.1,0.2,0.3,1}}
                Render.AddFullscreenPass{name='parameters', stage=Render.Stage.AfterTonemap,
                    execute=function(ctx, frame)
                        buffer:Set('Enabled', false)
                        assert(not pcall(function() ctx:BindConstantBuffer(4, buffer) end))
                        assert(not pcall(function() ctx:BindConstantBuffer(6, buffer) end))
                        ctx:SetPixelShader(shader)
                        local w,h = ctx:GetTextureSize(texture)
                        assert(w > 0 and h > 0)
                        assert(not pcall(function() ctx:SetRenderTarget(texture) end))
                        ctx:SetRenderTarget(frame.outputColor)
                        ctx:BindTexture(16, texture)
                        ctx:BindConstantBuffer(5, buffer)
                        ctx:DrawFullscreen()
                        calls = calls + 1
                    end}
            end,
            OnUpdate=function() assert(calls == 2) end
        }
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    Render();
    host.GetAssets().UnloadAll(); // The pass retains its read-only GPU snapshot.
    Render();
    EXPECT_TRUE(lua.UpdateInstance(id, 0, error)) << error;
}

TEST(LuaRenderHeadless, ChunkCannotCreateAndStartedScriptFailsCleanlyWithoutRenderer) {
    const auto root = std::filesystem::temp_directory_path() / ("lua-render-headless-" + NewUuid());
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } cleanup{root};
    std::string error;
    const auto path = root / "headless.lua";
    ASSERT_TRUE(CreateNewFile(path, R"(
        assert(Render.Stage.BeforeTonemap ~= nil)
        assert(not pcall(function() Render.CreateRenderTarget{scale=1} end))
        return {OnStart=function()
            assert(not pcall(function() Render.LoadShader('not-an-asset.hlsl') end))
        end}
    )", error)) << error;
    LuaRuntime lua;
    ASSERT_TRUE(lua.Initialize(root, error)) << error;
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance({path, NewUuid(), NewUuid()}, id, error)) << error;
    EXPECT_TRUE(lua.StartInstance(id, error)) << error;
    EXPECT_TRUE(lua.DestroyInstance(id, error)) << error;
}
}
