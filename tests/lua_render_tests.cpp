#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/PostProcessDefinition.hpp>
#include <engine/assets/RenderPipelineDefinition.hpp>
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
            Render.SetBuiltinTonemappingEnabled(false)
            Render.AddFullscreenPass{name='fault', stage=Render.Stage.BeforeTonemap, order=-1,
                execute=function() error('intentional render callback fault') end}
            Render.AddFullscreenPass{name='must not run', stage=Render.Stage.AfterTonemap,
                execute=function() error('retired callback executed') end}
        end}
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().IsBuiltinTonemappingEnabled());
    Render();
    EXPECT_TRUE(rendering->GetRendering().GetPostProcessing().IsBuiltinTonemappingEnabled());
    EXPECT_FALSE(lua.UpdateInstance(id, 0, error));
    EXPECT_NE(error.find("intentional render callback fault"), std::string::npos) << error;
    EXPECT_NE(error.find("Render.execute"), std::string::npos) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasPasses());
    lua.Shutdown();
    Render(); // Queued native callbacks must not hold a dead sol state.
}

TEST_F(LuaRenderTest, TonemappingBypassIsValidatedAndReleasedByEachOwnerAndSession) {
    auto desc = Script(R"(
        assert(not pcall(function() Render.SetBuiltinTonemappingEnabled(false) end))
        return {
            OnStart=function()
                for _, value in ipairs({0, 1, 'false', {}}) do
                    assert(not pcall(function() Render.SetBuiltinTonemappingEnabled(value) end))
                end
                assert(not pcall(function() Render.SetBuiltinTonemappingEnabled() end))
                Render.SetBuiltinTonemappingEnabled(false)
                Render.SetBuiltinTonemappingEnabled(false)
                Render.AddFullscreenPass{name='check scope', stage=Render.Stage.BeforeTonemap,
                    execute=function(ctx, frame)
                        assert(not pcall(function() Render.SetBuiltinTonemappingEnabled(true) end))
                        ctx:ClearRenderTarget(frame.outputColor, {.2, .3, .4, 1})
                    end}
            end,
            OnUpdate=function() Render.SetBuiltinTonemappingEnabled(true) end,
            OnDestroy=function()
                assert(not pcall(function() Render.SetBuiltinTonemappingEnabled(false) end))
            end
        }
    )");
    auto& effects = rendering->GetRendering().GetPostProcessing();
    EXPECT_TRUE(effects.IsBuiltinTonemappingEnabled());
    InstanceId first, second;
    ASSERT_TRUE(lua.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(lua.CreateInstance(desc, second, error)) << error;
    ASSERT_TRUE(lua.StartInstance(first, error)) << error;
    ASSERT_TRUE(lua.StartInstance(second, error)) << error;
    Render();
    ASSERT_TRUE(lua.UpdateInstance(first, 0, error)) << error;
    EXPECT_FALSE(effects.IsBuiltinTonemappingEnabled());
    ASSERT_TRUE(lua.DestroyInstance(second, error)) << error;
    EXPECT_TRUE(effects.IsBuiltinTonemappingEnabled());
    ASSERT_TRUE(lua.DestroyInstance(first, error)) << error;

    ASSERT_TRUE(lua.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(lua.StartInstance(first, error)) << error;
    EXPECT_FALSE(effects.IsBuiltinTonemappingEnabled());
    lua.Shutdown();
    EXPECT_TRUE(effects.IsBuiltinTonemappingEnabled());
    ASSERT_TRUE(lua.Initialize(root, host, error)) << error;
    Render();
    EXPECT_TRUE(effects.IsBuiltinTonemappingEnabled());
}

TEST_F(LuaRenderTest, StartAndUpdateFaultsRestoreBuiltinTonemapping) {
    auto failedStart = Script(R"(
        return {OnStart=function()
            Render.SetBuiltinTonemappingEnabled(false)
            error('mapper setup failed')
        end}
    )");
    auto failedUpdate = Script(R"(
        return {
            OnStart=function() Render.SetBuiltinTonemappingEnabled(false) end,
            OnUpdate=function() error('mapper update failed') end
        }
    )");
    auto& effects = rendering->GetRendering().GetPostProcessing();
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(failedStart, id, error)) << error;
    EXPECT_FALSE(lua.StartInstance(id, error));
    EXPECT_NE(error.find("mapper setup failed"), std::string::npos) << error;
    EXPECT_TRUE(effects.IsBuiltinTonemappingEnabled());
    ASSERT_TRUE(lua.CreateInstance(failedUpdate, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    EXPECT_FALSE(effects.IsBuiltinTonemappingEnabled());
    EXPECT_FALSE(lua.UpdateInstance(id, 0, error));
    EXPECT_NE(error.find("mapper update failed"), std::string::npos) << error;
    EXPECT_TRUE(effects.IsBuiltinTonemappingEnabled());
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

TEST_F(LuaRenderTest, ProceduralLifecycleRevokesContextAndRemovesLaterCallback) {
    auto desc = Script(R"(
        local calls, oldContext, later = 0
        local target
        return {
            OnStart=function(self)
                target = Render.CreateRenderTarget{width=7,height=9}
                assert(not pcall(function()
                    Render.AddProceduralPass{name='bad stage',stage=Render.Stage.AfterUI,execute=function() end}
                end))
                for _,value in ipairs({false, '0', 0.5, math.huge, {}}) do
                    assert(not pcall(function()
                        Render.AddProceduralPass{name='bad order',order=value,execute=function() end}
                    end))
                end
                assert(not pcall(function() Render.AddProceduralPass{name='bad execute',execute=3} end))
                self.pass = Render.AddProceduralPass{name='procedural lifecycle',order=-1,
                    execute=function(ctx,frame)
                        assert(frame.width == 96 and frame.height == 72)
                        local fields = 0
                        for key in pairs(frame) do
                            assert(key == 'width' or key == 'height')
                            fields = fields + 1
                        end
                        assert(fields == 2)
                        assert(ctx.SetRenderTarget == nil and ctx.ClearRenderTarget == nil and ctx.DrawFullscreen == nil)
                        local w,h = ctx:GetTextureSize(target)
                        assert(w == 7 and h == 9)
                        if oldContext then
                            assert(not pcall(function() oldContext:GetTextureSize(target) end))
                        end
                        assert(not pcall(function() Render.CreateRenderTarget{scale=1} end))
                        assert(not pcall(function() Render.LoadPipeline('missing.pipeline.json') end))
                        assert(not pcall(function()
                            Render.AddProceduralPass{name='nested',execute=function() end}
                        end))
                        for _,value in ipairs({-1, 1.5, 4294967296, '3', false, {}, math.huge, 0/0}) do
                            assert(not pcall(function() ctx:DrawProcedural(value) end))
                            assert(not pcall(function() ctx:DrawProcedural(3,value) end))
                            assert(not pcall(function() ctx:BindTexture(value,target) end))
                        end
                        assert(not pcall(function() ctx:DrawProcedural() end))
                        for _,value in ipairs({'pixel', 'Geometry', 1, false, {}}) do
                            assert(not pcall(function() ctx:BindTexture(16,target,value) end))
                        end
                        assert(not pcall(function() ctx:SetPipeline(target) end))
                        assert(not pcall(function() ctx:BindConstantBuffer(5,target) end))
                        oldContext = ctx
                        calls = calls + 1
                        if calls == 1 then later:Remove() end
                    end}
                later = Render.AddProceduralPass{name='removed',execute=function() error('removed callback ran') end}
            end,
            OnUpdate=function(self)
                assert(calls == 2)
                assert(not pcall(function() oldContext:GetTextureSize(target) end))
                self.pass:SetEnabled(false)
                self.pass:Remove()
            end,
            OnDestroy=function()
                assert(not pcall(function()
                    Render.AddProceduralPass{name='teardown',execute=function() end}
                end))
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
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasProceduralPasses());
}

TEST_F(LuaRenderTest, ProceduralFaultRetiresMixedPassesAndShutdownRevokesCallbacks) {
    auto desc = Script(R"(
        return {OnStart=function()
            Render.SetBuiltinTonemappingEnabled(false)
            Render.AddProceduralPass{name='fault',order=-1,
                execute=function() error('intentional procedural fault') end}
            Render.AddProceduralPass{name='retired procedural',
                execute=function() error('retired procedural executed') end}
            Render.AddFullscreenPass{name='retired fullscreen',stage=Render.Stage.AfterUI,
                execute=function() error('retired fullscreen executed') end}
        end}
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    Render();
    EXPECT_TRUE(rendering->GetRendering().GetPostProcessing().IsBuiltinTonemappingEnabled());
    EXPECT_FALSE(lua.UpdateInstance(id, 0, error));
    EXPECT_NE(error.find("intentional procedural fault"), std::string::npos) << error;
    EXPECT_NE(error.find("Render.execute"), std::string::npos) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasProceduralPasses());
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasPasses());

    auto pending = Script(R"(
        return {OnStart=function()
            Render.AddProceduralPass{name='pending shutdown',execute=function() error('dead Lua state') end}
        end}
    )");
    ASSERT_TRUE(lua.CreateInstance(pending, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    lua.Shutdown();
    Render();
    ASSERT_TRUE(lua.Initialize(root, host, error)) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasProceduralPasses());
}

TEST_F(LuaRenderTest, ProceduralRemovalAndOwnerRetirementReleaseLuaClosures) {
    ASSERT_TRUE(CreateNewFile(root / "shared.lua", "return {}", error)) << error;
    auto desc = Script(R"(
        return {OnStart=function()
            local shared = require('shared')
            shared.weak = setmetatable({}, {__mode='v'})
            do
                local marker = {}
                shared.weak[1] = marker
                local pass = Render.AddProceduralPass{name='removed closure',
                    execute=function() assert(marker) end}
                pass:Remove()
            end
            do
                local marker = {}
                shared.weak[2] = marker
                Render.AddProceduralPass{name='retired closure',
                    execute=function() assert(marker) end}
            end
            collectgarbage('collect')
            assert(shared.weak[1] == nil)
            assert(shared.weak[2] ~= nil)
        end}
    )");
    auto observer = Script(R"(
        return {OnStart=function()
            collectgarbage('collect')
            assert(require('shared').weak[1] == nil)
            assert(require('shared').weak[2] == nil)
        end}
    )");
    InstanceId id;
    ASSERT_TRUE(lua.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    ASSERT_TRUE(lua.DestroyInstance(id, error)) << error;
    ASSERT_TRUE(lua.CreateInstance(observer, id, error)) << error;
    ASSERT_TRUE(lua.StartInstance(id, error)) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasProceduralPasses());
}

TEST_F(LuaRenderTest, CatalogPipelineReflectsBothStagesDrawsAndGuardsPropertiesByOwner) {
    const auto shaderFile = root / "procedural.hlsl";
    ASSERT_TRUE(CreateNewFile(shaderFile, R"(
        cbuffer VertexParameters : register(b5) { float Offset; };
        cbuffer PixelParameters : register(b5) { float4 Tint; };
        struct Vertex { float4 position : SV_Position; };
        Vertex VSMain(uint id : SV_VertexID, uint instance : SV_InstanceID) {
            Vertex output;
            float2 positions[3] = {float2(-.5,-.5),float2(0,.5),float2(.5,-.5)};
            output.position = float4(positions[id % 3] + float2(Offset + instance * .01,0),.5,1);
            return output;
        }
        float4 PSMain(Vertex input) : SV_Target { return Tint; }
    )", error)) << error;
    AssetMetadata shaderMetadata;
    shaderMetadata.id = NewUuid();
    shaderMetadata.kind = CatalogAssetKind::Shader;
    ASSERT_TRUE(WriteAssetMetadata(shaderFile, shaderMetadata, error)) << error;
    RenderPipelineDefinition definition;
    definition.shader = {shaderMetadata.id, "game/procedural.hlsl"};
    definition.depth = ProceduralDepth::Disabled;
    std::string json;
    ASSERT_TRUE(SerializeRenderPipelineDefinition(definition, json, error)) << error;
    const auto pipelineFile = root / "procedural.pipeline.json";
    ASSERT_TRUE(CreateNewFile(pipelineFile, json, error)) << error;
    AssetMetadata metadata;
    metadata.id = NewUuid();
    metadata.kind = CatalogAssetKind::RenderPipeline;
    ASSERT_TRUE(WriteAssetMetadata(pipelineFile, metadata, error)) << error;
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    ASSERT_TRUE(CreateNewFile(root / "shared.lua", "return {}", error)) << error;
    auto desc = Script(std::string(R"(
        local calls, pipeline = 0
        return {
            OnStart=function()
                assert(not pcall(function() Render.LoadPipeline('game/procedural.hlsl') end))
                pipeline = Render.LoadPipeline('game/procedural.pipeline.json')
                local byId = Render.LoadPipeline(')") + metadata.id.ToString() + R"(')
                assert(byId.vertexShader ~= nil and byId.pixelShader ~= nil)
                require('shared').pipeline = pipeline
                assert(not pcall(function() pipeline.vertexShader = pipeline.pixelShader end))
                assert(not pcall(function() pipeline.pixelShader = pipeline.vertexShader end))
                local vertex = Render.CreateConstantBuffer{shader=pipeline.vertexShader,
                    name='VertexParameters',values={Offset=0}}
                local pixel = Render.CreateConstantBuffer{shader=pipeline.pixelShader,
                    name='PixelParameters',values={Tint={.1,.2,.3,1}}}
                assert(not pcall(function()
                    Render.CreateConstantBuffer{shader=pipeline.vertexShader,name='PixelParameters'}
                end))
                assert(not pcall(function()
                    Render.CreateConstantBuffer{shader=pipeline.pixelShader,name='VertexParameters'}
                end))
                local texture = Render.LoadTexture('engine/primitives/white/texture.png')
                Render.AddProceduralPass{name='pipeline',execute=function(ctx,frame)
                    vertex:Set('Offset', 0)
                    pixel:Set{Tint={.2,.3,.4,1}}
                    ctx:SetPipeline(pipeline)
                    ctx:BindConstantBuffer(5,vertex)
                    ctx:BindConstantBuffer(5,pixel)
                    ctx:BindTexture(16,texture)
                    ctx:BindTexture(16,texture,'Pixel')
                    ctx:BindTexture(16,texture,'Vertex')
                    ctx:DrawProcedural(3,0)
                    ctx:DrawProcedural(3)
                    ctx:DrawProcedural(3,2)
                    calls = calls + 1
                end}
            end,
            OnUpdate=function() assert(calls == 2) end,
            OnDestroy=function()
                assert(not pcall(function() return pipeline.vertexShader end))
                assert(not pcall(function() return pipeline.pixelShader end))
            end
        }
    )");
    auto foreign = Script(R"(
        return {
            OnStart=function()
                local pipeline = require('shared').pipeline
                assert(not pcall(function() return pipeline.vertexShader end))
                assert(not pcall(function() return pipeline.pixelShader end))
            end,
            OnUpdate=function()
                local pipeline = require('shared').pipeline
                assert(not pcall(function() return pipeline.vertexShader end))
                assert(not pcall(function() return pipeline.pixelShader end))
            end
        }
    )");
    InstanceId first, second;
    ASSERT_TRUE(lua.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(lua.StartInstance(first, error)) << error;
    ASSERT_TRUE(lua.CreateInstance(foreign, second, error)) << error;
    ASSERT_TRUE(lua.StartInstance(second, error)) << error;
    Render();
    Render();
    ASSERT_TRUE(lua.UpdateInstance(first, 0, error)) << error;
    ASSERT_TRUE(lua.DestroyInstance(first, error)) << error;
    ASSERT_TRUE(lua.UpdateInstance(second, 0, error)) << error;
    Render();
    EXPECT_FALSE(rendering->GetRendering().GetPostProcessing().HasProceduralPasses());
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
        assert(not pcall(function() Render.LoadPipeline('not-an-asset.pipeline.json') end))
        assert(not pcall(function() Render.AddProceduralPass{name='chunk',execute=function() end} end))
        return {OnStart=function()
            assert(not pcall(function() Render.LoadShader('not-an-asset.hlsl') end))
            assert(not pcall(function() Render.LoadPipeline('not-an-asset.pipeline.json') end))
            assert(not pcall(function() Render.AddProceduralPass{name='headless',execute=function() end} end))
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
