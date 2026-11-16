#include <engine/audio/AudioSubsystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/scripting/LuaBindings.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace engine;
struct LuaAudioTests : testing::Test {
    Engine host{EngineConfig{.taskThreads = 1}};
    LuaBindingsContext context{.engine = &host};
    sol::state lua;

    void SetUp() override {
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string);
        using DirectX::SimpleMath::Vector3;
        lua.new_usertype<Vector3>("Vector3", sol::constructors<Vector3(), Vector3(float, float, float)>());
        // LuaRuntime normally creates this before registering the binding groups.
        // Match the existing direct gameplay-binding fixture's bootstrap.
        lua.create_named_table("Scene");
        RegisterLuaBindings(lua, context);
    }
    void Lua(const char* source) {
        const auto result = lua.safe_script(source, sol::script_pass_on_error);
        if (!result.valid()) { const sol::error error = result; ADD_FAILURE() << error.what(); }
    }
};

struct LuaAudioOfflineTests : LuaAudioTests {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-lua-audio-" + NewUuid());
    std::filesystem::path previousRoot = AssetPathResolver::GetGameRoot();
    AudioSubsystem* audio = nullptr;
    void SetUp() override {
        LuaAudioTests::SetUp();
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        // One second of mono PCM; enough duration for control/fade tests without
        // sleeps, devices, or external assets.
        std::string wav;
        const auto u16 = [&](unsigned value) { wav.push_back(char(value & 255)); wav.push_back(char((value >> 8) & 255)); };
        const auto u32 = [&](unsigned value) { u16(value & 65535); u16(value >> 16); };
        wav += "RIFF"; u32(36 + 96000); wav += "WAVEfmt "; u32(16);
        u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16);
        wav += "data"; u32(96000);
        for (unsigned i = 0; i != 48000; ++i) u16(i % 100 < 50 ? 4000 : 61536);
        std::string error;
        ASSERT_TRUE(CreateNewFile(root / "tone.wav", wav, error)) << error;
        ASSERT_TRUE(WriteAssetMetadata(root / "tone.wav",
            AssetMetadata{AssetUuid("11111111-1111-4111-8111-111111111111"), CatalogAssetKind::AudioClip}, error)) << error;
        audio = host.AddSubsystem<AudioSubsystem>(AudioConfig{.output = AudioOutputMode::Offline});
        ASSERT_NE(audio, nullptr);
        ASSERT_TRUE(host.Initialize());
        (void)host.GetAssets().RescanCatalog(error);
        (void)host.GetSceneAssets().RescanCatalog(error);
        ASSERT_NE(host.GetAssets().GetCatalog().FindPath("game/tone.wav"), nullptr) << error;
        audio->BeginSession();
    }
    void TearDown() override {
        host.Shutdown();
        AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
}

