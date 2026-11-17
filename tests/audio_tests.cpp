#include <engine/audio/AudioSubsystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace engine;
using DirectX::SimpleMath::Vector3;
constexpr unsigned SampleRate = 48000;
constexpr unsigned ClipFrames = 24000;
constexpr unsigned BlockFrames = 960;
constexpr entt::entity NullEntity = entt::null;

// Integer square waves have an integral period at the output rate. The stepped
// fixture makes two cursors observable without exposing backend cursor state.
std::string Wave(unsigned channels, bool stepped = false, unsigned rate = SampleRate) {
    std::string bytes;
    const auto u16 = [&](unsigned value) {
        bytes.push_back(static_cast<char>(value & 255));
        bytes.push_back(static_cast<char>((value >> 8) & 255));
    };
    const auto u32 = [&](unsigned value) { u16(value & 65535); u16(value >> 16); };
    const unsigned dataSize = ClipFrames * channels * 2;
    bytes += "RIFF"; u32(36 + dataSize); bytes += "WAVEfmt "; u32(16);
    u16(1); u16(channels); u32(rate); u32(rate * channels * 2);
    u16(channels * 2); u16(16); bytes += "data"; u32(dataSize);
    for (unsigned i = 0; i < ClipFrames; ++i) {
        const int amplitude = stepped ? (i < ClipFrames / 2 ? 2048 : 8192) : 4096;
        const auto sample = static_cast<std::uint16_t>(i % 96 < 48 ? amplitude : -amplitude);
        for (unsigned channel = 0; channel < channels; ++channel) u16(sample);
    }
    return bytes;
}

double Energy(const std::vector<float>& samples, unsigned channel) {
    double energy = 0;
    for (std::size_t i = channel; i < samples.size(); i += 2) {
        EXPECT_TRUE(std::isfinite(samples[i]));
        energy += static_cast<double>(samples[i]) * samples[i];
    }
    return samples.empty() ? 0 : energy / static_cast<double>(samples.size() / 2);
}
double Energy(const std::vector<float>& samples) { return Energy(samples, 0) + Energy(samples, 1); }

double WindowEnergy(const std::vector<float>& samples, unsigned firstFrame, unsigned frames) {
    double energy = 0;
    for (std::size_t i = static_cast<std::size_t>(firstFrame) * 2;
         i < static_cast<std::size_t>(firstFrame + frames) * 2; ++i) {
        EXPECT_TRUE(std::isfinite(samples[i]));
        energy += static_cast<double>(samples[i]) * samples[i];
    }
    return frames ? energy / frames : 0;
}

void ExpectSilentAfterControlBoundary(const std::vector<float>& samples) {
    // miniaudio's node processing caches may retain already-rendered input in a
    // downstream group when a sound stops/pauses. Allow only a bounded prefix,
    // not a looser whole-buffer energy tolerance that could hide ongoing output.
    // See miniaudio.h: ma_node_read_pcm_frames(), cachedFrameCountIn/consumedFrameCountIn.
    constexpr unsigned maxTailFrames = 32;
    const auto frames = static_cast<unsigned>(samples.size() / 2);
    ASSERT_GT(frames, maxTailFrames);
    (void)Energy(samples); // Check the allowed prefix for non-finite output too.
    EXPECT_LT(WindowEnergy(samples, maxTailFrames, frames - maxTailFrames), 1e-10);
}

struct AudioTests : testing::Test {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-audio-" + NewUuid());
    std::filesystem::path previousRoot = AssetPathResolver::GetGameRoot();
    Engine host{EngineConfig{.taskThreads = 1}};
    AudioSubsystem* audio = nullptr;
    AssetReference tone, cursor, stereo;
    std::string error;

