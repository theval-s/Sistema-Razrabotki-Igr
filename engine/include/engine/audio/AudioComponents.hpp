#pragma once

#include <optional>
#include <engine/assets/AssetCatalog.hpp>

namespace engine {

enum class AudioLoadingMode { Buffered, Streamed };
enum class AudioBus { Music, SFX, UI };
enum class AudioSpatialMode { TwoD, ThreeD };
enum class AudioAttenuation { None, Inverse, Linear, Exponential };

/// Authored data only. Playback state belongs to AudioSubsystem, never to a scene snapshot.
struct AudioSource {
    std::optional<AssetReference> clip;
    bool enabled = true;
    bool playOnStart = false;
    bool looping = false;
    AudioLoadingMode loading = AudioLoadingMode::Buffered;
    AudioBus bus = AudioBus::SFX;
    float volume = 1.0f;
    float pitch = 1.0f;
    AudioSpatialMode spatial = AudioSpatialMode::ThreeD;
    AudioAttenuation attenuation = AudioAttenuation::Inverse;
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloff = 1.0f;
    bool operator==(const AudioSource&) const = default;
};

struct AudioListener {
    bool enabled = true;
    bool operator==(const AudioListener&) const = default;
};

/// Also used by the document codec and runtime/Lua controls. Volume is in [0,1].
[[nodiscard]] ENGINE_API bool ValidateAudioSource(const AudioSource& source, std::string& error);

} // namespace engine
