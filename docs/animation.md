# Skeletal animation

SRI's first animation milestone imports skinned meshes and named clips from a
self-contained glTF 2.0 GLB. It uses ozz-animation 0.17 as a static, private
engine dependency; building requires CMake 3.30 or newer. Skeletons and clips can
also load headlessly, without a renderer.

## Authoring assets

1. Export a self-contained `.glb` with its skin and named animation clips. Use
   LINEAR translation, rotation and scale tracks. Prefer in-place clips: there is
   no root-motion extraction or retargeting.
2. In the Content Browser, use **Import Animated Rig...** from the Import menu,
   or choose a GLB through normal **Import...** and select **Animated Rig...**.
   Inspection runs asynchronously and lists clip names, durations and validation
   diagnostics. Unsupported clips cannot be selected; supported clips remain selectable.
3. Choose a new destination folder name and the clips to generate. Optionally
   extract the embedded baseColor texture and create an instance of the selected
   base material (the engine default is preselected). Unsupported base-color extraction
   is reported by inspection. Adjust other surface properties in the material-instance
   editor; the importer maps the base-color image into the chosen engine material.
   One material instance applies to the rig.
4. Set the initial entity placement scale, then optionally select **Add to Scene
   after import**. The scale affects the entity transform only; source skeletal
   transforms remain intact. Choose a placement scale appropriate to the source
   model's units and your scene.

The importer generates the source, descriptors, metadata and `rig.import.json` in
the dedicated folder. Existing destination names are rejected. Static models still
use **Static Model...** and the existing static import settings.

### Reimport and independent duplication

Select a rig folder or one of its owned assets and choose **Reimport Animated
Rig...**. Pick the replacement self-contained GLB. Each existing active clip has
an explicit mapping: keep its matching source name, remap to another supported
clip, or remove it while reserving its UUID. Missing names require a choice.
Additional clips can be selected as new assets. Existing asset UUIDs are retained.
Prepared single-source rigs without a manifest can be inferred when unambiguous.
Previously removed clips are listed separately as reserved identities. They default
to **Keep removed** even if the source name returns; explicitly choose **Restore as**
to reuse the old UUID. Selecting a new clip with the same name creates a new identity.
Group discovery validates structure without requiring a healthy current source, so
a changed or corrupt source can be repaired by choosing a valid replacement GLB.

Material edits and the existing texture are preserved by default. **Refresh embedded
baseColor texture** explicitly replaces the generated texture while preserving
material-instance edits. Reimport asks to save the current scene, including an
untitled scene, and offers to save other open files before publishing. Cancelling
either step cancels reimport. After commit the saved scene reloads through the
normal lifecycle, ending previews and refreshing loaded assets. A catalog rescan
alone is not an asset refresh. Publication failures are shown; recovery-required
failures include the retained transaction directory under the project's
`.sri/rig-transactions/`.
Successful commits that retain transaction data also show its path with the warning.

**Duplicate Animated Rig...** creates a separately owned rig in a newly named
folder, assigning new identities and remapping owned dependencies. External
materials and textures remain shared. This is a distinct operation from generic
file duplication. Duplicating a scene entity continues to share its rig assets.
Content authoring actions and scene placement are disabled during Play.

New generated groups use their owned material instance when placed. Prepared rigs
do not take ownership of adjacent materials or textures. When placing a skinned
descriptor or rig with no owned instance, the editor asks for an existing material
instance, preselecting the engine default when available. Cancelling leaves the
scene untouched. This entity assignment does not change rig ownership; generic
skinned descriptors remain supported without a group manifest.

Descriptors can also be authored manually with matching `.meta` UUIDs.
The checked-in TestBot files use these version 1 descriptors:

```json
// model.skeleton.json
{ "version": 1, "source": { "id": "b52e7e3e-920c-48b7-b782-8f403e061001", "path": "game/models/TestBot/model.glb" } }
```

```json
// model.skinned-mesh.json
{ "version": 1, "skeleton": { "id": "b52e7e3e-920c-48b7-b782-8f403e061002", "path": "game/models/TestBot/model.skeleton.json" } }
```