    virtual void Configure() {}
    virtual AudioOutputMode Output() const { return AudioOutputMode::Offline; }
    AssetReference Asset(const char* name, const std::string& contents, CatalogAssetKind kind) {
        const AssetMetadata metadata{NewUuid(), kind};
        EXPECT_TRUE(CreateNewFile(root / name, contents, error)) << error;
        EXPECT_TRUE(WriteAssetMetadata(root / name, metadata, error)) << error;
        return {metadata.id, std::string("game/") + name};
    }
    void SetUp() override {
        std::filesystem::create_directories(root / "scenes");
        AssetPathResolver::SetGameRoot(root);
        tone = Asset("tone.wav", Wave(1), CatalogAssetKind::AudioClip);
        cursor = Asset("cursor.wav", Wave(1, true), CatalogAssetKind::AudioClip);
        stereo = Asset("stereo.wav", Wave(2), CatalogAssetKind::AudioClip);
        audio = host.AddSubsystem<AudioSubsystem>(AudioConfig{
            .output = Output(), .maxVoices = 4, .sampleRate = SampleRate});
        ASSERT_NE(audio, nullptr);
        Configure();
        ASSERT_TRUE(host.Initialize());
        ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
        ASSERT_TRUE(host.GetSceneAssets().RescanCatalog(error)) << error;
        ASSERT_NE(host.GetAssets().GetCatalog().Find(tone.id), nullptr);
        if (Output() == AudioOutputMode::Disabled) {
            ASSERT_FALSE(audio->IsAvailable());
            ASSERT_FALSE(audio->GetDiagnostic().empty());
        } else if (Output() == AudioOutputMode::Device && !audio->IsAvailable()) {
            GTEST_SKIP() << "Default audio output unavailable: " << audio->GetDiagnostic();
        } else ASSERT_TRUE(audio->IsAvailable()) << audio->GetDiagnostic();
        audio->BeginSession();
    }
    void TearDown() override {
        host.Shutdown();
        AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::vector<float> Mix(unsigned frames = BlockFrames) {
        std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
        EXPECT_TRUE(audio->MixOffline(samples, error)) << error;
        return samples;
    }
    void Reconcile() {
        FrameContext frame{}; // Spatial synchronization must also work at zero dt.
        audio->LateUpdate(frame);
    }
    AudioVoiceHandle Play(AudioPlayOptions options = {}) {
        auto voice = audio->Play2D(tone, options, error);
        EXPECT_TRUE(static_cast<bool>(voice)) << error;
        return voice;
    }
    entt::entity Source(bool autoplay = false, AudioLoadingMode loading = AudioLoadingMode::Buffered) {
        auto& scene = host.GetScene();
        const auto entity = scene.CreateEntity("source");
        AudioSource source;
        source.clip = tone;
        source.spatial = AudioSpatialMode::TwoD;
        source.playOnStart = autoplay;
        source.loading = loading;
        EXPECT_NE(scene.AddComponent<AudioSource>(entity, source), nullptr);
        return entity;
    }
    entt::entity Listener() {
        const auto entity = host.GetScene().CreateEntity("listener");
        EXPECT_NE(host.GetScene().AddComponent<AudioListener>(entity), nullptr);
        return entity;
    }
    void ExpectFadeInFromZero() {
        const auto beginning = Mix(480); // First 10 ms of a 100 ms fade.
        (void)Mix(1920);
        const double middle = Energy(Mix(480)); // 50-60 ms.
        (void)Mix(2400);
        const double full = Energy(Mix(480)); // After completion.
        ASSERT_GT(full, 1e-5);
        // Neither case has audible cached output: pending playback has never
        // mixed, and the paused case has already drained its bounded tail.
        EXPECT_LT(WindowEnergy(beginning, 0, 32), full * 0.002);
        const double early = Energy(beginning);
        EXPECT_GT(early, full * 0.0001); // It really ramps, rather than waiting.
        EXPECT_LT(early, full * 0.02);
        EXPECT_GT(middle, full * 0.1);
        EXPECT_LT(middle, full * 0.6);
    }
};

TEST_F(AudioTests, SharedClipOverlapsWithIndependentCursorsAndNaturalCompletion) {
    const auto first = audio->Play2D(cursor, {}, error);
    ASSERT_TRUE(static_cast<bool>(first)) << error;
    EXPECT_GT(Energy(Mix(14400)), 1e-5);
    const auto second = audio->Play2D(cursor, {}, error);
    ASSERT_TRUE(static_cast<bool>(second)) << error;
    const double overlap = Energy(Mix());
    EXPECT_EQ(audio->GetVoiceCount(), 2u);
    ASSERT_TRUE(audio->Pause(first));
    const double secondBeginning = Energy(Mix());
    EXPECT_GT(secondBeginning, 1e-5);
    // First cursor is in the loud half, while the new cursor is in the quiet half.
    EXPECT_GT(overlap, secondBeginning * 8);
    ASSERT_TRUE(audio->Resume(first));
    (void)Mix(11040);
    Reconcile();
    EXPECT_EQ(audio->GetStatus(first), AudioVoiceStatus::Finished);
    EXPECT_EQ(audio->GetStatus(second), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    (void)Mix(ClipFrames);
    Reconcile();
    EXPECT_EQ(audio->GetStatus(second), AudioVoiceStatus::Finished);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_LT(Energy(Mix()), 1e-10);
}

TEST_F(AudioTests, ExplicitPausePreservesCursorAndLoopingOutlivesClip) {
    const auto voice = Play();
    (void)Mix(2400);
    ASSERT_TRUE(audio->Pause(voice));
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Paused);
    ExpectSilentAfterControlBoundary(Mix(ClipFrames * 2));
    EXPECT_LT(Energy(Mix()), 1e-10);
    ASSERT_TRUE(audio->Resume(voice));
    EXPECT_GT(Energy(Mix(4800)), 1e-5);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing);
    (void)Mix(ClipFrames);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Finished);
    const auto loop = Play(AudioPlayOptions{.looping = true});
    EXPECT_GT(Energy(Mix(ClipFrames * 3)), 1e-5);
    EXPECT_EQ(audio->GetStatus(loop), AudioVoiceStatus::Playing);
    EXPECT_TRUE(audio->Stop(loop));
    EXPECT_EQ(audio->GetStatus(loop), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

TEST_F(AudioTests, SourcePlayRestartsPrimaryAndPitchChangesPlaybackDuration) {
    const auto entity = Source();
    const auto first = audio->PlaySource(entity, error);
    ASSERT_TRUE(static_cast<bool>(first)) << error;
    (void)Mix(14400);
    const auto restarted = audio->PlaySource(entity, error);
    ASSERT_TRUE(static_cast<bool>(restarted)) << error;
    EXPECT_EQ(audio->GetStatus(first), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    (void)Mix(14400);
    EXPECT_EQ(audio->GetStatus(restarted), AudioVoiceStatus::Playing);
    ASSERT_TRUE(audio->StopSource(entity));
    const auto fast = Play(AudioPlayOptions{.pitch = 2});
    EXPECT_GT(Energy(Mix(14400)), 1e-5);
    Reconcile();
    EXPECT_EQ(audio->GetStatus(fast), AudioVoiceStatus::Finished);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

TEST_F(AudioTests, GamePauseFreezesMusicAndSfxButNotUiOrExplicitPauseIntent) {
    const auto music = Play(AudioPlayOptions{.bus = AudioBus::Music});
    const auto sfx = Play();
    const auto explicitPause = Play();
    ASSERT_TRUE(audio->Pause(explicitPause));
    audio->SetGamePaused(true);
    const auto ui = Play(AudioPlayOptions{.bus = AudioBus::UI, .scope = AudioScope::Session});
    EXPECT_GT(Energy(Mix(ClipFrames + BlockFrames)), 1e-5);
    EXPECT_EQ(audio->GetStatus(ui), AudioVoiceStatus::Finished);
    EXPECT_EQ(audio->GetStatus(music), AudioVoiceStatus::Paused);
    EXPECT_EQ(audio->GetStatus(sfx), AudioVoiceStatus::Paused);
    EXPECT_LT(Energy(Mix()), 1e-10);
    // A request which becomes ready during pause must not leak its first buffer.
    const auto pending = Play();
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetStatus(pending), AudioVoiceStatus::Paused);
    audio->SetGamePaused(false);
    EXPECT_EQ(audio->GetStatus(explicitPause), AudioVoiceStatus::Paused);
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(music), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetStatus(sfx), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetStatus(pending), AudioVoiceStatus::Playing);
    EXPECT_TRUE(audio->Stop(music));
    EXPECT_TRUE(audio->Stop(sfx));
    EXPECT_TRUE(audio->Stop(pending));
    ExpectSilentAfterControlBoundary(Mix());
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_TRUE(audio->Resume(explicitPause));
    EXPECT_GT(Energy(Mix()), 1e-5);
}

TEST_F(AudioTests, GainCompositionMuteAndBusIsolationAreAudible) {
    const auto voice = Play(AudioPlayOptions{.looping = true});
    const double full = Energy(Mix());
    ASSERT_GT(full, 1e-5);
    ASSERT_TRUE(audio->SetVolume(voice, 0.5f));
    ASSERT_TRUE(audio->SetBusVolume(AudioBus::SFX, 0.5f));
    ASSERT_TRUE(audio->SetMasterVolume(0.5f));
    (void)Mix(); // Allow backend gain dezippering to settle.
    EXPECT_NEAR(Energy(Mix()) / full, 1.0 / 64, 0.003);
    EXPECT_FLOAT_EQ(audio->GetMasterVolume(), 0.5f);
    EXPECT_FLOAT_EQ(audio->GetBusVolume(AudioBus::SFX), 0.5f);
    ASSERT_TRUE(audio->SetBusVolume(AudioBus::SFX, 0));
    (void)Mix();
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing);
    const auto ui = Play(AudioPlayOptions{.bus = AudioBus::UI, .looping = true});
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_TRUE(audio->Stop(ui));
    EXPECT_TRUE(audio->SetBusVolume(AudioBus::SFX, 1));
    EXPECT_GT(Energy(Mix()), 1e-5);
}

