#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <SimpleMath.h>
#include <entt/entity/entity.hpp>
#include <engine/audio/AudioComponents.hpp>
#include <engine/core/Subsystem.hpp>

namespace engine {

enum class AudioOutputMode { Device, Disabled, Offline };
enum class AudioVoiceStatus { Invalid, Pending, Playing, Paused, Finished, Stopped, Failed };
enum class AudioScope { Scene, Session, Preview };

struct AudioConfig {
    AudioOutputMode output = AudioOutputMode::Device;
    std::uint32_t maxVoices = 128;
    std::uint32_t sampleRate = 48000; // offline output rate; devices negotiate their own rate
};

/// A copyable, lifetime-checked token. Retaining/dropping it does not keep/stop playback.
/// Terminal status remains available while the originating subsystem lifetime exists.
class ENGINE_API AudioVoiceHandle {
public:
    AudioVoiceHandle();
    ~AudioVoiceHandle();
    AudioVoiceHandle(const AudioVoiceHandle&);
    AudioVoiceHandle& operator=(const AudioVoiceHandle&);
    AudioVoiceHandle(AudioVoiceHandle&&) noexcept;
    AudioVoiceHandle& operator=(AudioVoiceHandle&&) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
    friend class AudioSubsystem;
};

struct AudioPlayOptions {
    AudioLoadingMode loading = AudioLoadingMode::Buffered;
    AudioBus bus = AudioBus::SFX;
    AudioScope scope = AudioScope::Scene;
    bool looping = false;
    float volume = 1.0f;
    float pitch = 1.0f;
    AudioAttenuation attenuation = AudioAttenuation::Inverse;
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloff = 1.0f;
};

/// All operations, including offline mixing, are main-thread only. No backend types cross the SDK.
class ENGINE_API AudioSubsystem final : public Subsystem {
public:
    explicit AudioSubsystem(AudioConfig config = {});
    ~AudioSubsystem() override;
    std::string_view GetName() const override { return "AudioSubsystem"; }
    bool Initialize() override;
    void Shutdown() override;
    void LateUpdate(const FrameContext& frame) override;
    void ResetSimulation() override;

    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] std::string GetDiagnostic() const;
    /// Invalidates existing voices; explicit retry is performed on the main thread.
    [[nodiscard]] bool RetryDevice(std::string& error);
    [[nodiscard]] AudioVoiceHandle Play2D(const AssetReference& clip, const AudioPlayOptions& options, std::string& error);
    [[nodiscard]] AudioVoiceHandle PlayAt(const AssetReference& clip, DirectX::SimpleMath::Vector3 position,
                                         const AudioPlayOptions& options, std::string& error);
    [[nodiscard]] AudioVoiceHandle PlaySource(entt::entity entity, std::string& error);
    [[nodiscard]] AudioVoiceHandle GetSourceVoice(entt::entity entity) const;
    bool PauseSource(entt::entity entity);
    bool ResumeSource(entt::entity entity);
    bool StopSource(entt::entity entity);
    [[nodiscard]] AudioVoiceStatus GetStatus(const AudioVoiceHandle& voice) const;
    [[nodiscard]] std::string GetVoiceError(const AudioVoiceHandle& voice) const;
    bool Pause(const AudioVoiceHandle& voice);
    bool Resume(const AudioVoiceHandle& voice);
    bool Stop(const AudioVoiceHandle& voice);
    bool SetVolume(const AudioVoiceHandle& voice, float volume);
    bool SetPitch(const AudioVoiceHandle& voice, float pitch);
    /// Fades use playback time (freeze while paused). stopAtEnd releases the voice.
    bool Fade(const AudioVoiceHandle& voice, float targetVolume, double seconds, bool stopAtEnd = false);
    bool SetMasterVolume(float volume);
    [[nodiscard]] float GetMasterVolume() const;
    bool SetBusVolume(AudioBus bus, float volume);
    [[nodiscard]] float GetBusVolume(AudioBus bus) const;
    [[nodiscard]] std::size_t GetVoiceCount() const;
    [[nodiscard]] entt::entity GetEffectiveListener() const;
    [[nodiscard]] std::string GetListenerDiagnostic() const;
    [[nodiscard]] AudioVoiceHandle Preview(const AssetReference& clip, std::string& error);
    void StopPreview();

    /// Interleaved stereo floats. Only Offline mode; jobs are pumped deterministically here.
    [[nodiscard]] bool MixOffline(std::span<float> stereoSamples, std::string& error);

    // Engine lifecycle boundaries. Startup protection applies to StartGame's post-callback reset only.
    void BeginSession(bool protectStartupReset = false);
    void EndSession();
    void BeginSceneTransition();
    void EndSceneTransition();
    void SetGamePaused(bool paused);

private:
    struct Impl;
    AudioConfig config_;
    std::unique_ptr<Impl> impl_;
    AudioVoiceHandle Play(const AssetReference&, const AudioPlayOptions&, AudioSpatialMode,
                          DirectX::SimpleMath::Vector3, entt::entity, std::string&);
};
} // namespace engine
