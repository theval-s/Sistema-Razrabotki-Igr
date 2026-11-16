#include <engine/audio/AudioSubsystem.hpp>

#include "Miniaudio.hpp"
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Utf.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace engine {
namespace {
using DirectX::SimpleMath::Vector3;
bool Gain(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
bool Positive(float value) { return std::isfinite(value) && value > 0; }
bool Finite(Vector3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool BusValid(AudioBus bus) { return bus >= AudioBus::Music && bus <= AudioBus::UI; }
AudioSource SourceSettings(const AudioPlayOptions& options, AudioSpatialMode spatial) {
    AudioSource source;
    source.loading = options.loading; source.bus = options.bus; source.looping = options.looping;
    source.volume = options.volume; source.pitch = options.pitch; source.spatial = spatial;
    source.attenuation = options.attenuation; source.minDistance = options.minDistance;
    source.maxDistance = options.maxDistance; source.rolloff = options.rolloff;
    return source;
}
AudioPlayOptions PlaySettings(const AudioSource& source) {
    AudioPlayOptions options;
    options.loading = source.loading; options.bus = source.bus; options.looping = source.looping;
    options.volume = source.volume; options.pitch = source.pitch; options.attenuation = source.attenuation;
    options.minDistance = source.minDistance; options.maxDistance = source.maxDistance; options.rolloff = source.rolloff;
    return options;
}
ma_attenuation_model Attenuation(AudioAttenuation model) {
    switch (model) {
    case AudioAttenuation::None: return ma_attenuation_model_none;
    case AudioAttenuation::Linear: return ma_attenuation_model_linear;
    case AudioAttenuation::Exponential: return ma_attenuation_model_exponential;
    default: return ma_attenuation_model_inverse;
    }
}
} // namespace

bool ValidateAudioSource(const AudioSource& source, std::string& error) {
    if (source.loading != AudioLoadingMode::Buffered && source.loading != AudioLoadingMode::Streamed)
        error = "Invalid audio loading mode";
    else if (!BusValid(source.bus)) error = "Invalid audio bus";
    else if (source.spatial != AudioSpatialMode::TwoD && source.spatial != AudioSpatialMode::ThreeD)
        error = "Invalid audio spatial mode";
    else if (source.attenuation < AudioAttenuation::None || source.attenuation > AudioAttenuation::Exponential)
        error = "Invalid audio attenuation model";
    else if (!Gain(source.volume)) error = "Audio volume must be finite and in [0,1]";
    else if (!Positive(source.pitch)) error = "Audio pitch must be finite and positive";
    else if (!Positive(source.minDistance) || !Positive(source.maxDistance) || source.maxDistance < source.minDistance)
        error = "Audio distances must be finite, positive, and maximum >= minimum (metres)";
    else if (!std::isfinite(source.rolloff) || source.rolloff < 0) error = "Audio rolloff must be finite and nonnegative";
    else { error.clear(); return true; }
    return false;
}

struct AudioVoiceHandle::State {
    std::weak_ptr<void> lifetime;
    AudioVoiceStatus status = AudioVoiceStatus::Pending;
    std::string error;
};
AudioVoiceHandle::AudioVoiceHandle() = default;
AudioVoiceHandle::~AudioVoiceHandle() = default;
AudioVoiceHandle::AudioVoiceHandle(const AudioVoiceHandle&) = default;
AudioVoiceHandle& AudioVoiceHandle::operator=(const AudioVoiceHandle&) = default;
AudioVoiceHandle::AudioVoiceHandle(AudioVoiceHandle&&) noexcept = default;
AudioVoiceHandle& AudioVoiceHandle::operator=(AudioVoiceHandle&&) noexcept = default;
AudioVoiceHandle::operator bool() const noexcept { return state_ && !state_->lifetime.expired(); }

struct AudioSubsystem::Impl {
    // Never move initialized backend objects: Impl, Voice and their members have stable addresses.
    struct Voice {
        ma_resource_manager_data_source resource{};
        ma_sound sound{};
        bool resourceReady = false, soundReady = false;
        AudioVoiceHandle handle;
        CatalogEntry clip; // immutable descriptor copy; no pointers survive a catalog rescan
        AudioPlayOptions options;
        AudioSpatialMode spatial = AudioSpatialMode::TwoD;
        Vector3 position{};
        entt::entity entity = entt::null;
        std::uint64_t scopeGeneration = 0;
        bool explicitlyPaused = false;
        bool fading = false, fadeStop = false;
        float fadeTarget = 1;
        double fadeSeconds = 0;
        ma_uint64 fadeEnd = 0;
        ma_uint64 gainWriteTime = 0;
        float gainWriteValue = 1;
        ~Voice() {
            // Detach and wait for the mixer before releasing the resource lease. Resource uninit
            // waits for outstanding jobs (pumps them itself with a no-threading resource manager).
            if (soundReady) ma_sound_uninit(&sound);
            if (resourceReady) ma_resource_manager_data_source_uninit(&resource);
        }
    };
    struct SourceRuntime {
        AudioSource authored;
        AudioVoiceHandle primary;
        bool activated = false;
    };
    ma_resource_manager resources{};
    ma_engine backend{};
    std::array<ma_sound_group, 3> groups{};
    bool resourcesReady = false, backendReady = false;
    std::size_t groupsReady = 0;
    bool initialized = false, available = false, session = false, sessionEnded = false;
    bool transition = false, protectReset = false, gamePaused = false;
    std::atomic<bool> deviceStopped{false};
    std::shared_ptr<void> lifetime;
    std::array<std::uint64_t, 3> generations{1, 1, 1};
    std::vector<std::unique_ptr<Voice>> voices;
    std::unordered_map<entt::entity, SourceRuntime> sources;
    Scene* scene = nullptr;
    std::weak_ptr<void> sceneLifetime;
    std::size_t observer = 0;
    entt::entity listener = entt::null;
    std::string diagnostic, listenerDiagnostic;
    float master = 1;
    std::array<float, 3> busVolumes{1, 1, 1};

    ~Impl() { Close(); }
    void Close() {
        available = false;
        if (scene && !sceneLifetime.expired() && observer) scene->RemoveLifecycleObserver(observer);
        observer = 0; scene = nullptr;
        lifetime.reset();
        // Quiesce the device before reclaiming nodes, then drain resource jobs before their manager.
        if (backendReady) ma_engine_stop(&backend);
        voices.clear(); sources.clear();
        while (groupsReady) ma_sound_group_uninit(&groups[--groupsReady]);
        if (backendReady) ma_engine_uninit(&backend);
        backendReady = false;
        if (resourcesReady) ma_resource_manager_uninit(&resources);
        resourcesReady = false;
        initialized = false;
    }
    Voice* Find(const AudioVoiceHandle& handle) const {
        if (!handle.state_ || handle.state_->lifetime.lock() != lifetime || !lifetime) return nullptr;
        for (const auto& voice : voices) if (voice->handle.state_ == handle.state_) return voice.get();
        return nullptr;
    }
    void Finish(Voice& voice, AudioVoiceStatus status, std::string error = {}) {
        voice.handle.state_->status = status;
        voice.handle.state_->error = std::move(error);
        if (status == AudioVoiceStatus::Failed)
            log::Engine().warn("Audio clip {} ({}): {}", voice.clip.metadata.id.ToString(), voice.clip.path, voice.handle.state_->error);
    }
    void Collect() {
        std::erase_if(voices, [](const auto& voice) {
            const auto status = voice->handle.state_->status;
            return status == AudioVoiceStatus::Stopped || status == AudioVoiceStatus::Finished || status == AudioVoiceStatus::Failed;
        });
    }
    void StopScope(AudioScope scope) {
        ++generations[static_cast<std::size_t>(scope)];
        for (auto& voice : voices) if (voice->options.scope == scope) Finish(*voice, AudioVoiceStatus::Stopped);
        Collect();
        if (scope == AudioScope::Scene) { sources.clear(); listener = entt::null; }
    }
    bool Paused(const Voice& voice) const {
        return voice.explicitlyPaused || (gamePaused && voice.options.bus != AudioBus::UI && voice.options.scope != AudioScope::Preview);
    }
    void ApplyPause(Voice& voice) {
        if (!voice.soundReady) return;
        if (Paused(voice)) {
            ma_sound_stop(&voice.sound);
            voice.handle.state_->status = AudioVoiceStatus::Paused;
        } else if (voice.handle.state_->status != AudioVoiceStatus::Playing) {
            const auto result = ma_sound_start(&voice.sound);
            if (result != MA_SUCCESS) Finish(voice, AudioVoiceStatus::Failed, ma_result_description(result));
            else voice.handle.state_->status = AudioVoiceStatus::Playing;
        }
    }
    void Spatial(Voice& voice) {
        if (!voice.soundReady) return;
        const bool spatial = voice.spatial == AudioSpatialMode::ThreeD;
        ma_sound_set_spatialization_enabled(&voice.sound, spatial);
        ma_sound_set_pinned_listener_index(&voice.sound, 0);
        ma_sound_set_doppler_factor(&voice.sound, 0);
        ma_sound_set_position(&voice.sound, voice.position.x, voice.position.y, voice.position.z);
        ma_sound_set_attenuation_model(&voice.sound, Attenuation(voice.options.attenuation));
        ma_sound_set_min_distance(&voice.sound, voice.options.minDistance);
        ma_sound_set_max_distance(&voice.sound, voice.options.maxDistance);
        ma_sound_set_rolloff(&voice.sound, voice.options.rolloff);
        // Listener disabling alone does not mute a pinned listener in miniaudio. Gate separately.
        ma_sound_set_volume(&voice.sound, spatial && (listener == entt::null || !Finite(voice.position)) ? 0.0f : 1.0f);
    }
    void SyncListener() {
        entt::entity chosen = entt::null;
        unsigned count = 0;
        if (scene && !sceneLifetime.expired()) {
            scene->ForEach<AudioListener>([&](entt::entity entity, const AudioListener& value) {
                if (value.enabled) { ++count; chosen = entity; }
            });
            if (!count) chosen = scene->GetActiveCamera();
        }
        std::string message;
        if (count > 1) { chosen = entt::null; message = "Multiple enabled audio listeners; spatial output suppressed"; }
        else if (chosen == entt::null) message = "No audio listener or active scene camera; spatial output suppressed";
        if (chosen != entt::null) {
            const auto world = scene->GetWorldMatrix(chosen);
            const auto position = world.Translation();
            auto forward = Vector3::TransformNormal(-Vector3::UnitZ, world);
            auto up = Vector3::TransformNormal(Vector3::UnitY, world);
            bool valid = Finite(position) && Finite(forward) && Finite(up) && forward.LengthSquared() > 1e-12f;
            if (valid) {
                forward.Normalize();
                up -= forward * up.Dot(forward);
                valid = up.LengthSquared() > 1e-12f && Finite(up);
            }
            if (!valid) { chosen = entt::null; message = "Degenerate audio listener transform; spatial output suppressed"; }
            else {
                up.Normalize();
                ma_engine_listener_set_position(&backend, 0, position.x, position.y, position.z);
                ma_engine_listener_set_direction(&backend, 0, forward.x, forward.y, forward.z);
                ma_engine_listener_set_world_up(&backend, 0, up.x, up.y, up.z);
                ma_engine_listener_set_velocity(&backend, 0, 0, 0, 0);
            }
        }
        if (message != listenerDiagnostic && !message.empty()) log::Engine().warn("Audio: {}", message);
        listenerDiagnostic = std::move(message);
        listener = chosen;
    }
    void StartFade(Voice& voice) {
        const auto frames = static_cast<ma_uint64>(voice.fadeSeconds * ma_engine_get_sample_rate(&backend));
        const auto time = ma_sound_get_time_in_pcm_frames(&voice.sound);
        // Miniaudio has one deferred envelope-setting slot. A gain assignment followed by a
        // fade before processing (including while paused) must not read the old mixer gain.
        const float begin = time == voice.gainWriteTime ? voice.gainWriteValue : -1.0f;
        ma_sound_set_fade_in_pcm_frames(&voice.sound, begin, voice.fadeTarget, frames);
        voice.gainWriteTime = time; voice.gainWriteValue = begin;
        voice.fadeEnd = time + frames;
    }
    void Poll() {
        for (auto& ptr : voices) {
            auto& voice = *ptr;
            if (voice.scopeGeneration != generations[static_cast<std::size_t>(voice.options.scope)]) {
                Finish(voice, AudioVoiceStatus::Stopped); continue;
            }
            if (!voice.soundReady) {
                const auto result = ma_resource_manager_data_source_result(&voice.resource);
                if (result == MA_BUSY) continue;
                if (result != MA_SUCCESS) { Finish(voice, AudioVoiceStatus::Failed, ma_result_description(result)); continue; }
                ma_uint32 channels = 0;
                const auto formatResult = ma_data_source_get_data_format(&voice.resource, nullptr, &channels, nullptr, nullptr, 0);
                if (formatResult != MA_SUCCESS) { Finish(voice, AudioVoiceStatus::Failed, ma_result_description(formatResult)); continue; }
                if (voice.spatial == AudioSpatialMode::ThreeD && channels != 1) {
                    Finish(voice, AudioVoiceStatus::Failed, "3D point emitters require a mono clip; use 2D for multichannel audio"); continue;
                }
                const auto initResult = ma_sound_init_from_data_source(&backend, &voice.resource,
                    MA_SOUND_FLAG_NO_SPATIALIZATION, &groups[static_cast<std::size_t>(voice.options.bus)], &voice.sound);
                if (initResult != MA_SUCCESS) { Finish(voice, AudioVoiceStatus::Failed, ma_result_description(initResult)); continue; }
                voice.soundReady = true;
                ma_sound_set_looping(&voice.sound, voice.options.looping);
                ma_sound_set_pitch(&voice.sound, voice.options.pitch);
                ma_sound_set_fade_in_pcm_frames(&voice.sound, voice.options.volume, voice.options.volume, 0);
                voice.gainWriteTime = 0; voice.gainWriteValue = voice.options.volume;
                Spatial(voice);
                if (voice.fading) StartFade(voice);
                ApplyPause(voice); // Initial pose, gain and listener gating precede start.
            } else if (voice.fading && ma_sound_get_time_in_pcm_frames(&voice.sound) >= voice.fadeEnd) {
                voice.fading = false;
                voice.options.volume = voice.fadeTarget;
                if (voice.fadeStop) Finish(voice, AudioVoiceStatus::Stopped);
            } else if (ma_sound_at_end(&voice.sound)) Finish(voice, AudioVoiceStatus::Finished);
            if (voice.soundReady) {
                const auto result = ma_resource_manager_data_source_result(&voice.resource);
                if (result != MA_SUCCESS && result != MA_BUSY) Finish(voice, AudioVoiceStatus::Failed, ma_result_description(result));
            }
        }
        Collect();
    }
    void PumpJobs() {
        while (ma_resource_manager_process_next_job(&resources) == MA_SUCCESS) {}
    }
};

AudioSubsystem::AudioSubsystem(AudioConfig config) : config_(config), impl_(std::make_unique<Impl>()) {}
AudioSubsystem::~AudioSubsystem() = default;

bool AudioSubsystem::Initialize() {
    if (impl_->initialized) return true;
    auto& p = *impl_;
    p.initialized = true;
    p.lifetime = std::make_shared<int>(0);
    p.sessionEnded = false; p.transition = false; p.protectReset = false;
    p.gamePaused = TryGetEngine() && GetEngine().IsPaused();
    p.diagnostic.clear(); p.listenerDiagnostic.clear();
    p.deviceStopped.store(false);
    if (TryGetEngine()) {
        p.scene = &GetEngine().GetScene(); p.sceneLifetime = p.scene->GetLifetimeToken();
        p.observer = p.scene->AddLifecycleObserver([this](Scene::LifecycleEvent event, entt::entity entity, entt::id_type component) {
            if (event == Scene::LifecycleEvent::EntityDestroying || component == entt::type_hash<AudioSource>::value()) {
                StopSource(entity); impl_->sources.erase(entity);
            }
        });
    }
    if (config_.output == AudioOutputMode::Disabled) { p.diagnostic = "Audio is disabled"; return true; }
    if (config_.maxVoices == 0 || config_.sampleRate < 8000 || config_.sampleRate > 384000) {
        p.diagnostic = "Invalid audio configuration (voice cap or sample rate)";
        log::Engine().warn("Audio: {}", p.diagnostic); return true;
    }
    auto resourceConfig = ma_resource_manager_config_init();
    resourceConfig.decodedFormat = ma_format_f32;
    resourceConfig.decodedChannels = 0; // Preserve mono/stereo; required for point-emitter validation.
    resourceConfig.decodedSampleRate = 0;
    if (config_.output == AudioOutputMode::Offline) {
        resourceConfig.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
        resourceConfig.jobThreadCount = 0;
    } else resourceConfig.jobThreadCount = 1;
    auto result = ma_resource_manager_init(&resourceConfig, &p.resources);
    if (result == MA_SUCCESS) {
        p.resourcesReady = true;
        auto engineConfig = ma_engine_config_init();
        engineConfig.pResourceManager = &p.resources;
        engineConfig.noAutoStart = MA_TRUE;
        engineConfig.noDevice = config_.output == AudioOutputMode::Offline;
        engineConfig.channels = 2;
        engineConfig.sampleRate = engineConfig.noDevice ? config_.sampleRate : 0;
        engineConfig.pProcessUserData = &p;
        engineConfig.notificationCallback = [](const ma_device_notification* notification) {
            // The device's user data is owned by ma_engine. No scene/logging/UI work on this thread.
            auto* engine = static_cast<ma_engine*>(notification->pDevice->pUserData);
            auto* state = engine ? static_cast<Impl*>(engine->pProcessUserData) : nullptr;
            if (state && notification->type == ma_device_notification_type_stopped)
                state->deviceStopped.store(true, std::memory_order_release);
        };
        result = ma_engine_init(&engineConfig, &p.backend);
        if (result == MA_SUCCESS) {
            p.backendReady = true;
            // Set before the device starts. Engine coordinates: RH, +Y up, local -Z forward.
            p.backend.listeners[0].config.handedness = ma_handedness_right;
            for (auto& group : p.groups) {
                result = ma_sound_group_init(&p.backend, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &group);
                if (result != MA_SUCCESS) break;
                ++p.groupsReady;
            }
            if (result == MA_SUCCESS) {
                ma_engine_set_volume(&p.backend, p.master);
                for (std::size_t i = 0; i < p.groups.size(); ++i) ma_sound_group_set_volume(&p.groups[i], p.busVolumes[i]);
                if (config_.output == AudioOutputMode::Device) result = ma_engine_start(&p.backend);
            }
        }
    }
    p.available = result == MA_SUCCESS;
    if (!p.available) {
        p.diagnostic = std::string("Audio unavailable: ") + ma_result_description(result) + "; use RetryDevice to retry";
        log::Engine().warn("{}", p.diagnostic);
    }
    return true; // Device/decoder availability must never prevent the engine from starting.
}
void AudioSubsystem::Shutdown() { impl_->session = false; impl_->sessionEnded = true; impl_->Close(); }
bool AudioSubsystem::IsAvailable() const { return impl_->available; }
std::string AudioSubsystem::GetDiagnostic() const { return impl_->diagnostic; }
bool AudioSubsystem::RetryDevice(std::string& error) {
    const bool session = impl_->session;
    const bool ended = impl_->sessionEnded;
    const bool transition = impl_->transition;
    Shutdown(); Initialize();
    impl_->session = session; impl_->sessionEnded = ended; impl_->transition = transition;
    error = impl_->diagnostic;
    return IsAvailable();
}

AudioVoiceHandle AudioSubsystem::Play(const AssetReference& clip, const AudioPlayOptions& options,
    AudioSpatialMode spatial, Vector3 position, entt::entity entity, std::string& error) {
    auto fail = [&](std::string message) {
        error = std::move(message);
        log::Engine().warn("Audio clip {} ({}): {}", clip.id.ToString(), clip.path, error);
        return AudioVoiceHandle{};
    };
    auto& p = *impl_;
    if (!p.available) return fail(p.diagnostic.empty() ? "Audio is not initialized" : p.diagnostic);
    if (!TryGetEngine()) return fail("Audio playback requires an Engine catalog");
    if (options.scope < AudioScope::Scene || options.scope > AudioScope::Preview) return fail("Invalid audio ownership scope");
    if (p.transition || (p.sessionEnded && options.scope != AudioScope::Preview)) return fail("Audio playback is suspended during scene/session teardown");
    if (options.scope != AudioScope::Preview && GetEngine().IsEditorMode() && !p.session)
        return fail("Scene playback requires Play mode; use explicit Preview in Edit mode");
    if (options.scope == AudioScope::Preview && (!GetEngine().IsEditorMode() || p.session))
        return fail("Audio preview is available only in editor Edit mode");
    if (spatial == AudioSpatialMode::ThreeD && options.scope != AudioScope::Scene)
        return fail("Positional playback must be scene-owned");
    if (!Finite(position)) return fail("Audio position must be finite");
    if (!ValidateAudioSource(SourceSettings(options, spatial), error)) return fail(error);
    if (p.voices.size() >= config_.maxVoices) return fail("Audio voice capacity reached (pending and paused voices count)");
    const auto& catalog = options.scope == AudioScope::Scene ? GetEngine().GetSceneAssets().GetCatalog() : GetEngine().GetAssets().GetCatalog();
    const auto* descriptor = catalog.Find(clip.id, CatalogAssetKind::AudioClip);
    if (!descriptor) return fail("Missing audio clip UUID; path is a diagnostic hint only");
    auto voice = std::make_unique<Impl::Voice>();
    voice->clip = *descriptor;
    voice->options = options; voice->spatial = spatial; voice->position = position; voice->entity = entity;
    voice->scopeGeneration = p.generations[static_cast<std::size_t>(options.scope)];
    voice->handle.state_ = std::make_shared<AudioVoiceHandle::State>();
    voice->handle.state_->lifetime = p.lifetime;
    auto config = ma_resource_manager_data_source_config_init();
    config.flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC |
        (options.loading == AudioLoadingMode::Buffered ? MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE : MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM);
#ifdef _WIN32
    config.pFilePathW = voice->clip.source.c_str(); // Native wide, seekable filesystem access, including streaming.
#else
    config.pFilePath = voice->clip.source.c_str();
#endif
    const auto result = ma_resource_manager_data_source_init_ex(&p.resources, &config, &voice->resource);
    if (result != MA_SUCCESS) return fail(ma_result_description(result));
    voice->resourceReady = true;
    auto handle = voice->handle;
    p.voices.push_back(std::move(voice));
    p.SyncListener();
    error.clear();
    return handle;
}
AudioVoiceHandle AudioSubsystem::Play2D(const AssetReference& clip, const AudioPlayOptions& options, std::string& error) {
    return Play(clip, options, AudioSpatialMode::TwoD, {}, entt::null, error);
}
AudioVoiceHandle AudioSubsystem::PlayAt(const AssetReference& clip, Vector3 position, const AudioPlayOptions& options, std::string& error) {
    return Play(clip, options, AudioSpatialMode::ThreeD, position, entt::null, error);
}
AudioVoiceHandle AudioSubsystem::PlaySource(entt::entity entity, std::string& error) {
    auto* source = impl_->scene && !impl_->sceneLifetime.expired() ? impl_->scene->TryGetComponent<AudioSource>(entity) : nullptr;
    if (!source || !source->enabled || !source->clip) { error = "AudioSource requires a valid enabled entity and an assigned clip"; return {}; }
    StopSource(entity);
    auto& runtime = impl_->sources[entity];
    runtime.authored = *source; runtime.activated = true; // Record/consume intent before requesting asynchronous loading.
    runtime.primary = Play(*source->clip, PlaySettings(*source), source->spatial,
        impl_->scene->GetWorldMatrix(entity).Translation(), entity, error);
    return runtime.primary;
}
AudioVoiceHandle AudioSubsystem::GetSourceVoice(entt::entity entity) const {
    const auto it = impl_->sources.find(entity);
    return it == impl_->sources.end() ? AudioVoiceHandle{} : it->second.primary;
}
bool AudioSubsystem::PauseSource(entt::entity entity) { return Pause(GetSourceVoice(entity)); }
bool AudioSubsystem::ResumeSource(entt::entity entity) { return Resume(GetSourceVoice(entity)); }
bool AudioSubsystem::StopSource(entt::entity entity) {
    auto& p = *impl_;
    auto it = p.sources.find(entity);
    if (it != p.sources.end()) { it->second.activated = true; Stop(it->second.primary); return true; }
    const auto* source = p.scene && !p.sceneLifetime.expired() ? p.scene->TryGetComponent<AudioSource>(entity) : nullptr;
    if (!source) return false;
    p.sources.emplace(entity, Impl::SourceRuntime{*source, {}, true});
    return true;
}
AudioVoiceStatus AudioSubsystem::GetStatus(const AudioVoiceHandle& voice) const {
    if (!voice.state_ || !impl_->lifetime || voice.state_->lifetime.lock() != impl_->lifetime) return AudioVoiceStatus::Invalid;
    return voice.state_->status;
}
std::string AudioSubsystem::GetVoiceError(const AudioVoiceHandle& voice) const {
    return GetStatus(voice) == AudioVoiceStatus::Invalid ? "Invalid or expired audio voice" : voice.state_->error;
}
bool AudioSubsystem::Pause(const AudioVoiceHandle& handle) {
    auto* voice = impl_->Find(handle); if (!voice) return false;
    voice->explicitlyPaused = true; impl_->ApplyPause(*voice); return true;
}
bool AudioSubsystem::Resume(const AudioVoiceHandle& handle) {
    auto* voice = impl_->Find(handle); if (!voice) return false;
    voice->explicitlyPaused = false; impl_->ApplyPause(*voice); return true;
}
bool AudioSubsystem::Stop(const AudioVoiceHandle& handle) {
    auto* voice = impl_->Find(handle); if (!voice) return false;
    impl_->Finish(*voice, AudioVoiceStatus::Stopped); impl_->Collect(); return true;
}
bool AudioSubsystem::SetVolume(const AudioVoiceHandle& handle, float volume) {
    auto* voice = impl_->Find(handle); if (!voice || !Gain(volume)) return false;
    voice->options.volume = volume; voice->fading = false;
    if (voice->soundReady) {
        ma_sound_set_fade_in_pcm_frames(&voice->sound, volume, volume, 0);
        voice->gainWriteTime = ma_sound_get_time_in_pcm_frames(&voice->sound);
        voice->gainWriteValue = volume;
    }
    return true;
}
bool AudioSubsystem::SetPitch(const AudioVoiceHandle& handle, float pitch) {
    auto* voice = impl_->Find(handle); if (!voice || !Positive(pitch)) return false;
    voice->options.pitch = pitch;
    if (voice->soundReady) ma_sound_set_pitch(&voice->sound, pitch);
    return true;
}
bool AudioSubsystem::Fade(const AudioVoiceHandle& handle, float target, double seconds, bool stopAtEnd) {
    auto* voice = impl_->Find(handle);
    if (!voice || !Gain(target) || !std::isfinite(seconds) || seconds < 0 || seconds > 86400) return false;
    voice->fading = true; voice->fadeTarget = target; voice->fadeSeconds = seconds; voice->fadeStop = stopAtEnd;
    if (voice->soundReady) impl_->StartFade(*voice);
    return true;
}
bool AudioSubsystem::SetMasterVolume(float volume) {
    if (!Gain(volume)) return false;
    impl_->master = volume;
    if (impl_->backendReady) ma_engine_set_volume(&impl_->backend, volume);
    return true;
}
float AudioSubsystem::GetMasterVolume() const { return impl_->master; }
bool AudioSubsystem::SetBusVolume(AudioBus bus, float volume) {
    if (!BusValid(bus) || !Gain(volume)) return false;
    const auto index = static_cast<std::size_t>(bus);
    impl_->busVolumes[index] = volume;
    if (index < impl_->groupsReady) ma_sound_group_set_volume(&impl_->groups[index], volume);
    return true;
}
float AudioSubsystem::GetBusVolume(AudioBus bus) const { return BusValid(bus) ? impl_->busVolumes[static_cast<std::size_t>(bus)] : 0; }
std::size_t AudioSubsystem::GetVoiceCount() const { return impl_->voices.size(); }
entt::entity AudioSubsystem::GetEffectiveListener() const { return impl_->listener; }
std::string AudioSubsystem::GetListenerDiagnostic() const { return impl_->listenerDiagnostic; }
AudioVoiceHandle AudioSubsystem::Preview(const AssetReference& clip, std::string& error) {
    StopPreview(); AudioPlayOptions options; options.scope = AudioScope::Preview; options.bus = AudioBus::UI;
    return Play2D(clip, options, error);
}
void AudioSubsystem::StopPreview() { impl_->StopScope(AudioScope::Preview); }

void AudioSubsystem::BeginSession(bool protectStartupReset) {
    impl_->StopScope(AudioScope::Scene); impl_->StopScope(AudioScope::Session); StopPreview();
    impl_->session = true; impl_->sessionEnded = false; impl_->transition = false; impl_->protectReset = protectStartupReset;
}
void AudioSubsystem::EndSession() {
    impl_->session = false; impl_->sessionEnded = true; impl_->protectReset = false;
    impl_->StopScope(AudioScope::Scene); impl_->StopScope(AudioScope::Session); StopPreview();
}
void AudioSubsystem::BeginSceneTransition() {
    impl_->transition = true; impl_->protectReset = false;
    impl_->StopScope(AudioScope::Scene); StopPreview();
}
void AudioSubsystem::EndSceneTransition() { impl_->transition = false; }
void AudioSubsystem::ResetSimulation() {
    if (impl_->protectReset) { impl_->protectReset = false; return; }
    impl_->StopScope(AudioScope::Scene); StopPreview();
}
void AudioSubsystem::SetGamePaused(bool paused) {
    impl_->gamePaused = paused;
    impl_->Poll();
    for (auto& voice : impl_->voices) impl_->ApplyPause(*voice);
    impl_->Collect();
}

void AudioSubsystem::LateUpdate(const FrameContext&) {
    auto& p = *impl_;
    if (!p.available) return;
    if (p.deviceStopped.exchange(false, std::memory_order_acq_rel)) {
        p.available = false; p.diagnostic = "Audio device stopped; use RetryDevice to reinitialize";
        for (auto& voice : p.voices) p.Finish(*voice, AudioVoiceStatus::Failed, p.diagnostic);
        p.Collect(); log::Engine().warn("{}", p.diagnostic); return;
    }
    p.SyncListener();
    if (p.scene && !p.sceneLifetime.expired() && !p.transition) {
        p.scene->ForEach<AudioSource>([&](entt::entity entity, const AudioSource& source) {
            auto [it, inserted] = p.sources.try_emplace(entity, Impl::SourceRuntime{source, {}, false});
            auto& runtime = it->second;
            const auto old = runtime.authored;
            const bool replaced = !inserted && (old.clip != source.clip || old.loading != source.loading || old.spatial != source.spatial);
            if (replaced || !source.enabled) {
                Stop(runtime.primary);
                // Replacement consumes autoplay; disabling allows a new activation upon reenable.
                runtime.activated = source.enabled;
            }
            if (!source.enabled) runtime.activated = false;
            else if (!runtime.activated && p.session && !p.sessionEnded) {
                runtime.activated = true;
                if (source.playOnStart && source.clip) { std::string error; (void)PlaySource(entity, error); }
            }
            if (auto* voice = p.Find(runtime.primary)) {
                std::string error;
                if (!ValidateAudioSource(source, error)) p.Finish(*voice, AudioVoiceStatus::Failed, error);
                else {
                    if (old.volume != source.volume) SetVolume(runtime.primary, source.volume);
                    if (old.pitch != source.pitch) SetPitch(runtime.primary, source.pitch);
                    if (old.bus != source.bus) {
                        voice->options.bus = source.bus;
                        if (voice->soundReady) ma_node_attach_output_bus(&voice->sound, 0, &p.groups[static_cast<std::size_t>(source.bus)], 0);
                    }
                    voice->options.attenuation = source.attenuation; voice->options.minDistance = source.minDistance;
                    voice->options.maxDistance = source.maxDistance; voice->options.rolloff = source.rolloff;
                    voice->options.looping = source.looping;
                    if (voice->soundReady) { ma_sound_set_looping(&voice->sound, source.looping); p.ApplyPause(*voice); }
                }
            }
            runtime.authored = source;
        });
    }
    for (auto& voice : p.voices) {
        if (voice->entity != entt::null && p.scene && !p.sceneLifetime.expired())
            voice->position = p.scene->GetWorldMatrix(voice->entity).Translation();
        p.Spatial(*voice);
    }
    p.Poll();
}

bool AudioSubsystem::MixOffline(std::span<float> samples, std::string& error) {
    auto& p = *impl_;
    if (config_.output != AudioOutputMode::Offline || !p.available || samples.size() % 2 != 0) {
        error = "MixOffline requires available Offline audio and interleaved stereo frames"; return false;
    }
    p.SyncListener();
    for (auto& voice : p.voices) p.Spatial(*voice);
    // Bounded chunks give streamed pages deterministic service without worker threads or sleeps.
    std::size_t offset = 0;
    do {
        p.PumpJobs(); p.Poll();
        const auto frames = std::min<std::size_t>((samples.size() - offset) / 2, 256);
        if (!frames) break;
        const auto result = ma_engine_read_pcm_frames(&p.backend, samples.data() + offset, frames, nullptr);
        if (result != MA_SUCCESS) { error = ma_result_description(result); return false; }
        offset += frames * 2;
        p.Poll();
    } while (offset < samples.size());
    error.clear(); return true;
}
} // namespace engine