TEST_F(AudioTests, InvalidFiniteSettingsAreRejectedAndCapacityCountsPendingAndPaused) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float value : {nan, inf, -1.0f, 1.1f}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(audio->SetMasterVolume(value));
        EXPECT_FALSE(audio->SetBusVolume(AudioBus::SFX, value));
        auto options = AudioPlayOptions{};
        options.volume = value;
        EXPECT_FALSE(static_cast<bool>(audio->Play2D(tone, options, error)));
        EXPECT_FALSE(error.empty());
    }
    for (const float value : {nan, inf, 0.0f, -1.0f}) {
        auto options = AudioPlayOptions{};
        options.pitch = value;
        EXPECT_FALSE(static_cast<bool>(audio->Play2D(tone, options, error)));
        EXPECT_FALSE(error.empty());
    }
    auto options = AudioPlayOptions{};
    options.minDistance = 5; options.maxDistance = 1;
    EXPECT_FALSE(static_cast<bool>(audio->PlayAt(tone, {}, options, error)));
    options = {};
    options.scope = AudioScope::Session;
    EXPECT_FALSE(static_cast<bool>(audio->PlayAt(tone, {}, options, error)));
    EXPECT_FALSE(static_cast<bool>(audio->PlayAt(tone, {nan, 0, 0}, {}, error)));
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    std::vector<AudioVoiceHandle> voices;
    for (int i = 0; i < 4; ++i) voices.push_back(Play());
    ASSERT_TRUE(audio->Pause(voices.front()));
    EXPECT_EQ(audio->GetVoiceCount(), 4u);
    EXPECT_FALSE(static_cast<bool>(audio->Play2D(tone, {}, error)));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(audio->SetVolume(voices.front(), nan));
    EXPECT_FALSE(audio->SetPitch(voices.front(), 0));
    EXPECT_FALSE(audio->Fade(voices.front(), 0, -1));
    EXPECT_FALSE(audio->Fade(voices.front(), 0, std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(audio->Stop(voices.front()));
    EXPECT_TRUE(static_cast<bool>(Play()));
    EXPECT_EQ(audio->GetVoiceCount(), 4u);
    // The released handle must never target its replacement.
    EXPECT_FALSE(audio->Stop(voices.front()));
    EXPECT_FALSE(audio->SetVolume(voices.front(), 0));
    EXPECT_EQ(audio->GetVoiceCount(), 4u);
}

TEST_F(AudioTests, FadesUsePlaybackTimeAndReclaimStoppedVoices) {
    const auto voice = Play(AudioPlayOptions{.looping = true});
    const double full = Energy(Mix());
    ASSERT_TRUE(audio->Fade(voice, 0, 0.02));
    (void)Mix(1920);
    EXPECT_LT(Energy(Mix()), full * 0.001);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing);
    ASSERT_TRUE(audio->SetVolume(voice, 1));
    ASSERT_TRUE(audio->Fade(voice, 0, 0.02, true));
    ASSERT_TRUE(audio->Pause(voice));
    EXPECT_LT(Energy(Mix(ClipFrames)), 1e-10);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Paused);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    ASSERT_TRUE(audio->Resume(voice));
    (void)Mix(1920);
    Reconcile();
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_LT(Energy(Mix()), 1e-10);
}

TEST_F(AudioTests, PendingMutedPlayImmediatelyFadesFromZeroInsteadOfBackendDefaultGain) {
    const auto voice = Play(AudioPlayOptions{.looping = true, .volume = 0});
    ASSERT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Pending);
    ASSERT_TRUE(audio->Fade(voice, 1, 0.1)); // Before the first resource-job pump.
    ExpectFadeInFromZero();
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
}

TEST_F(AudioTests, SetVolumeImmediatelyFollowedByFadeWhilePausedStartsFromZeroOnResume) {
    const auto voice = Play(AudioPlayOptions{.looping = true});
    ASSERT_GT(Energy(Mix()), 1e-5);
    ASSERT_TRUE(audio->Pause(voice));
    ASSERT_TRUE(audio->SetVolume(voice, 0));
    ASSERT_TRUE(audio->Fade(voice, 1, 0.1)); // No intervening audio processing.
    ExpectSilentAfterControlBoundary(Mix(ClipFrames));
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Paused);
    ASSERT_TRUE(audio->Resume(voice));
    ExpectFadeInFromZero();
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing);
}

TEST_F(AudioTests, UnicodeFilenameStreamLoopsThroughSeekAndRestartsFromBeginning) {
    // Construct the Windows filename from UTF-8 explicitly, never the ANSI code
    // page. Multiple loop boundaries exercise the streaming decoder's seek path.
    const std::string name = "\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA-\xE9\x9F\xB3.wav";
    const auto file = root / FromUtf8(name);
    const AssetMetadata metadata{NewUuid(), CatalogAssetKind::AudioClip};
    ASSERT_TRUE(CreateNewFile(file, Wave(1, true), error)) << error;
    ASSERT_TRUE(WriteAssetMetadata(file, metadata, error)) << error;
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    ASSERT_TRUE(host.GetSceneAssets().RescanCatalog(error)) << error;
    const AssetReference clip{metadata.id, "game/" + name};
    const auto options = AudioPlayOptions{.loading = AudioLoadingMode::Streamed, .looping = true};
    const auto voice = audio->Play2D(clip, options, error);
    ASSERT_TRUE(static_cast<bool>(voice)) << error;
    for (int loop = 0; loop < 3; ++loop) {
        SCOPED_TRACE(loop);
        const auto quiet = Mix(ClipFrames / 2);
        const auto loud = Mix(ClipFrames / 2);
        // Ignore boundary cache/decoder latency, and inspect the middle of each
        // half instead of accepting a loop that goes silent or repeats one page.
        const double quietEnergy = WindowEnergy(quiet, 2400, 4800);
        const double loudEnergy = WindowEnergy(loud, 2400, 4800);
        ASSERT_GT(quietEnergy, 1e-5) << audio->GetVoiceError(voice);
        EXPECT_NEAR(loudEnergy / quietEnergy, 16, 1);
        EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing);
    }
    ASSERT_TRUE(audio->Stop(voice));
    ExpectSilentAfterControlBoundary(Mix());
    const auto restart = audio->Play2D(clip, options, error);
    ASSERT_TRUE(static_cast<bool>(restart)) << error;
    const auto beginning = Mix(BlockFrames);
    EXPECT_GT(Energy(beginning), 1e-5);
    EXPECT_LT(Energy(beginning), 0.02); // Quiet half, not the old loud cursor.
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetStatus(restart), AudioVoiceStatus::Playing);
}

TEST_F(AudioTests, ShutdownInvalidatesHandlesAcrossReinitialization) {
    const auto old = Play(AudioPlayOptions{.loading = AudioLoadingMode::Streamed, .looping = true});
    // Shutdown with a resource request outstanding, then again while initialized
    // streaming playback is active. Both must drain jobs before releasing storage.
    audio->Shutdown();
    audio->Shutdown();
    ASSERT_TRUE(audio->Initialize());
    audio->BeginSession();
    const auto fresh = Play(AudioPlayOptions{.loading = AudioLoadingMode::Streamed, .looping = true});
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(old), AudioVoiceStatus::Invalid);
    EXPECT_FALSE(audio->GetVoiceError(old).empty());
    EXPECT_FALSE(audio->Stop(old));
    EXPECT_FALSE(audio->Pause(old));
    EXPECT_FALSE(audio->Resume(old));
    EXPECT_FALSE(audio->SetVolume(old, 0));
    EXPECT_FALSE(audio->SetPitch(old, 2));
    EXPECT_FALSE(audio->Fade(old, 0, 0.01, true));
    EXPECT_EQ(audio->GetStatus(fresh), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    audio->Shutdown();
    ASSERT_TRUE(audio->Initialize());
    audio->BeginSession();
    EXPECT_LT(Energy(Mix()), 1e-10); // Pump the new, empty resource manager as well.
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_EQ(audio->GetStatus(fresh), AudioVoiceStatus::Invalid);
}

