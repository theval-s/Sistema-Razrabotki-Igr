# Audio: clips, sources, listeners and mixing

Audio is engine-owned through `AudioSubsystem` (miniaudio internally). Public C++
headers are `engine/audio/AudioSubsystem.hpp` and `AudioComponents.hpp`; gameplay
and Lua never own backend pointers. Calls are main-thread only, including offline
mixing. See [ExampleGame](../example_game/README.md) in the source repository for
generated CC0 clips and a complete camera/emitter/menu demonstration.

## Assets and supported formats

- WAV, FLAC and MP3 use the built-in decoders. OGG/Vorbis and Opus are not supported.
- **ThreeD requires a mono clip.** Stereo/multichannel clips are rejected for point
  emitters; author a separate mono file. There is no automatic downmix.
- TwoD supports stereo music, ambience and UI sounds without spatial processing.
- Buffered (default) asynchronously decodes short clips and shares decoded resources
  between independent voices. Streamed uses bounded buffers and is selected explicitly
  for long music/ambience. Looping and streaming are independent options.

Place audio under project `content/`, for example `content/audio/impact.wav`, addressed
as `game/audio/impact.wav`. Every clip has an adjacent `impact.wav.meta`:

```json
{ "version": 1, "id": "375c7852-3506-4aba-a2bc-2006c5472001", "kind": "audioClip" }
```

Run `sri-asset-meta generate <content-directory>` then `check` for new assets. Move
sidecars with their files. Scene references store `{ "id": "UUID", "path": "game/..." }`;
UUID is authoritative and path is only a diagnostic hint. A missing UUID reference
is preserved on save. Catalog resolution does not mean decoding has completed:
playback can still be Pending or fail later.

## Editor authoring

1. Add **AudioSource** to an entity through Add Component. Pick a catalogued audio
   clip; use mono for ThreeD. An unassigned source is valid and silent.
2. Choose Buffered/Streamed, Music/SFX/UI, looping, volume and pitch. Enable
   **Play On Start** for automatic playback on activation during Play.
3. For ThreeD choose attenuation and distances in metres. Parent the entity normally;
   the emitter uses its world transform, including animated/physics parents.
4. Add one enabled **AudioListener**, usually on the gameplay camera or its rig,
   or rely on the active scene camera fallback. Inspect listener diagnostics if silent.
5. Explicitly Preview a clip in Edit to audition it in 2D. Stop ends audition;
   replacement, scene/project changes and entering Play also clear preview.
6. Save, enter Play, and test movement and rotation. Stop restores authored scene
   state; playback handles/cursors are never written into the scene or save game.

Scene sources do not autoplay in Edit. Inspector edits use scene history for undo/redo.
The editor free camera is not the gameplay listener during Play. Preview is one
audition at a time, separate from paused game audio.

## Components and units

`AudioSource` stores these authored values (C++ enum spellings and Lua strings agree):