TEST_F(LuaAudioTests, UnavailableRuntimeAndMalformedCallsReturnDiagnostics) {
    Lua(R"(
        local id = '11111111-1111-4111-8111-111111111111'
        assert(not Audio.IsAvailable() and #Audio.GetDiagnostic() > 0)
        local function failure(fn, ...)
            local value, err = fn(...)
            assert(not value and type(err) == 'string' and #err > 0)
        end
        failure(Audio.Play2D, id)
        failure(Audio.Play2D)
        failure(Audio.Play2D, {})
        failure(Audio.Play2D, id, false)
        failure(Audio.Play2D, id, {volume=0/0})
        failure(Audio.Play2D, id, {volume=1.00000001})
        failure(Audio.Play2D, id, {pitch=1e300})
        failure(Audio.Play2D, id, {pitch=0})
        failure(Audio.Play2D, id, {loading='Unknown'})
        failure(Audio.Play2D, id, {bus='Master'})
        failure(Audio.Play2D, id, {scope='preview'})
        failure(Audio.Play2D, id, {looping=1})
        failure(Audio.Play2D, id, {minDistance=10, maxDistance=1})
        failure(Audio.Play2D, id, {rolloff=-1})
        failure(Audio.Play2D, id, {typo=1})
        failure(Audio.PlayAt, id, Vector3.new(0,0,0), {scope='session'})
        failure(Audio.PlayAt, id, Vector3.new(0/0,0,0))
        failure(Audio.PlayAt, id, {})
        failure(Audio.PlaySource, {})
        failure(Audio.PauseSource, nil)
        failure(Audio.ResumeSource, false)
        failure(Audio.StopSource)
        failure(Audio.GetSourceVoice, {})
        failure(Audio.SetMasterVolume, '1')
        failure(Audio.SetMasterVolume, math.huge)
        failure(Audio.SetBusVolume, 'Bad', 1)
        failure(Audio.GetMasterVolume)
        failure(Audio.GetBusVolume, 'SFX')
    )");
}

TEST_F(LuaAudioTests, ComponentCopiesValidateTransactionallyAndPreserveMissingUuid) {
    const auto entity = host.GetScene().CreateEntity("audio");
    lua["e"] = LuaEntityHandle::FromScene(host.GetScene(), entity);
    Lua(R"(
        e:AddComponent('AudioSource')
        e:AddComponent('AudioListener', {enabled=true})
        local c = e:GetComponent('AudioSource')
        assert(c.clip == '' and c.spatial == 'ThreeD' and c.loading == 'Buffered')
        assert(c.enabled and not c.playOnStart and not c.looping and c.volume == 1)
        c.volume = 0.25
        assert(e:GetComponent('AudioSource').volume == 1)
        e:SetComponent('AudioSource', {clip='11111111-1111-4111-8111-111111111111',
            bus='Music', spatial='TwoD', loading='Streamed', looping=true, volume=0.5})
        local function bad(patch)
            assert(not pcall(function() e:SetComponent('AudioSource', patch) end))
            local source = e:GetComponent('AudioSource')
            assert(source.volume == 0.5 and source.bus == 'Music' and source.enabled)
        end
        bad({enabled=false, volume=0/0})
        bad({volume=2})
        bad({volume=1.00000001})
        bad({pitch=0})
        bad({loading='Bad'})
        bad({spatial='Bad'})
        bad({attenuation='Bad'})
        bad({bus='Bad'})
        bad({clip=false})
        bad({clip='game/missing.wav'})
        bad({maxDistance=0})
        bad({minDistance=101})
        bad({rolloff=-1})
        bad({typo=1})
        e:SetComponent('AudioSource', {pitch=2})
        assert(e:GetComponent('AudioSource').clip == '11111111-1111-4111-8111-111111111111')
        e:SetComponent('AudioSource', {clip=''})
        assert(e:GetComponent('AudioSource').clip == '')
        assert(not pcall(function() e:SetComponent('AudioListener', {enabled=1}) end))
        assert(e:GetComponent('AudioListener').enabled)
    )");
    host.GetScene().DestroyEntity(entity);
    Lua(R"(
        local v, err = Audio.PlaySource(e)
        assert(v == nil and #err > 0)
        local ok, diagnostic = Audio.StopSource(e)
        assert(ok == false and #diagnostic > 0)
        assert(not pcall(function() e:GetComponent('AudioSource') end))
    )");
}

TEST_F(LuaAudioTests, GainControlsAndDetachedContext) {
    auto* audio = host.AddSubsystem<AudioSubsystem>(AudioConfig{.output = AudioOutputMode::Disabled});
    ASSERT_NE(audio, nullptr);
    ASSERT_TRUE(audio->Initialize());
    Lua(R"(
        local ok, err = Audio.SetMasterVolume(0.25)
        assert(ok and err == '')
        local gain, diagnostic = Audio.GetMasterVolume()
        assert(gain == 0.25 and diagnostic == '')
        assert(Audio.SetBusVolume('Music', 0.5))
        assert(Audio.GetBusVolume('Music') == 0.5)
        local ok = Audio.SetBusVolume('Music', -1)
        assert(not ok and Audio.GetBusVolume('Music') == 0.5)
    )");
    context.engine = nullptr;
    Lua(R"(
        assert(not Audio.IsAvailable())
        local ok, err = Audio.SetMasterVolume(1)
        assert(ok == false and #err > 0)
        collectgarbage('collect')
    )");
    audio->Shutdown();
}

TEST(LuaAudioRuntime, RegistersInStandaloneRuntimeAndCanReinitialize) {
    LuaRuntime runtime;
    std::string error;
    for (int i = 0; i != 2; ++i) {
        ASSERT_TRUE(runtime.Initialize(error)) << error;
        ASSERT_TRUE(runtime.ExecuteString(R"(
            assert(type(Audio) == 'table' and not Audio.IsAvailable())
            local voice, err = Audio.Play2D('11111111-1111-4111-8111-111111111111', {scope='session'})
            assert(voice == nil and #err > 0)
        )", "audio runtime", error)) << error;
        runtime.Shutdown();
    }
}

TEST_F(LuaAudioOfflineTests, VoiceControlsGcAndExpiredSubsystemAreSafe) {
    Lua(R"(
        local err
        voice, err = Audio.Play2D('game/tone.wav', {looping=true, scope='session'})
        assert(voice and err == '' and voice:IsValid())
        assert(voice:GetError() == '')
    )");
    // Playback starts asynchronously. Pump loading before asserting ready-voice
    // status; Pause on a pending request records intent while status stays Pending.
    std::vector<float> samples(256 * 2);
    std::string error;
    ASSERT_TRUE(audio->MixOffline(samples, error)) << error;
    Lua(R"(
        assert(voice:GetStatus() == 'Playing')
        assert(voice:Pause())
        assert(voice:GetStatus() == 'Paused')
        assert(voice:Resume())
        assert(voice:SetVolume(0.5))
        assert(voice:SetPitch(1.5))
        local ok, diagnostic = voice:Fade(0, 0/0, true)
        assert(not ok and #diagnostic > 0)
        assert(not voice:SetVolume(2))
        assert(not voice:SetPitch(0))
        assert(not voice:Fade(0, -1))
        assert(not voice:Fade(0, 1, 'yes'))
        assert(not voice:Stop(1))
        assert(voice:Fade(0, 0.01, true))
        local discarded = Audio.Play2D('game/tone.wav', {looping=true})
        assert(discarded)
        discarded = nil
        collectgarbage('collect')
    )");
    EXPECT_EQ(audio->GetVoiceCount(), 2u);
    samples.resize(4800 * 2);
    ASSERT_TRUE(audio->MixOffline(samples, error)) << error;
    Lua("assert(voice:GetStatus() == 'Stopped'); assert(not voice:Resume())");
    EXPECT_EQ(audio->GetVoiceCount(), 1u); // Unreferenced one-shot still plays.
    audio->Shutdown();
    ASSERT_TRUE(audio->Initialize());
    audio->BeginSession();
    Lua(R"(
        assert(not voice:IsValid() and voice:GetStatus() == 'Invalid')
        assert(#voice:GetError() > 0)
        local ok, err = voice:Stop()
        assert(not ok and #err > 0)
        voice = nil
        collectgarbage('collect')
    )");
}

TEST_F(LuaAudioOfflineTests, SourceControlsAndSceneResetRespectOwnership) {
    const auto entity = host.GetScene().CreateEntity("source");
    lua["e"] = LuaEntityHandle::FromScene(host.GetScene(), entity);
    Lua(R"(
        e:AddComponent('AudioSource', {clip='game/tone.wav', spatial='TwoD', looping=true})
        source, err = Audio.PlaySource(e)
        assert(source and err == '')
        assert(Audio.GetSourceVoice(e):IsValid())
    )");
    std::vector<float> samples(256 * 2);
    std::string error;
    ASSERT_TRUE(audio->MixOffline(samples, error)) << error;
    Lua(R"(
        assert(source:GetStatus() == 'Playing')
        assert(Audio.PauseSource(e) and source:GetStatus() == 'Paused')
        assert(Audio.ResumeSource(e))
        assert(Audio.StopSource(e) and source:GetStatus() == 'Stopped')
        source = Audio.PlaySource(e)
        assert(source)
        session = Audio.Play2D('game/tone.wav', {scope='session', looping=true})
        scene = Audio.Play2D('game/tone.wav', {looping=true})
        assert(session and scene)
    )");
    audio->ResetSimulation();
    Lua(R"(
        assert(source:GetStatus() ~= 'Playing' and source:GetStatus() ~= 'Pending')
        assert(scene:GetStatus() ~= 'Playing' and scene:GetStatus() ~= 'Pending')
        assert(session:IsValid())
        assert(session:GetStatus() == 'Playing' or session:GetStatus() == 'Pending')
    )");
    audio->EndSession();
    Lua("assert(session:GetStatus() ~= 'Playing' and session:GetStatus() ~= 'Pending')");
}