TEST_F(AudioTests, EditorPreviewIgnoresGamePauseAndObeysEditAndLifecycleBoundaries) {
    audio->EndSession();
    host.SetEditorMode();
    const auto entity = Source(true);
    Reconcile();
    EXPECT_FALSE(static_cast<bool>(audio->GetSourceVoice(entity)));
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_LT(Energy(Mix()), 1e-10);

    audio->SetGamePaused(true);
    const auto first = audio->Preview(tone, error);
    ASSERT_TRUE(static_cast<bool>(first)) << error;
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(first), AudioVoiceStatus::Playing);
    const auto replacement = audio->Preview(cursor, error);
    ASSERT_TRUE(static_cast<bool>(replacement)) << error;
    EXPECT_EQ(audio->GetStatus(first), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(replacement), AudioVoiceStatus::Playing);
    audio->StopPreview();
    EXPECT_EQ(audio->GetStatus(replacement), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    ExpectSilentAfterControlBoundary(Mix());

    const auto enteringPlay = audio->Preview(tone, error);
    ASSERT_TRUE(static_cast<bool>(enteringPlay)) << error;
    audio->BeginSession(); // Cancel even a preview whose load has not been pumped.
    EXPECT_EQ(audio->GetStatus(enteringPlay), AudioVoiceStatus::Stopped);
    EXPECT_FALSE(static_cast<bool>(audio->Preview(tone, error)));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_LT(Energy(Mix()), 1e-10);

    audio->EndSession();
    const auto transitioning = audio->Preview(tone, error);
    ASSERT_TRUE(static_cast<bool>(transitioning)) << error;
    EXPECT_GT(Energy(Mix()), 1e-5);
    audio->BeginSceneTransition();
    EXPECT_EQ(audio->GetStatus(transitioning), AudioVoiceStatus::Stopped);
    EXPECT_FALSE(static_cast<bool>(audio->Preview(tone, error)));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    ExpectSilentAfterControlBoundary(Mix());
    audio->EndSceneTransition();
    const auto afterTransition = audio->Preview(tone, error);
    ASSERT_TRUE(static_cast<bool>(afterTransition)) << error;
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(afterTransition), AudioVoiceStatus::Playing);
}

struct AudioDeviceTests : AudioTests {
    AudioOutputMode Output() const override { return AudioOutputMode::Device; }
    void Configure() override {
        // Apply before Initialize starts the default output device, and retain
        // a per-voice zero gain across every shutdown/retry as well.
        ASSERT_TRUE(audio->SetMasterVolume(0));
    }
    void WaitUntilReady(const AudioVoiceHandle& voice) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (audio->GetStatus(voice) == AudioVoiceStatus::Pending &&
               std::chrono::steady_clock::now() < deadline) {
            Reconcile(); // Poll completion; Device mode uses a real resource worker.
            std::this_thread::yield();
        }
        ASSERT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Playing)
            << audio->GetVoiceError(voice) << "; " << audio->GetDiagnostic();
    }
};