Scene JSON uses component keys `AudioSource` and `AudioListener`, with lowercase
enum strings: `buffered`/`streamed`, `music`/`sfx`/`ui`, `2d`/`3d`, and
`none`/`inverse`/`linear`/`exponential`. Lua component type names remain
`AudioSource`/`AudioListener`; see the [scene JSON example](scene-assets.md#audio-components).

| Field | Default / constraint |
|---|---|
| `clip` | Unassigned optional AssetReference; Lua uses UUID/path string |
| `enabled`, `playOnStart`, `looping` | true, false, false |
| `loading`, `bus` | Buffered, SFX |
| `volume`, `pitch` | 1, 1; gain in [0,1], pitch finite and positive |
| `spatial` | ThreeD (or TwoD) |
| `attenuation` | Inverse; also None, Linear, Exponential |
| `minDistance`, `maxDistance` | 1 m, 100 m; finite, min > 0, max >= min |
| `rolloff` | 1; finite and nonnegative |

One scene unit is **one metre**, +Y is up and local -Z is forward. Spatial poses are
synchronized after simulation in LateUpdate using world matrices. Listener orientation
is normalized/orthogonalized; transform scale does not rescale authored distances.
Ordinary 3D is panning plus attenuation, without HRTF, geometry occlusion or reverb.

**maxDistance is not a hard cull/silence radius.** It limits the attenuation curve's
distance calculation; depending on the model, audible gain can remain beyond it.
Use explicit Stop or gameplay distance policy when a hard boundary is needed.

`AudioListener` only has `enabled` (default true). Selection is deterministic:

1. Exactly one enabled explicit listener takes precedence over the active camera.
2. With none, use the active scene camera.
3. With neither, spatial output is suppressed; TwoD continues.

Multiple enabled explicit listeners are rejected by scene validation. Runtime-created
conflicts diagnose and suppress spatial output until corrected, rather than choosing
an arbitrary entity. `GetEffectiveListener()` and `GetListenerDiagnostic()` expose
the decision to native hosts. A host using only a procedural render camera should
sync an explicit listener to it, as ExampleGame does.

## C++ playback

Register before scripting in a custom host (default subsystem registration includes
audio). Initialize the engine and load a catalogued scene before requesting its clips:

```cpp
#include <engine/audio/AudioSubsystem.hpp>

// Before Engine::Initialize(), in a manually assembled host:
engine.AddSubsystem<engine::AudioSubsystem>();
// Add ScriptSubsystem after audio, if needed.

// After initialization/catalog loading:
auto* audio = engine.GetSubsystem<engine::AudioSubsystem>();
engine::AudioPlayOptions options;
options.bus = engine::AudioBus::Music;
options.loading = engine::AudioLoadingMode::Streamed;
options.scope = engine::AudioScope::Session;
options.looping = true;
options.volume = 0.3f;
std::string error;
auto music = audio->Play2D(engine::AssetReference{
    "375c7852-3506-4aba-a2bc-2006c5472003", "game/audio/ambience-stereo.wav"}, options, error);
if (!music) { /* report error */ }
// Poll GetStatus(music) and GetVoiceError(music) for asynchronous failure.
```

`PlayAt(clip, worldPosition, options, error)` creates a fixed-position mono one-shot;
use scene scope. `PlaySource(entity, error)` uses the entity's authored AudioSource
and follows it; repeating Play restarts its one primary voice. `GetSourceVoice`,
`PauseSource`, `ResumeSource`, `StopSource` control that primary voice. Independent
Play2D/PlayAt calls overlap and clean themselves up on completion.

Handle controls: `Pause`, `Resume`, `Stop`, `SetVolume`, `SetPitch`, and
`Fade(voice, targetVolume, seconds, stopAtEnd)`. Fade uses playback time and freezes
while paused; `stopAtEnd=true` reclaims playback at fade completion. Master and bus
volume compose with per-voice gain. Zero gain is mute, not pause. Set gains with
`SetMasterVolume` / `SetBusVolume` and read them with corresponding getters.

## Lua playback and components

Audio functions use **dot calls**; voice methods use **colon calls**. Playback returns
`voice, error` or `nil, error`; controls return `ok, error`. GetMasterVolume and
GetBusVolume return `number, error` or `nil, error`. Diagnostic/status methods return
their value directly. Handle errors without aborting gameplay when audio is optional:

```lua
local impact = "375c7852-3506-4aba-a2bc-2006c5472001"
local voice, message = Audio.PlayAt(impact, Vector3.new(0, 2, -4), {
    bus = "SFX", volume = 0.7, minDistance = 2, maxDistance = 30
})
if not voice then Log.warning(message) end

local music, err = Audio.Play2D("game/audio/ambience-stereo.wav", {
    loading = "Streamed", bus = "Music", scope = "session", looping = true
})
if music then
    music:SetVolume(0.3)
    -- music:Pause(); music:Resume(); music:Fade(0, 1.5, true)
else
    Log.warning(err)
end
Audio.SetMasterVolume(0.75)
Audio.SetBusVolume("Music", 0.5)
```

Clip strings accept UUID or catalog path; paths resolve immediately to catalog identity.
Options are `loading`, `bus`, `scope`, `looping`, `volume`, `pitch`, `attenuation`,
`minDistance`, `maxDistance`, `rolloff`. Enum strings are case-sensitive. Scope is
lowercase `"scene"` (default), or `"session"` for Play2D only. Unknown options, wrong
types and nonfinite/invalid numbers return errors. Preview scope is native/editor only.

```lua
-- entity is an existing Entity handle in the current scene.
if not entity:HasComponent("AudioSource") then entity:AddComponent("AudioSource") end
entity:SetComponent("AudioSource", {
    clip = "game/audio/orbit-mono.wav", playOnStart = false,
    looping = true, spatial = "ThreeD", bus = "SFX", minDistance = 2
})
local loop, message = Audio.PlaySource(entity)
if not loop then Log.warning(message) end
-- Audio.PauseSource(entity), ResumeSource(entity), StopSource(entity)
-- Audio.GetSourceVoice(entity) -> voice, error (nil when there is no primary voice)
```

Component validation errors raise Lua errors, as in other Entity component methods.
Use the existing Entity component API to AddComponent before SetComponent when absent.
GetComponent returns a snapshot; apply edits through SetComponent. AudioListener uses
`{ enabled = true }`. For AudioSource, omitted/nil `clip` preserves the reference;
`clip = ""` clears it; a valid missing UUID remains authorable. Removing/disable of
a source stops its following primary voice. Clip replacement cancels the old voice;
explicitly Play the replacement or activate the component again. Live gain/pitch,
distance and transform edits do not restart playback.

## Status, ownership and pause

`voice:GetStatus()` (C++ `GetStatus`) returns:

| Status | Meaning |
|---|---|
| Pending | Accepted, resource preparation/start not completed |
| Playing | Active playback |
| Paused | Cursor frozen (explicitly or by game-pause policy) |
| Finished | Natural end; backend voice/resources released |
| Stopped | Explicit stop or lifecycle cancellation; resources released |
| Failed | Decode/start failure; inspect `GetError()` / `GetVoiceError()` |
| Invalid | Empty, foreign or expired subsystem token |

Terminal status remains queryable while the originating subsystem lifetime exists.
`IsValid()` is a token/lifetime check, **not** a test for Playing; use status. Dropping
a C++ handle or Lua userdata does not stop fire-and-forget audio. Terminal tokens
cannot control a new/reused voice. Shutdown/reinitialization makes old tokens stale.
The default cap is 128 live voices, counting Pending and Paused; overflow is rejected
with a diagnostic instead of stealing a voice.

| Scope | Lifetime |
|---|---|
| Scene (default) | Reset/replacement, session Stop, or shutdown |
| Session (explicit, independent TwoD only) | Survives level changes; ends on session Stop/shutdown |
| Preview (editor) | Explicit stop/replacement, scene/project switch, entering Play, shutdown |

Entity-following sources always belong to the scene. PlayAt is scene-owned but
independent of the entity that triggered it; deleting that entity does not end it.
Stopping while Pending cancels its start. Play-on-start runs once per activation;
Stop consumes that intent, so a finished/stopped voice is not resurrected every frame.
Disabling stops playback; reenabling creates a new activation eligible for autoplay.

Scope controls lifetime; **bus controls pause**. Game pause freezes Music/SFX cursors
(including newly ready sounds), but UI and editor preview remain usable. A voice
explicitly paused by gameplay stays paused after the game resumes. Muting does not
freeze time. Scene teardown rejects new old-scene requests; startup requests in
Lua `OnStart` and native controller `OnCreate`/`OnEnable` requests survive session
startup. Stop clears Scene/Session playback without
editing authored source data; saves/snapshots contain no voices or playback positions.

**LoadGame restarts the audio session**, including session-owned music: it ends the
old session and begins a new one before replacement scripts start. This differs from
an ordinary level change, which preserves session-owned TwoD playback. Restart music
from Lua `OnStart` (as ExampleGame does); no saved cursor is restored, and retaining
an old voice token does not resume that playback. Lua update callbacks are `OnUpdate`
and `OnFixedUpdate`; `OnCreate`/`OnEnable` are native controller hooks, not Lua callbacks.

## Devices, disabled and offline operation

`AudioConfig.output` defaults to `AudioOutputMode::Device`, opening the default output
with device-negotiated format. Missing hardware/device loss does not prevent engine
startup. `IsAvailable()` and `GetDiagnostic()` (also `Audio.IsAvailable()` and
`Audio.GetDiagnostic()` in Lua) explain availability. Playback can return an immediate
error or fail after Pending; poll terminal status for decoding failures.

Native hosts can call `RetryDevice(error)` on the main thread at a safe point.
Retry invalidates existing voices; request fresh playback afterwards. Automatic
backoff/recovery and cursor restoration are not promised. Lua has no retry binding.

Choose `AudioOutputMode::Disabled` when constructing AudioSubsystem to intentionally
disable audio: no device is opened and playback reports unavailable; the rest of the
game can run. It is distinct from master gain zero and from game pause.

For device-free verification choose `AudioOutputMode::Offline` and optional
`AudioConfig.sampleRate` (default 48000). `MixOffline(std::span<float>, error)` fills
interleaved stereo float samples and pumps jobs deterministically; pass an even
sample count, two floats per frame. No audible device is opened. Offline mixing is
main-thread only and is not available in Device/Disabled mode. Use normal scene
updates for source/listener synchronization. A successful offline check does not
verify real-device hotplug behavior or perceived spatial balance.

## Packaging and listening checks

Ship complete audio files plus `.meta` sidecars, including streamed content (streams
need seekable files for the lifetime of playback). `sri-build` recursively copies
project content into `game/`, so no audio extension allowlist is needed. ExampleGame's
asset-copy and install rules include its complete `audio/` directory. The common
deploy helper copies vcpkg notices, including miniaudio; docs directory installation
includes this guide in editor and SDK packages.

Before release, run outside the source tree: approach/retreat from an emitter, rotate
the listener, hear its parent move, overlap effects/music, test pause and independent
bus gains, repeated Play/Stop and scene changes, streaming loops, non-ASCII paths,
and device unplug/switch followed by explicit retry. Listen for pops, underruns and
loop seams. These are manual hardware/listening checks, separate from offline tests.