```json
// idle.animation.json
{ "version": 1, "skeleton": { "id": "b52e7e3e-920c-48b7-b782-8f403e061002", "path": "game/models/TestBot/model.skeleton.json" }, "clip": "idle" }
```

The comment labels above identify files; omit comments from actual JSON. IDs
come from the referenced assets' `.meta` files, and paths are logical asset paths.
The UUID is authoritative, not a fallback to the path. See
[scene assets](scene-assets.md) for metadata and project paths.

The importer preserves the whole source node hierarchy, up to 1024 nodes. Each
mesh section supports at most 256 palette joints; the four strongest vertex
weights are retained and normalized. One material instance applies to every
section. Skinning is performed by the engine before material hooks in all
passes; existing material schemas and hooks need no skinning additions.

Each asset is imported from the GLB on a CPU worker into immutable shared data.
Skeleton compatibility uses a signature of the full source, rejecting mismatched
revisions. Reload the scene after editing a source or descriptor outside the
reimport workflow. The build validation tool
checks descriptor compatibility and packages the source and descriptors; there
are no cooked ozz archives or disk import cache yet.

## Editor and example

Drag a `.skinned-mesh.json` descriptor into the scene, or add a Skinned Mesh
Renderer and Animator to an entity. Their inspectors expose mesh and material,
and skeleton, clip, speed, loop and play-on-start respectively. Animator asset
references may be null; configure the matching assets to play. Assigning a skinned
mesh in its inspector also infers its Animator skeleton in the same undo step.
Clip choices are restricted to compatible skeleton sources. Missing authored
references are preserved and diagnosed; source-revision mismatches are also
reported when loaded, including when rendering falls back to the rest pose.

In **Edit**, select an entity and click **Preview clip**. Only that entity's
transient player advances on real time; gameplay and other animators remain
frozen. Change the playback clip to crossfade, pause/resume, or seek with the
timeline or seconds field. The status displays current time and clip duration,
loading/transition state and errors. Transport speed and loop settings are
separate from the undoable authored starting speed, loop and play-on-start.
Changing transport loop takes effect when restarting or changing the clip.
**End preview** restores the authored starting clip at zero if play-on-start is
enabled, otherwise the rest pose. Selection changes, Animator edits/removal,
scene/project changes, entering Play, simulation reset and shutdown end preview.
In **Play**, the same transport controls runtime playback; pausing the game also
freezes animation. Playback state, clocks and fades never dirty the scene.

The viewport toolbar's **Show** menu turns editor overlays on and off. **Skeletons**
shows the selected loaded skeleton hierarchy, joint names and animated pose (or
compatible rest pose). **Audio Sources** shows scene emitter/listener markers.
Selecting a 3D emitter adds green minimum-distance and orange maximum-distance
spheres, measured in world units regardless of entity scale. Maximum distance is
an attenuation bound, not an audio culling radius.

For a working editor setup, copy `example_game/models/TestBot/` into the project's
`content/models/TestBot/` and `example_game/scenes/animation.scene.json` (with its
`.meta`) into `content/scenes/`, preserving all sidecars. Open that scene and
preview in Edit or enter Play mode. The Playback section's clip picker selects a
transient crossfade target without changing the authored starting clip.
Alternatively run `ExampleGame --animation`; press
**C** to toggle the first bot between idle and walk with a 0.35-second crossfade.
TestBot is an original CC0 fixture with `idle` and in-place `walk` clips; see
`example_game/models/TestBot/README.md` and `tools/generate_animation_fixture.py`
in the source repository.

## Lua controls

On an entity with an Animator, controls return `success, error` (an empty error
string on success):

```lua
local ok, err = entity:play_animation("game/models/TestBot/walk.animation.json", 0.35, true)
entity:pause_animation()       -- pause; pass false to resume
entity:seek_animation(0.5)    -- seconds
entity:set_animation_speed(1.0)
entity:stop_animation()
local state = entity:animation_state()
```