TEST_F(AudioDeviceTests, WorkerStreamCancellationShutdownReinitializeAndRetryInvalidateOldHandles) {
    const AudioPlayOptions options{
        .loading = AudioLoadingMode::Streamed, .looping = true, .volume = 0};
    const auto cancelled = Play(options);
    ASSERT_EQ(audio->GetStatus(cancelled), AudioVoiceStatus::Pending);
    ASSERT_TRUE(audio->Stop(cancelled)); // No main-thread completion poll yet.
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    const auto survivor = Play(options);
    ASSERT_NO_FATAL_FAILURE(WaitUntilReady(survivor));
    EXPECT_EQ(audio->GetStatus(cancelled), AudioVoiceStatus::Stopped);
    EXPECT_FALSE(audio->Resume(cancelled));
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    ASSERT_TRUE(audio->Stop(survivor));

    const auto pendingShutdown = Play(options);
    ASSERT_EQ(audio->GetStatus(pendingShutdown), AudioVoiceStatus::Pending);
    audio->Shutdown(); // Joins outstanding stream jobs without a completion poll.
    audio->Shutdown();
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_EQ(audio->GetStatus(pendingShutdown), AudioVoiceStatus::Invalid);
    ASSERT_TRUE(audio->Initialize());
    if (!audio->IsAvailable())
        GTEST_SKIP() << "Default audio output unavailable after reinitialize: " << audio->GetDiagnostic();
    ASSERT_TRUE(audio->SetMasterVolume(0));
    audio->BeginSession();
    const auto reinitialized = Play(options);
    ASSERT_NO_FATAL_FAILURE(WaitUntilReady(reinitialized));
    EXPECT_FALSE(audio->Stop(pendingShutdown));
    EXPECT_FALSE(audio->SetVolume(pendingShutdown, 0));
    EXPECT_EQ(audio->GetStatus(reinitialized), AudioVoiceStatus::Playing);

    const auto pendingRetry = Play(options);
    ASSERT_EQ(audio->GetStatus(pendingRetry), AudioVoiceStatus::Pending);
    const bool retried = audio->RetryDevice(error);
    EXPECT_EQ(audio->GetStatus(reinitialized), AudioVoiceStatus::Invalid);
    EXPECT_EQ(audio->GetStatus(pendingRetry), AudioVoiceStatus::Invalid);
    EXPECT_FALSE(audio->GetVoiceError(pendingRetry).empty());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    if (!retried) {
        EXPECT_FALSE(error.empty());
        GTEST_SKIP() << "Default audio output unavailable after retry: " << error;
    }
    ASSERT_TRUE(audio->SetMasterVolume(0));
    const auto fresh = Play(options); // Retry must preserve the active session.
    ASSERT_NO_FATAL_FAILURE(WaitUntilReady(fresh));
    EXPECT_FALSE(audio->Stop(pendingRetry));
    EXPECT_FALSE(audio->Pause(reinitialized));
    EXPECT_FALSE(audio->Resume(pendingRetry));
    EXPECT_FALSE(audio->SetPitch(pendingRetry, 2));
    EXPECT_FALSE(audio->Fade(pendingRetry, 0, 0.01, true));
    EXPECT_EQ(audio->GetStatus(fresh), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    ASSERT_TRUE(audio->Stop(fresh));
    Reconcile();
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

struct AudioLoadingTests : AudioTests, testing::WithParamInterface<AudioLoadingMode> {};

TEST_P(AudioLoadingTests, GainOnlyGroupsDoNotRetainSamplesAfterLastChildStops) {
    for (const auto bus : {AudioBus::Music, AudioBus::SFX, AudioBus::UI}) {
        SCOPED_TRACE(static_cast<int>(bus));
        const auto voice = audio->Play2D(tone, AudioPlayOptions{.loading = GetParam(), .bus = bus, .looping = true}, error);
        ASSERT_TRUE(static_cast<bool>(voice)) << error;
        ASSERT_GT(Energy(Mix()), 1e-5);
        ASSERT_TRUE(audio->Stop(voice));
        EXPECT_EQ(audio->GetVoiceCount(), 0u);
        // Unity-pitch resamplers on bus/master nodes used to emit old samples here.
        // Detaching the last child of a gain-only graph must silence the whole next block.
        EXPECT_LT(Energy(Mix()), 1e-10);
    }
}

TEST_F(AudioTests, MetadataPreservesNativeFormatAndOutputOnFailureWithoutDevice) {
    AudioClipInfo info;
    ASSERT_TRUE(InspectAudioClip(root / "stereo.wav", info, error)) << error;
    EXPECT_EQ(info.channels, 2u);
    EXPECT_EQ(info.sampleRate, SampleRate);
    EXPECT_EQ(info.frameCount, ClipFrames);
    EXPECT_EQ(info.fileBytes, Wave(2).size());
    EXPECT_DOUBLE_EQ(info.durationSeconds, 0.5);
    const auto before = info;
    EXPECT_FALSE(InspectAudioClip(root / "missing.wav", info, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(info.channels, before.channels);
    EXPECT_EQ(info.frameCount, before.frameCount);
    EXPECT_EQ(info.fileBytes, before.fileBytes);
    EXPECT_EQ(info.sampleRate, before.sampleRate);
    EXPECT_DOUBLE_EQ(info.durationSeconds, before.durationSeconds);
    const auto unicode = root / FromUtf8("\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA-\xE9\x9F\xB3.wav");
    ASSERT_TRUE(CreateNewFile(unicode, Wave(1), error)) << error;
    audio->Shutdown(); // Probe is independent of subsystem/device lifetime.
    ASSERT_TRUE(InspectAudioClip(unicode, info, error)) << error;
    EXPECT_EQ(info.channels, 1u);
    EXPECT_EQ(info.frameCount, ClipFrames);
    std::vector<AudioClipInfo> concurrentInfo(4);
    std::vector<std::string> concurrentErrors(4);
    std::vector<std::thread> probes;
    for (std::size_t i = 0; i < concurrentInfo.size(); ++i) probes.emplace_back([&, i] {
        (void)InspectAudioClip(i % 2 ? unicode : root / "stereo.wav", concurrentInfo[i], concurrentErrors[i]);
    });
    for (auto& probe : probes) probe.join();
    for (std::size_t i = 0; i < concurrentInfo.size(); ++i) {
        EXPECT_TRUE(concurrentErrors[i].empty()) << concurrentErrors[i];
        EXPECT_EQ(concurrentInfo[i].channels, i % 2 ? 1u : 2u);
        EXPECT_EQ(concurrentInfo[i].frameCount, ClipFrames);
    }
    ASSERT_TRUE(CreateNewFile(root / "invalid.wav", "not audio", error)) << error;
    EXPECT_FALSE(InspectAudioClip(root / "invalid.wav", info, error));
    EXPECT_EQ(info.channels, 1u);
    EXPECT_EQ(info.frameCount, ClipFrames);
    EXPECT_FALSE(error.empty());
}

TEST_F(AudioTests, AuditionGainIsIndependentOfEveryRuntimeGainAndPersistsAcrossRetry) {
    audio->EndSession(); host.SetEditorMode();
    ASSERT_TRUE(audio->SetMasterVolume(0));
    for (auto bus : {AudioBus::Music, AudioBus::SFX, AudioBus::UI}) ASSERT_TRUE(audio->SetBusVolume(bus, 0));
    const auto first = audio->Preview(tone, AudioPreviewOptions{.looping = true}, error);
    ASSERT_TRUE(static_cast<bool>(first)) << error;
    const auto full = Energy(Mix());
    ASSERT_GT(full, 1e-5);
    ASSERT_TRUE(audio->SetVolume(first, 0.5f)); (void)Mix();
    EXPECT_NEAR(Energy(Mix()) / full, 0.25, 0.01);
    ASSERT_TRUE(audio->RetryDevice(error)) << error;
    EXPECT_EQ(audio->GetStatus(first), AudioVoiceStatus::Invalid);
    const auto second = audio->Preview(tone, AudioPreviewOptions{.loading = AudioLoadingMode::Streamed, .looping = true, .volume = 0.5f}, error);
    ASSERT_TRUE(static_cast<bool>(second)) << error;
    EXPECT_NEAR(Energy(Mix()) / full, 0.25, 0.01);
    EXPECT_FLOAT_EQ(audio->GetMasterVolume(), 0);
    EXPECT_FLOAT_EQ(audio->GetBusVolume(AudioBus::UI), 0);
}

TEST_P(AudioLoadingTests, SeekUsesSourceSecondsRejectsPendingAndPreservesPause) {
    const auto voice = audio->Play2D(cursor, AudioPlayOptions{.loading = GetParam(), .looping = true}, error);
    ASSERT_TRUE(static_cast<bool>(voice)) << error;
    EXPECT_FALSE(audio->GetPlaybackPosition(voice));
    EXPECT_FALSE(audio->Seek(voice, 0.3, error));
    EXPECT_FALSE(error.empty());
    (void)Mix();
    ASSERT_TRUE(audio->Pause(voice));
    ExpectSilentAfterControlBoundary(Mix());
    ASSERT_TRUE(audio->Seek(voice, 0.3, error)) << error;
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Paused);
    (void)Mix(); // Stream seek is serviced by the resource worker, not by unpausing.
    auto position = audio->GetPlaybackPosition(voice);
    ASSERT_TRUE(position);
    EXPECT_NEAR(position->durationSeconds, 0.5, 1.0 / SampleRate);
    EXPECT_NEAR(position->timeSeconds, 0.3, 1.0 / SampleRate);
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_NEAR(audio->GetPlaybackPosition(voice)->timeSeconds, 0.3, 1.0 / SampleRate);
    for (const double value : {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(audio->Seek(voice, value, error)); EXPECT_FALSE(error.empty());
    }
    ASSERT_TRUE(audio->Resume(voice));
    const auto loud = Energy(Mix());
    ASSERT_TRUE(audio->Seek(voice, 0.05, error));
    (void)Mix();
    const auto quiet = Energy(Mix());
    ASSERT_GT(quiet, 1e-5);
    EXPECT_GT(loud, quiet * 8);
    ASSERT_TRUE(audio->Pause(voice));
    ASSERT_TRUE(audio->Seek(voice, 100, error)); // Looping clamp maps duration to the loop start.
    (void)Mix();
    ASSERT_TRUE(audio->GetPlaybackPosition(voice));
    EXPECT_NEAR(audio->GetPlaybackPosition(voice)->timeSeconds, 0, 1.0 / SampleRate);
    ASSERT_TRUE(audio->SetLooping(voice, false));
    ASSERT_TRUE(audio->Seek(voice, 0.49, error));
    ASSERT_TRUE(audio->Resume(voice)); (void)Mix(2400);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Finished);
    EXPECT_FALSE(audio->GetPlaybackPosition(voice));
    EXPECT_FALSE(audio->Seek(voice, 0, error));
    EXPECT_FALSE(audio->SetLooping(voice, true));
    EXPECT_FALSE(audio->Seek({}, 0, error));
}

TEST_P(AudioLoadingTests, TransportUsesNativeRateRatherThanMixerRateOrPitch) {
    const auto clip = Asset("native-rate.wav", Wave(1, true, 24000), CatalogAssetKind::AudioClip);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    ASSERT_TRUE(host.GetSceneAssets().RescanCatalog(error)) << error;
    AudioClipInfo info;
    ASSERT_TRUE(InspectAudioClip(root / "native-rate.wav", info, error)) << error;
    EXPECT_EQ(info.sampleRate, 24000u);
    EXPECT_EQ(info.channels, 1u);
    EXPECT_DOUBLE_EQ(info.durationSeconds, 1);
    const auto voice = audio->Play2D(clip, AudioPlayOptions{.loading = GetParam(), .looping = true, .pitch = 2}, error);
    ASSERT_TRUE(static_cast<bool>(voice)) << error;
    (void)Mix();
    ASSERT_TRUE(audio->Pause(voice));
    ASSERT_TRUE(audio->Seek(voice, 0.75, error)) << error;
    (void)Mix();
    const auto position = audio->GetPlaybackPosition(voice);
    ASSERT_TRUE(position);
    EXPECT_NEAR(position->timeSeconds, 0.75, 1.0 / 24000);
    EXPECT_NEAR(position->durationSeconds, 1, 1.0 / 24000);
    EXPECT_EQ(audio->GetStatus(voice), AudioVoiceStatus::Paused);
}

TEST_P(AudioLoadingTests, CorruptSupportedClipFailsExplicitlyAndReclaimsCapacityWithoutPathFallback) {
    auto truncated = Wave(1);
    truncated.resize(32); // Valid RIFF/WAVE signature, incomplete fmt chunk and no PCM data.
    const auto corrupt = Asset("corrupt.wav", truncated, CatalogAssetKind::AudioClip);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    ASSERT_TRUE(host.GetSceneAssets().RescanCatalog(error)) << error;
    ASSERT_NE(host.GetSceneAssets().GetCatalog().Find(corrupt.id), nullptr);
    const AudioPlayOptions options{.loading = GetParam(), .looping = true};
    // Leave exactly one capacity slot for a decoder request, so successful
    // replacement below proves a failed load released its reservation.
    std::vector<AudioVoiceHandle> occupied;
    for (int i = 0; i < 3; ++i) occupied.push_back(Play(options));
    ASSERT_GT(Energy(Mix()), 1e-5);
    const auto failed = audio->Play2D(corrupt, options, error);
    const auto immediateError = error;
    (void)Mix();
    Reconcile();
    if (failed) {
        EXPECT_EQ(audio->GetStatus(failed), AudioVoiceStatus::Failed);
        EXPECT_FALSE(audio->GetVoiceError(failed).empty());
        EXPECT_FALSE(audio->Resume(failed));
    } else EXPECT_FALSE(immediateError.empty());
    EXPECT_EQ(audio->GetVoiceCount(), 3u);

    const AssetReference missing{NewUuid(), tone.path}; // Existing valid path is only a hint.
    EXPECT_FALSE(static_cast<bool>(audio->Play2D(missing, options, error)));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(audio->GetVoiceCount(), 3u);
    const auto replacement = Play(options);
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(replacement), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 4u);
    EXPECT_FALSE(static_cast<bool>(audio->Play2D(tone, options, error)));
    for (const auto& voice : occupied) EXPECT_TRUE(audio->Stop(voice));
    EXPECT_TRUE(audio->Stop(replacement));
    ExpectSilentAfterControlBoundary(Mix());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

TEST_P(AudioLoadingTests, EarlyStopResetAndTransitionsCannotResurrectRequests) {
    const auto options = AudioPlayOptions{.loading = GetParam(), .looping = true};
    const auto cancelled = Play(options);
    ASSERT_TRUE(audio->Stop(cancelled)); // Before the first deterministic job pump.
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetStatus(cancelled), AudioVoiceStatus::Stopped);
    auto sessionOptions = options;
    sessionOptions.scope = AudioScope::Session;
    const auto session = Play(sessionOptions);
    const auto scene = Play(options); // Shares the clip, but not its lifetime lease.
    audio->ResetSimulation();
    EXPECT_EQ(audio->GetStatus(scene), AudioVoiceStatus::Stopped);
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(session), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    audio->BeginSceneTransition();
    EXPECT_FALSE(static_cast<bool>(audio->Play2D(tone, options, error)));
    EXPECT_FALSE(error.empty());
    audio->EndSceneTransition();
    const auto replacement = Play(options);
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(replacement), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetStatus(cancelled), AudioVoiceStatus::Stopped);
    audio->EndSession();
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_EQ(audio->GetStatus(session), AudioVoiceStatus::Stopped);
    audio->BeginSession();
    const auto pendingAtEnd = Play(options);
    audio->EndSession();
    audio->BeginSession();
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetStatus(pendingAtEnd), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

TEST_P(AudioLoadingTests, ComponentActivationConsumesAutoplayAndClipChangesNeedNewIntent) {
    const auto entity = Source(true, GetParam());
    Reconcile();
    const auto first = audio->GetSourceVoice(entity);
    ASSERT_TRUE(static_cast<bool>(first));
    ASSERT_TRUE(audio->StopSource(entity)); // Consumes even an unready autoplay.
    (void)Mix(); Reconcile();
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_LT(Energy(Mix()), 1e-10);
    auto* source = host.GetScene().TryGetComponent<AudioSource>(entity);
    ASSERT_NE(source, nullptr);
    source->enabled = false; Reconcile();
    source->enabled = true; Reconcile();
    const auto activated = audio->GetSourceVoice(entity);
    ASSERT_TRUE(static_cast<bool>(activated));
    EXPECT_GT(Energy(Mix()), 1e-5);
    (void)Mix(ClipFrames + BlockFrames); Reconcile();
    EXPECT_EQ(audio->GetStatus(activated), AudioVoiceStatus::Finished);
    Reconcile(); (void)Mix(); Reconcile();
    EXPECT_EQ(audio->GetVoiceCount(), 0u); // Completion is not a new activation.
    const auto explicitVoice = audio->PlaySource(entity, error);
    ASSERT_TRUE(static_cast<bool>(explicitVoice)) << error;
    source->clip = cursor;
    Reconcile();
    EXPECT_EQ(audio->GetStatus(explicitVoice), AudioVoiceStatus::Stopped);
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    const auto replacement = audio->PlaySource(entity, error);
    ASSERT_TRUE(static_cast<bool>(replacement)) << error;
    EXPECT_GT(Energy(Mix()), 1e-5);
    source->enabled = false; Reconcile();
    EXPECT_EQ(audio->GetStatus(replacement), AudioVoiceStatus::Stopped);
    EXPECT_LT(Energy(Mix()), 1e-10);
}

TEST_P(AudioLoadingTests, ComponentRemovalAndEntityDeletionCancelPendingFollowingVoices) {
    auto& scene = host.GetScene();
    const auto entity = Source(false, GetParam());
    const auto pending = audio->PlaySource(entity, error);
    ASSERT_TRUE(static_cast<bool>(pending)) << error;
    ASSERT_TRUE(scene.RemoveComponent<AudioSource>(entity));
    Reconcile();
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetStatus(pending), AudioVoiceStatus::Stopped);
    const auto other = Source(false, GetParam());
    const auto following = audio->PlaySource(other, error);
    ASSERT_TRUE(static_cast<bool>(following)) << error;
    Listener();
    const auto fixed = audio->PlayAt(tone, {1, 0, -1}, AudioPlayOptions{.loading = GetParam()}, error);
    ASSERT_TRUE(static_cast<bool>(fixed)) << error;
    scene.DestroyEntity(other);
    Reconcile();
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(following), AudioVoiceStatus::Stopped);
    EXPECT_EQ(audio->GetStatus(fixed), AudioVoiceStatus::Playing);
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
}

