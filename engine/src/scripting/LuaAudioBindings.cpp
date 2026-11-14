#include "LuaBindingGroups.hpp"

#include <engine/audio/AudioSubsystem.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/scripting/LuaBindingsContext.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <sol/sol.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace engine {
namespace {
using Vector3 = DirectX::SimpleMath::Vector3;
using Result = std::tuple<bool, std::string>;
using ValueResult = std::tuple<sol::object, std::string>;

// No subsystem/context pointer in userdata, and no custom GC: dropping this token
// never stops playback. Runtime callbacks own the context, as in other groups.
struct LuaAudioVoice { AudioVoiceHandle handle; };

void Require(bool valid, const char* error) {
    if (!valid) throw std::runtime_error(error);
}
bool Present(const sol::object& value) {
    return value.valid() && value.get_type() != sol::type::nil;
}
std::string Text(const sol::object& value) {
    Require(value.get_type() == sol::type::string, "expected a string");
    return value.as<std::string>();
}
double Number(const sol::object& value) {
    Require(value.get_type() == sol::type::number, "expected a number");
    const double number = value.as<double>();
    Require(std::isfinite(number), "number must be finite");
    return number;
}
float Scalar(const sol::object& value) {
    const double number = Number(value);
    Require(std::abs(number) <= std::numeric_limits<float>::max(), "number is out of range");
    return static_cast<float>(number);
}
float Gain(const sol::object& value) {
    const double number = Number(value);
    Require(number >= 0 && number <= 1, "volume must be in [0,1]");
    return static_cast<float>(number);
}
bool Boolean(const sol::object& value) {
    Require(value.get_type() == sol::type::boolean, "expected a boolean");
    return value.as<bool>();
}
template<class T> T Enum(const sol::object& value, std::initializer_list<const char*> names) {
    const auto name = Text(value);
    int index = 0;
    for (const auto* candidate : names) {
        if (name == candidate) return static_cast<T>(index);
        ++index;
    }
    throw std::runtime_error("invalid audio enum: " + name);
}
AudioBus Bus(const sol::object& value) { return Enum<AudioBus>(value, {"Music", "SFX", "UI"}); }

AudioSubsystem* FindAudio(LuaBindingsContext& context) {
    return context.engine ? context.engine->GetSubsystem<AudioSubsystem>() : nullptr;
}
AudioSubsystem& CheckedSubsystem(LuaBindingsContext& context) {
    auto* audio = FindAudio(context);
    Require(audio != nullptr, "audio subsystem is unavailable");
    return *audio;
}
entt::entity Source(LuaBindingsContext& context, const sol::object& value) {
    Require(value.is<LuaEntityHandle>(), "expected an Entity handle");
    const auto& entity = value.as<const LuaEntityHandle&>();
    auto* scene = context.GetScene();
    Require(scene && entity.IsValid() && entity.BelongsTo(*scene), "entity is invalid or belongs to another scene");
    Require(scene->HasComponent<AudioSource>(entity.Resolve()), "AudioSource component is missing");
    return entity.Resolve();
}
AssetReference Clip(LuaBindingsContext& context, const sol::object& value) {
    const auto name = Text(value);
    // UUIDs remain authoritative, including missing references. Paths are accepted
    // only as authoring convenience and immediately converted to catalog identity.
    if (auto id = AssetUuid::Parse(name); id && id->IsValid()) return {*id, {}};
    Require(context.engine != nullptr, "asset catalog is unavailable");
    const auto* entry = context.engine->GetAssets().GetCatalog().FindPath(name);
    Require(entry && entry->metadata.kind == CatalogAssetKind::AudioClip, "expected an audio clip UUID or catalog path");
    return {entry->metadata.id, entry->path};
}
AudioPlayOptions Options(const sol::object& value, bool spatial) {
    AudioPlayOptions options;
    if (!Present(value)) return options;
    Require(value.get_type() == sol::type::table, "audio options must be a table");
    for (const auto& [key, field] : value.as<sol::table>()) {
        const auto name = Text(key);
        if (name == "loading") options.loading = Enum<AudioLoadingMode>(field, {"Buffered", "Streamed"});
        else if (name == "bus") options.bus = Bus(field);
        else if (name == "scope") {
            const auto scope = Text(field);
            Require(scope == "scene" || (!spatial && scope == "session"), "scope must be scene (or session for Play2D only)");
            options.scope = scope == "session" ? AudioScope::Session : AudioScope::Scene;
        } else if (name == "looping") options.looping = Boolean(field);
        else if (name == "volume") options.volume = Gain(field);
        else if (name == "pitch") options.pitch = Scalar(field);
        else if (name == "attenuation") options.attenuation = Enum<AudioAttenuation>(field, {"None", "Inverse", "Linear", "Exponential"});
        else if (name == "minDistance") options.minDistance = Scalar(field);
        else if (name == "maxDistance") options.maxDistance = Scalar(field);
        else if (name == "rolloff") options.rolloff = Scalar(field);
        else throw std::runtime_error("unknown audio option: " + name);
    }
    Require(options.pitch > 0, "pitch must be positive");
    Require(options.minDistance > 0 && options.maxDistance >= options.minDistance, "invalid audio distance bounds");
    Require(options.rolloff >= 0, "rolloff must be nonnegative");
    return options;
}
const char* Status(AudioVoiceStatus status) {
    switch (status) {
    case AudioVoiceStatus::Pending: return "Pending";
    case AudioVoiceStatus::Playing: return "Playing";
    case AudioVoiceStatus::Paused: return "Paused";
    case AudioVoiceStatus::Finished: return "Finished";
    case AudioVoiceStatus::Stopped: return "Stopped";
    case AudioVoiceStatus::Failed: return "Failed";
    default: return "Invalid";
    }
}
sol::object Arg(sol::variadic_args args, std::size_t index, sol::this_state state) {
    return index < args.size() ? sol::object(args[index]) : sol::make_object(state, sol::nil);
}
ValueResult VoiceResult(sol::this_state state, AudioVoiceHandle handle, std::string error) {
    if (!handle) return {sol::make_object(state, sol::nil), error.empty() ? "audio playback failed" : std::move(error)};
    return {sol::make_object(state, LuaAudioVoice{std::move(handle)}), {}};
}
ValueResult Play(LuaBindingsContext& context, sol::this_state state, sol::variadic_args args, bool spatial) {
    try {
        Require(args.size() >= (spatial ? 2u : 1u) && args.size() <= (spatial ? 3u : 2u), "incorrect playback arguments");
        const auto clip = Clip(context, Arg(args, 0, state));
        const auto options = Options(Arg(args, spatial ? 2 : 1, state), spatial);
        Vector3 position{};
        if (spatial) {
            const auto value = Arg(args, 1, state);
            Require(value.is<Vector3>(), "expected a finite Vector3 position");
            position = value.as<Vector3>();
            Require(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z), "position must be finite");
        }
        auto& audio = CheckedSubsystem(context);
        std::string error;
        auto handle = spatial ? audio.PlayAt(clip, position, options, error) : audio.Play2D(clip, options, error);
        return VoiceResult(state, std::move(handle), std::move(error));
    } catch (const std::exception& e) { return {sol::make_object(state, sol::nil), e.what()}; }
}