`play_animation(clipRef, fadeSeconds, loop)` accepts a clip UUID, logical path,
or `{id = "...", path = "..."}`; the table's ID is authoritative and its path is
optional. Fade defaults to zero and loop to true. Fade, seek time and speed must
be finite and nonnegative. Seek requires a loaded clip and no pending transition;
another fade is rejected while a transition is in progress. `animation_state()`
returns nil when unavailable, otherwise a detached snapshot with `ready`,
`playing`, `transitioning`, `time_seconds`, `duration`, and `error`. Loading can
complete after a successful play request, so inspect the state for readiness or
asynchronous errors.

## C++ API

Include `<engine/animation/AnimationComponents.hpp>` for `engine::Animator` and
`engine::SkinnedMeshRenderer`, and `<engine/animation/AnimationSubsystem.hpp>`
for the main-thread control API. Register `engine::AnimationSubsystem` with the
engine host before `RenderExtractionSubsystem`; `RegisterDefaultSubsystems` already
does this. `Animator` holds `skeleton`, `clip`, `speed`, `loop`, and
`playOnStart`; the renderer holds mesh and material references and handles.

`AnimationSubsystem` provides `Play(entt::entity, const AssetReference&, float
fadeSeconds, bool loop, std::string& error)`, `SetPaused(entity, bool)`,
`Stop(entity)`, `Seek(entity, double seconds, std::string& error)`,
`SetSpeed(entity, float speed, std::string& error)`, `GetPose(entity)`, and
`GetPlaybackState(entity)`. Play starts at zero once loaded; snapshots own their
memory. `AnimationPose` exposes a skeleton signature and joint model matrices;
`AnimationPlaybackState` exposes readiness, playback/transition flags, time,
duration and error, plus the current `clip`, pending `targetClip`, runtime `speed`
and `loop`. The inspector displays those runtime values so scripted transitions
can be distinguished from the authored starting configuration and selected
transport target. The CPU runtime sampling layout and ozz types remain private.
`BuildSkinningPalette` uses the rest pose for a missing or incompatible pose;
`SkinnedMeshBounds` computes asset-local bounds.

Editor hosts can call `Preview(entity, clip, fadeSeconds, loop, error)` in Edit
mode, then use `SetPaused`, `Seek` and `SetSpeed` on the transient player.
`GetPreviewEntity()` identifies the single current audition, or `entt::null`.
`EndPreview()` restores the normal Edit pose. Preview rejects standalone and Play
hosts; engine scene/reset lifecycle clears it, and the editor ends it on selection
and project changes. Returned pose snapshots retain their own memory after ending
or replacing preview.

## Build and test

From the repository root with the normal Windows prerequisites and submodules:

```powershell
cmake --preset windows-vs2022 -DSRI_BUILD_TESTS=ON
cmake --build --preset windows-debug --target animation_tests rendering_skinning_tests ExampleGame
ctest --test-dir build/windows-vs2022 -C Debug -R '^(animation_tests|rendering_skinning_tests)$' --output-on-failure
```

`animation_tests` also covers rig transactions and the complete import → preview →
reimport → independent-duplication workflow. Set `SRI_TEST_RIG_SOURCE` to a supported
GLB to run the same integration check against a production asset. The check reads
that source and creates a disposable temporary project; it never edits the supplied
asset or its project. For example:

```powershell
$env:SRI_TEST_RIG_SOURCE = "C:\Projects\MyGame\content\models\character.glb"
ctest --test-dir build/windows-vs2022 -C Debug -R '^animation_tests$' --output-on-failure
```

Qt import/reimport dialogs are covered by `content_browser_drop_tests`. Audio
audition and mixer widgets are covered by `audio_editor_tests`.

`viewport_overlay_tests` checks label compositing with GPU pixel readback, including
movement, clearing, transparency and DPI changes. Set `SRI_TEST_PROJECT` to an
existing `.sriproject` with an animated startup scene to also check the combined
project/engine catalog and native DX11 scene rendering. This check reads the project
without saving it.