INSTANTIATE_TEST_SUITE_P(BufferedAndStreamed, AudioLoadingTests,
    testing::Values(AudioLoadingMode::Buffered, AudioLoadingMode::Streamed));

TEST_F(AudioTests, ParentedMonoSourcePansFromFirstOutputAndAttenuatesInWorldSpace) {
    auto& scene = host.GetScene();
    const auto listenerParent = scene.CreateEntity("listener parent");
    scene.GetTransform(listenerParent).position = {10, 0, 0};
    const auto listener = Listener();
    ASSERT_TRUE(scene.SetParent(listener, listenerParent));
    const auto sourceParent = scene.CreateEntity("source parent");
    scene.GetTransform(sourceParent).position = {14, 0, 0};
    const auto entity = Source();
    ASSERT_TRUE(scene.SetParent(entity, sourceParent));
    auto* source = scene.TryGetComponent<AudioSource>(entity);
    ASSERT_NE(source, nullptr);
    source->spatial = AudioSpatialMode::ThreeD;
    source->looping = true;
    // No LateUpdate before Play: initial world poses must precede the first sample.
    const auto voice = audio->PlaySource(entity, error);
    ASSERT_TRUE(static_cast<bool>(voice)) << error;
    const auto right = Mix();
    EXPECT_GT(Energy(right, 1), 1e-6);
    EXPECT_GT(Energy(right, 1), Energy(right, 0) * 4);
    EXPECT_EQ(audio->GetEffectiveListener(), listener);
    scene.GetTransform(sourceParent).position.x = 6;
    Reconcile(); (void)Mix();
    const auto left = Mix();
    EXPECT_GT(Energy(left, 0), Energy(left, 1) * 4);
    EXPECT_NEAR(Energy(left) / Energy(right), 1, 0.15);
    scene.GetTransform(sourceParent).position.x = -6; // 16 metres, versus 4.
    Reconcile(); (void)Mix();
    const auto distant = Mix();
    EXPECT_GT(Energy(distant), 1e-8);
    EXPECT_LT(Energy(distant), Energy(left) * 0.2);
    // Translating both parents together restores the same relative arrangement.
    scene.GetTransform(listenerParent).position.x = -2;
    Reconcile(); (void)Mix();
    EXPECT_NEAR(Energy(Mix()) / Energy(left), 1, 0.15);
    scene.GetTransform(listenerParent).rotation =
        DirectX::SimpleMath::Quaternion::CreateFromAxisAngle(Vector3::UnitY, 3.14159265f);
    // Nonuniform scale must not contaminate the listener's orientation basis.
    scene.GetTransform(listenerParent).scale = {2, 3, 4};
    Reconcile(); (void)Mix();
    const auto turned = Mix();
    EXPECT_GT(Energy(turned, 1), Energy(turned, 0) * 4);
    EXPECT_NEAR(Energy(turned) / Energy(left), 1, 0.15);
}