enum class VoiceCommand { Pause, Resume, Stop, Volume, Pitch, Fade };
Result Control(LuaBindingsContext& context, const LuaAudioVoice& voice, VoiceCommand command,
               sol::this_state state, sol::variadic_args args) {
    try {
        const auto count = command == VoiceCommand::Fade ? 2u : command >= VoiceCommand::Volume ? 1u : 0u;
        Require(args.size() >= count && args.size() <= count + (command == VoiceCommand::Fade ? 1u : 0u), "incorrect voice control arguments");
        float value = 0;
        double seconds = 0;
        bool stop = false;
        if (command == VoiceCommand::Volume || command == VoiceCommand::Fade) value = Gain(Arg(args, 0, state));
        if (command == VoiceCommand::Pitch) { value = Scalar(Arg(args, 0, state)); Require(value > 0, "pitch must be positive"); }
        if (command == VoiceCommand::Fade) {
            seconds = Number(Arg(args, 1, state)); Require(seconds >= 0, "fade duration must be nonnegative");
            if (args.size() == 3) stop = Boolean(Arg(args, 2, state));
        }
        Require(bool(voice.handle), "audio voice is invalid or expired");
        auto& audio = CheckedSubsystem(context);
        bool ok = false;
        switch (command) {
        case VoiceCommand::Pause: ok = audio.Pause(voice.handle); break;
        case VoiceCommand::Resume: ok = audio.Resume(voice.handle); break;
        case VoiceCommand::Stop: ok = audio.Stop(voice.handle); break;
        case VoiceCommand::Volume: ok = audio.SetVolume(voice.handle, value); break;
        case VoiceCommand::Pitch: ok = audio.SetPitch(voice.handle, value); break;
        case VoiceCommand::Fade: ok = audio.Fade(voice.handle, value, seconds, stop); break;
        }
        return {ok, ok ? "" : "audio voice cannot accept this operation"};
    } catch (const std::exception& e) { return {false, e.what()}; }
}
} // namespace

