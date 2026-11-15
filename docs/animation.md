# Skeletal animation

SRI's first animation milestone imports skinned meshes and named clips from a
self-contained glTF 2.0 GLB. It uses ozz-animation 0.17 as a static, private
engine dependency; building requires CMake 3.30 or newer. Skeletons and clips can
also load headlessly, without a renderer.

## Authoring assets

1. Export a self-contained `.glb` with its skin and named animation clips. Use
   LINEAR translation, rotation and scale tracks. Prefer in-place clips: there is
   no root-motion extraction or retargeting.
2. Place it under the project's `content/` directory. Run
   `sri-asset-meta generate <project-content-directory>` to create its sidecar.
   The GLB must have kind `mesh` and `centerOnLoad: false`; check its `.meta`.
3. Manually author a `.skeleton.json` referring to the GLB, a
   `.skinned-mesh.json` referring to that skeleton, and one `.animation.json`
   per clip, referring to the same skeleton and naming the clip exactly. Run
   `sri-asset-meta generate <project-content-directory>` again for their
   sidecars, then `sri-asset-meta check <project-content-directory>`. Use the
   generated UUIDs in references. The content browser has no rig import wizard.

For example, the checked-in TestBot files use these version 1 descriptors:

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
revisions. Reload the scene after editing a source or descriptor; these asset
types do not yet support live reimport. The build validation tool
checks descriptor compatibility and packages the source and descriptors; there
are no cooked ozz archives or disk import cache yet.

## Editor and example

Drag a `.skinned-mesh.json` descriptor into the scene, or add a Skinned Mesh
Renderer and Animator to an entity. Their inspectors expose mesh and material,
and skeleton, clip, speed, loop and play-on-start respectively. Animator asset
references may be null; configure the matching assets to play. Inspector playback
controls include play, crossfade, pause/resume, stop and seek. Playback state,
clocks and fades are transient, not scene-authored data; changing the component
restarts playback from its authored configuration.

For a working editor setup, copy `example_game/models/TestBot/` into the project's
`content/models/TestBot/` and `example_game/scenes/animation.scene.json` (with its
`.meta`) into `content/scenes/`, preserving all sidecars. Open that scene and enter
Play mode. The Playback section's clip picker selects a transient crossfade target
without changing the authored starting clip. Alternatively run `ExampleGame --animation`; press
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
duration and error. The CPU runtime sampling layout and ozz types remain private.
`BuildSkinningPalette` uses the rest pose for a missing or incompatible pose;
`SkinnedMeshBounds` computes asset-local bounds.

## Build and test

From the repository root with the normal Windows prerequisites and submodules:

```powershell
cmake --preset windows-vs2022 -DSRI_BUILD_TESTS=ON
cmake --build --preset windows-debug --target animation_tests rendering_skinning_tests ExampleGame
ctest --test-dir build/windows-vs2022 -C Debug -R '^(animation_tests|rendering_skinning_tests)$' --output-on-failure
```