TEST_F(AudioTests, ConflictingListenersGateTheVeryFirstBufferWithoutLateUpdate) {
    Listener();
    Listener();
    const auto spatial = audio->PlayAt(tone, {4, 0, 0}, {}, error);
    ASSERT_TRUE(static_cast<bool>(spatial)) << error;
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetEffectiveListener(), NullEntity);
    EXPECT_FALSE(audio->GetListenerDiagnostic().empty());
    const auto flat = Play();
    EXPECT_GT(Energy(Mix()), 1e-5);
    EXPECT_EQ(audio->GetStatus(flat), AudioVoiceStatus::Playing);
}

TEST_F(AudioTests, MissingAndConflictingListenersSuppressFirstSpatialOutputButNot2D) {
    auto& scene = host.GetScene();
    const auto spatial = audio->PlayAt(tone, {4, 0, 0}, AudioPlayOptions{.looping = true}, error);
    ASSERT_TRUE(static_cast<bool>(spatial)) << error;
    EXPECT_LT(Energy(Mix()), 1e-10);
    EXPECT_EQ(audio->GetEffectiveListener(), NullEntity);
    EXPECT_FALSE(audio->GetListenerDiagnostic().empty());
    const auto twoD = Play(AudioPlayOptions{.looping = true});
    EXPECT_GT(Energy(Mix()), 1e-5);
    ASSERT_TRUE(audio->Stop(twoD));
    const auto camera = scene.CreateEntity("fallback camera");
    ASSERT_NE(scene.AddComponent<Camera>(camera), nullptr);
    scene.SetActiveCamera(camera);
    Reconcile();
    EXPECT_EQ(audio->GetEffectiveListener(), camera);
    EXPECT_GT(Energy(Mix()), 1e-6);
    const auto explicitListener = Listener();
    Reconcile();
    EXPECT_EQ(audio->GetEffectiveListener(), explicitListener);
    const auto conflict = Listener();
    Reconcile();
    EXPECT_EQ(audio->GetEffectiveListener(), NullEntity);
    EXPECT_FALSE(audio->GetListenerDiagnostic().empty());
    (void)Mix();
    EXPECT_LT(Energy(Mix()), 1e-10);
    ASSERT_TRUE(audio->Stop(spatial));
    const auto newSpatial = audio->PlayAt(tone, {4, 0, 0}, {}, error);
    ASSERT_TRUE(static_cast<bool>(newSpatial)) << error;
    EXPECT_LT(Energy(Mix()), 1e-10); // Conflict also gates a newly started voice.
    const auto ui = Play(AudioPlayOptions{.bus = AudioBus::UI});
    EXPECT_GT(Energy(Mix()), 1e-5);
    ASSERT_TRUE(audio->Stop(ui));
    scene.TryGetComponent<AudioListener>(conflict)->enabled = false;
    Reconcile();
    EXPECT_EQ(audio->GetEffectiveListener(), explicitListener);
    EXPECT_GT(Energy(Mix()), 1e-6);
}

TEST_F(AudioTests, StereoIsSupportedIn2DButRejectedForPointEmitters) {
    Listener();
    const auto point = audio->PlayAt(stereo, {1, 0, -1}, {}, error);
    const auto rejectionError = error;
    // Format inspection may complete asynchronously. Either immediate rejection
    // or a terminal failed voice is valid, but it must never produce output.
    EXPECT_LT(Energy(Mix()), 1e-10);
    Reconcile();
    if (point) {
        EXPECT_EQ(audio->GetStatus(point), AudioVoiceStatus::Failed);
        EXPECT_FALSE(audio->GetVoiceError(point).empty());
    } else EXPECT_FALSE(rejectionError.empty());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    const auto flat = audio->Play2D(stereo, {}, error);
    ASSERT_TRUE(static_cast<bool>(flat)) << error;
    const auto samples = Mix();
    EXPECT_GT(Energy(samples, 0), 1e-5);
    EXPECT_NEAR(Energy(samples, 0), Energy(samples, 1), 1e-5);
}

struct AudioSessionTests : AudioTests {
    ScriptSubsystem* scripts = nullptr;
    void Configure() override { scripts = host.AddSubsystem<ScriptSubsystem>(); }
    std::filesystem::path ScriptedScene(const char* name, const AssetReference& script) {
        SceneDocument document;
        SceneEntityDefinition entity;
        entity.id = NewUuid(); entity.name = name;
        entity.script = ScriptComponent{script, true};
        document.entities.push_back(entity);
        std::string text;
        EXPECT_TRUE(SerializeSceneDocument(document, text, error)) << error;
        const std::string path = std::string("scenes/") + name + ".scene.json";
        (void)Asset(path.c_str(), text, CatalogAssetKind::Scene);
        return root / path;
    }
    AssetReference LifecycleScript() {
        return Asset("lifecycle.lua", R"(
            function OnStart()
                local voice, err = Audio.Play2D('game/tone.wav', {looping=true})
                assert(voice, err)
            end
            function OnDestroy()
                -- An old-scene/session teardown must not recreate either scope.
                Audio.Play2D('game/tone.wav', {looping=true})
                Audio.Play2D('game/tone.wav', {looping=true, scope='session'})
            end
        )", CatalogAssetKind::Script);
    }
    void Rescan() {
        ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
        ASSERT_TRUE(host.GetSceneAssets().RescanCatalog(error)) << error;
    }
};