void RegisterAudioBindings(sol::state& lua, LuaBindingsContext& context) {
    auto voice = lua.new_usertype<LuaAudioVoice>("AudioVoice", sol::no_constructor);
    voice["IsValid"] = [&context](const LuaAudioVoice& v) {
        auto* audio = FindAudio(context);
        return bool(v.handle) && audio && audio->GetStatus(v.handle) != AudioVoiceStatus::Invalid;
    };
    voice["GetStatus"] = [&context](const LuaAudioVoice& v) {
        auto* audio = FindAudio(context);
        return std::string(Status(v.handle && audio ? audio->GetStatus(v.handle) : AudioVoiceStatus::Invalid));
    };
    voice["GetError"] = [&context](const LuaAudioVoice& v) {
        if (!v.handle) return std::string("audio voice is invalid or expired");
        auto* audio = FindAudio(context);
        return audio ? audio->GetVoiceError(v.handle) : std::string("audio subsystem is unavailable");
    };
    const auto bindControl = [&](const char* name, VoiceCommand command) {
        voice[name] = [&context, command](const LuaAudioVoice& v, sol::this_state state, sol::variadic_args args) {
            return Control(context, v, command, state, args);
        };
    };
    bindControl("Pause", VoiceCommand::Pause); bindControl("Resume", VoiceCommand::Resume);
    bindControl("Stop", VoiceCommand::Stop); bindControl("SetVolume", VoiceCommand::Volume);
    bindControl("SetPitch", VoiceCommand::Pitch); bindControl("Fade", VoiceCommand::Fade);

    auto audio = lua.create_named_table("Audio");
    audio.set_function("Play2D", [&context](sol::this_state state, sol::variadic_args args) { return Play(context, state, args, false); });
    audio.set_function("PlayAt", [&context](sol::this_state state, sol::variadic_args args) { return Play(context, state, args, true); });
    audio.set_function("PlaySource", [&context](sol::this_state state, sol::variadic_args args) -> ValueResult {
        try {
            Require(args.size() == 1, "PlaySource expects an Entity");
            const auto entity = Source(context, Arg(args, 0, state));
            auto& subsystem = CheckedSubsystem(context);
            std::string error;
            auto handle = subsystem.PlaySource(entity, error);
            return VoiceResult(state, std::move(handle), std::move(error));
        } catch (const std::exception& e) { return {sol::make_object(state, sol::nil), e.what()}; }
    });
    audio.set_function("GetSourceVoice", [&context](sol::this_state state, sol::variadic_args args) -> ValueResult {
        try {
            Require(args.size() == 1, "GetSourceVoice expects an Entity");
            const auto entity = Source(context, Arg(args, 0, state));
            return VoiceResult(state, CheckedSubsystem(context).GetSourceVoice(entity), "source has no voice");
        } catch (const std::exception& e) { return {sol::make_object(state, sol::nil), e.what()}; }
    });
    const auto sourceControl = [&](const char* name, int command) {
        audio.set_function(name, [&context, command](sol::this_state state, sol::variadic_args args) -> Result {
            try {
                Require(args.size() == 1, "source control expects an Entity");
                const auto entity = Source(context, Arg(args, 0, state));
                auto& subsystem = CheckedSubsystem(context);
                const bool ok = command == 0 ? subsystem.PauseSource(entity) : command == 1 ? subsystem.ResumeSource(entity) : subsystem.StopSource(entity);
                return {ok, ok ? "" : "source has no controllable voice"};
            } catch (const std::exception& e) { return {false, e.what()}; }
        });
    };
    sourceControl("PauseSource", 0); sourceControl("ResumeSource", 1); sourceControl("StopSource", 2);
    const auto gainControl = [&](const char* name, bool bus, bool set) {
        audio.set_function(name, [&context, bus, set](sol::this_state state, sol::variadic_args args) -> ValueResult {
            try {
                Require(args.size() == unsigned(bus) + unsigned(set), "incorrect gain arguments");
                const auto category = bus ? Bus(Arg(args, 0, state)) : AudioBus::SFX;
                const auto gain = set ? Gain(Arg(args, bus ? 1 : 0, state)) : 0.f;
                auto& subsystem = CheckedSubsystem(context);
                if (set) {
                    const bool ok = bus ? subsystem.SetBusVolume(category, gain) : subsystem.SetMasterVolume(gain);
                    return {sol::make_object(state, ok), ok ? "" : "audio gain operation failed"};
                }
                return {sol::make_object(state, bus ? subsystem.GetBusVolume(category) : subsystem.GetMasterVolume()), {}};
            } catch (const std::exception& e) {
                return {set ? sol::make_object(state, false) : sol::make_object(state, sol::nil), e.what()};
            }
        });
    };
    gainControl("SetMasterVolume", false, true); gainControl("GetMasterVolume", false, false);
    gainControl("SetBusVolume", true, true); gainControl("GetBusVolume", true, false);
    audio.set_function("IsAvailable", [&context] { auto* a = FindAudio(context); return a && a->IsAvailable(); });
    audio.set_function("GetDiagnostic", [&context] { auto* a = FindAudio(context); return a ? a->GetDiagnostic() : std::string("audio subsystem is unavailable"); });
}
} // namespace engine