TEST_F(AudioSessionTests, StandaloneStartupSurvivesPostCallbackResetAndStopRejectsTeardown) {
    audio->EndSession();
    const auto script = LifecycleScript();
    const auto file = ScriptedScene("Start", script);
    Rescan();
    ASSERT_TRUE(host.LoadScene(file, error)) << error;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(host.StartGame(root, {}, error)) << error;
        EXPECT_GT(Energy(Mix()), 1e-5);
        EXPECT_EQ(audio->GetVoiceCount(), 1u);
        host.StopGame(); host.StopGame();
        Reconcile();
        EXPECT_EQ(audio->GetVoiceCount(), 0u);
        EXPECT_LT(Energy(Mix()), 1e-10);
    }
}

TEST_F(AudioSessionTests, LoadGameReplacesAudioSessionAndStartsFreshAutoloadMusicExactlyOnce) {
    audio->EndSession();
    const auto music = Asset("music.lua", R"(
        starts = 0
        function OnStart()
            assert(oldRuntime == nil)
            starts = starts + 1
            assert(starts == 1)
            voice = assert(Audio.Play2D('game/tone.wav',
                {scope='session', bus='Music', looping=true}))
        end
        function OnSaveGame() return {marker=17} end
        function OnLoadGame(data)
            assert(data.marker == 17)
            loaded = true
        end
        function OnDestroy()
            -- Fresh-runtime LoadGame is a session boundary, not just a scene
            -- transition. Both requests must be rejected during old teardown.
            local sceneVoice = Audio.Play2D('game/tone.wav', {looping=true})
            local sessionVoice = Audio.Play2D('game/tone.wav', {scope='session', looping=true})
            assert(sceneVoice == nil and sessionVoice == nil)
        end
    )", CatalogAssetKind::Script);
    const auto empty = Asset("empty.lua", "return {}", CatalogAssetKind::Script);
    const auto level = ScriptedScene("Saved", empty);
    Rescan();
    ASSERT_TRUE(host.LoadScene(level, error)) << error;
    const std::vector<AutoloadDefinition> autoloads{{"Music", music, true}};
    ASSERT_TRUE(host.StartGame(root, autoloads, error)) << error;
    const double musicEnergy = Energy(Mix());
    ASSERT_GT(musicEnergy, 1e-5);
    ASSERT_EQ(audio->GetVoiceCount(), 1u);
    const auto save = root / "checkpoint.save.json";
    ASSERT_TRUE(host.SaveGame(save, error)) << error;
    for (int restore = 0; restore < 2; ++restore) {
        SCOPED_TRACE(restore);
        // This native handle remains observable after the old Lua state dies.
        const auto oldSession = Play(AudioPlayOptions{.scope = AudioScope::Session, .looping = true});
        ASSERT_TRUE(scripts->GetRuntime().ExecuteString(
            "Autoloads.Music.oldRuntime = true", "mark old audio runtime", error)) << error;
        ASSERT_TRUE(host.LoadGame(save, error)) << error;
        EXPECT_EQ(audio->GetStatus(oldSession), AudioVoiceStatus::Stopped);
        Reconcile();
        (void)Mix(); // Drain any already-rendered old-session boundary samples.
        EXPECT_NEAR(Energy(Mix()) / musicEnergy, 1, 0.05);
        EXPECT_EQ(audio->GetVoiceCount(), 1u);
        ASSERT_TRUE(scripts->GetRuntime().ExecuteString(R"(
            assert(Autoloads.Music.loaded and Autoloads.Music.starts == 1)
            assert(Autoloads.Music.oldRuntime == nil)
            assert(Autoloads.Music.voice:GetStatus() == 'Playing')
        )", "fresh audio runtime after LoadGame", error)) << error;
    }
    host.StopGame();
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    ExpectSilentAfterControlBoundary(Mix());
    EXPECT_LT(Energy(Mix()), 1e-10);
}

struct AudioDisabledSessionTests : AudioSessionTests {
    AudioOutputMode Output() const override { return AudioOutputMode::Disabled; }
};

TEST_F(AudioDisabledSessionTests, FailedStartupUnwindsAndUnavailableAudioAllowsHealthyRetry) {
    audio->EndSession();
    const auto empty = Asset("empty.lua", "return {}", CatalogAssetKind::Script);
    const auto level = ScriptedScene("Disabled", empty);
    const auto required = Asset("required.lua", R"(
        function OnStart()
            local voice, err = Audio.Play2D('game/tone.wav', {scope='session'})
            assert(voice, err)
        end
        function OnDestroy()
            assert(Audio.Play2D('game/tone.wav', {scope='session'}) == nil)
        end
    )", CatalogAssetKind::Script);
    const auto optional = Asset("optional.lua", R"(
        function OnStart()
            assert(not Audio.IsAvailable())
            local voice, err = Audio.Play2D('game/tone.wav', {scope='session'})
            assert(voice == nil and type(err) == 'string' and #err > 0)
        end
    )", CatalogAssetKind::Script);
    Rescan();
    ASSERT_TRUE(host.LoadScene(level, error)) << error;
    const std::vector<AutoloadDefinition> failing{{"Required", required, true}};
    EXPECT_FALSE(host.StartGame(root, failing, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(host.IsPlaying());
    EXPECT_FALSE(scripts->GetRuntime().IsInitialized());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    EXPECT_FALSE(audio->IsAvailable());
    const std::vector<AutoloadDefinition> healthy{{"Optional", optional, true}};
    ASSERT_TRUE(host.StartGame(root, healthy, error)) << error;
    EXPECT_TRUE(host.IsPlaying());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
    host.StopGame(); host.StopGame();
    EXPECT_FALSE(host.IsPlaying());
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

TEST_F(AudioSessionTests, EditorPlaySceneReplacementAndStopRespectSessionAndAuthoringState) {
    audio->EndSession();
    host.SetEditorMode();
    const auto script = LifecycleScript();
    const auto next = ScriptedScene("Next", script);
    Rescan();
    auto& scene = host.GetScene();
    scene.AddComponent<ScriptComponent>(scene.CreateEntity("original"), ScriptComponent{script, true});
    const auto authoredEntity = Source(true);
    const auto authoredId = scene.GetId(authoredEntity);
    const auto authored = *scene.TryGetComponent<AudioSource>(authoredEntity);
    Reconcile();
    EXPECT_LT(Energy(Mix()), 1e-10); // Edit must not activate authored autoplay.
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(host.PlayCurrentScene(root, {}, error)) << error;
        Reconcile();
        EXPECT_GT(Energy(Mix()), 1e-5);
        EXPECT_EQ(audio->GetVoiceCount(), 2u); // Startup callback and component.
        const auto session = Play(AudioPlayOptions{.scope = AudioScope::Session, .looping = true});
        ASSERT_TRUE(host.LoadScene(next, error)) << error;
        Reconcile();
        EXPECT_GT(Energy(Mix()), 1e-5);
        EXPECT_EQ(audio->GetVoiceCount(), 2u); // Session music and new startup only.
        EXPECT_EQ(audio->GetStatus(session), AudioVoiceStatus::Playing);
        host.StopPlay(); host.StopPlay();
        Reconcile();
        EXPECT_EQ(audio->GetVoiceCount(), 0u);
        EXPECT_LT(Energy(Mix()), 1e-10);
        const auto restored = scene.FindById(authoredId.ToString());
        ASSERT_TRUE(scene.IsValid(restored));
        const auto* source = scene.TryGetComponent<AudioSource>(restored);
        ASSERT_NE(source, nullptr);
        EXPECT_EQ(*source, authored);
        EXPECT_NE(scene.FindFirstByName("original"), NullEntity);
        EXPECT_EQ(scene.FindFirstByName("Next"), NullEntity);
    }
}
} // namespace
