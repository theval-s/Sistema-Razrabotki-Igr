# Scene and asset files (code-only API)

Scene JSON uses stable entity UUIDs and versioned component objects. Meshes,
material instances and material dependencies are referenced by the UUID in an
adjacent file sidecar; the human-readable path is a hint, not a fallback.

## Metadata

For `game/models/table/model.obj`, create `model.obj.meta` next to it:

~~~json
{ "version": 1, "id": "a4669b8d-a6df-46d0-b19b-d491854db8e0", "kind": "mesh", "centerOnLoad": false }
~~~

Valid kinds: `mesh`, `texture`, `shader`, `material`, `materialInstance`,
`scene`. `centerOnLoad` is only valid for meshes. The generator preserves
existing sidecars and creates new UUIDs for recognized files lacking them:

~~~text
sri-asset-meta generate <project-content-directory>
sri-asset-meta check <project-content-directory>
~~~

Run separately for the shipped engine asset directory when authoring engine
assets. Keep `.meta` with its source when moving it; do not copy one sidecar to
another asset. A catalog scan rejects missing/orphaned metadata, duplicate UUIDs,
wrong kinds and case-insensitive path collisions. Engine built-ins and ExampleGame
assets include checked-in sidecars.

## Scene JSON

Store a scene under the game root as `<name>.scene.json` and give it its own
`.meta`. The following illustrates the version 1 component fields:

~~~json
{
  "version": 1,
  "activeCamera": "28b432ad-835e-4442-a766-6a510c2cae43",
  "directionalLight": null,
  "entities": [
    {
      "id": "28b432ad-835e-4442-a766-6a510c2cae43",
      "name": "Camera",
      "parent": null,
      "components": {
        "Transform": { "position": [0, 4, 9], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
        "Camera": { "fovY": 1.04719755, "nearPlane": 0.1, "farPlane": 100 }
      }
    },
    {
      "id": "bd52dd11-2d8b-435a-839f-e44ec4b1b7ad",
      "name": "Cube",
      "parent": null,
      "components": {
        "Transform": { "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
        "MeshRenderer": {
          "mesh": { "id": "a1051591-1cee-4520-a41d-0b282d7103b4", "path": "engine/primitives/cube/model.obj" },
          "materialInstance": { "id": "8d3e822a-2c06-4660-8f75-12396a136929", "path": "engine/primitives/default.material-instance.json" }
        }
      }
    }
  ]
}
~~~

The cube and material UUIDs above match checked-in engine sidecars. Entity UUIDs
are illustrative and unique within the scene. Parent values refer to entity UUIDs
within the same file. The codec also supports `Bounds`, `Controller` and
`GamePersistence` (below);
it rejects unknown fields/components, duplicate keys, cycles, invalid numbers,
and bad active camera/light selections before scene switching.

## Code

~~~cpp
engine::Engine engine(config);
engine.AddSubsystem<engine::RenderingSubsystem>();
engine.AddSubsystem<engine::RenderExtractionSubsystem>();
if (!engine.Initialize()) return;

std::string error;
if (!engine.LoadScene("game/scenes/start.scene.json", error)) {
    // Report syntax or structural errors. The previous scene is still active.
}
// Or resolve a scene by its own checked-in .meta UUID (rename-stable):
engine.LoadScene(engine::AssetReference{"2a9a7998-f44d-44d8-8fc3-57e78da18796",
                                        "game/scenes/demo.scene.json"}, error);
// Game logic can add components through Scene::AddComponent<T>().
if (!engine.SaveScene("game/scenes/start.scene.json", error)) {
    // Report unresolvable authored references or filesystem errors.
}
~~~

`LoadScene` parses without GPU work, then clears the old scene and its scene render
items, drains its outstanding asset requests, destroys its asset-backed GPU resources,
and requests assets needed by the new scene asynchronously. Render extraction
skips mesh renderers until both assets are ready. Old asset handles become stale
after a switch. `SaveScene` preserves authored UUID references even when an asset
is missing; renderers added programmatically without such references need handles
resolvable in the catalog. It writes a temporary file beside the destination
and replaces the destination on success. For a newly authored scene, run the
metadata generator and reload the catalog (or restart the engine) before
referencing that scene by UUID.

Use `engine.GetSceneAssets()` for additional assets owned by the active scene;
it is a separate AssetSystem instance from `engine.GetAssets()` (which keeps
host-owned assets across scene switches). Each scene registry deduplicates its
own requests and tracks dependencies; `LoadScene` evicts only the scene instance.

`RenderWorld::sceneItems` are replaced by extraction every frame, while
`RenderWorld::renderItems` remain available for host-owned procedural draws.
ExampleGame loads `game/scenes/demo.scene.json` for its file-backed meshes,
materials, textures and sun, including the ground, enchanted cube and visible
light markers. The cube's shader reads engine simulation time from the frame
buffer. The host still animates point/spot lights in `RenderWorld::lights` and
updates matching scene entity transforms for the visible markers. Sign and
hammer motion use registered native scene controllers. CMake copies the scene,
sidecars and source assets under the executable's `game/` directory.

## Names, tags, controllers, and queries

Names need not be unique. `Scene::FindFirstByName`, `FindAllByName` and
`FindWithTag` return EnTT handles in depth-first root/child order. Use UUIDs for
persistent links; check handles after a scene switch. Optional entity `tags` are
an array of distinct, nonempty strings, for example `"tags": ["animated"]`.
Use `AddTag`/`RemoveTag` to author them in code.

One optional `Controller` component is supported per entity:

~~~json
"Controller": { "type": "example/sign", "version": 1, "enabled": true, "data": 0.3 }
~~~

`data` can be any JSON value; the engine imposes no fixed controller payload
schema. Register a stable type name, version, optional side-effect-free validator,
and factory with `Scene::RegisterController` before loading. The validator receives
an engine `StateValue` (typed objects, arrays, numbers and strings), without raw
JSON parsing in controller code. `LoadAuthored(const StateValue&)` runs before
`OnCreate`. An
unavailable type/version or invalid registered payload is diagnosed in `LoadScene`'s
`error` output **even when it returns true**. That entity still loads; its authored
controller definition is retained and saved unchanged. Registering a missing type
later retries its definitions. Invalid structure in the fixed controller envelope
instead fails the whole parse before switching. No Lua runtime is required.

`OnCreate` runs after attachment, `OnEnable` when the scene is active and the
controller enabled, `Update(deltaSeconds)` before render extraction on unpaused
frames, then `OnDisable` and `OnDestroy` before its entity disappears. Mutations
to controllers and entity deletion requested in callbacks are deferred until after
callbacks. Controllers added during an update start on the next update. Controller
runtime fields are not written to the scene: the stored authored `data` changes
through `Scene::SetControllerAuthoredData` or an explicit
`Scene::CommitControllerAuthored` (calls `Controller::SaveAuthored`).
`SetControllerEnabled` authors the enabled bit. Scene switching
from an update callback is deferred until before render extraction.

## Playthrough saves

`SaveScene` writes authored starting configuration. `SaveGame` writes a separate
`*.save.json` artifact against the **catalog-backed scene UUID**; `LoadGame` loads
that scene by UUID before applying the playthrough changes. For example:

~~~cpp
scene.AddComponent<engine::GamePersistence>(npc, engine::GamePersistence{true, true});
engine.SaveGame("game/example.save.json", error);
engine.LoadGame("game/example.save.json", error);
~~~

`GamePersistence` is an optional authored scene component with `transform` and
`controllerState` boolean fields. Only opted-in existing entities get state
records. The engine captures/restores transforms independently of controllers;
controllers opt into live state with `SaveGameState(StateValue&)` and
`LoadGameState(const StateValue&)`. These callbacks never change the authored
controller payload. Unavailable controllers retain both their scene definition
and saved game state for later registration. Persistent runtime-spawned entities
carry authored snapshots; deletion markers name authored UUIDs removed during
play. UUIDs must stay unique across both sets. Spawn parent UUIDs are stored
separately, allowing parents from either set. Missing parent references, cycles,
duplicate IDs, type/version mismatches and invalid JSON fail validation before
the old scene is replaced. Missing assets still follow normal scene loading rules.
Controllers receive authored data and `OnCreate`, then saved transforms and
controller state, then `OnEnable` and the first update. `LoadGame` can succeed
with nonempty diagnostics for unavailable controllers. Game saves are atomic
replacements; save files do not need `.meta` sidecars. ExampleGame uses **F5** to
save, **F9** to restore, and **R** to raycast from the camera.

An optional `Bounds` component adds local-space min/max arrays for non-rendered
entities, e.g. `"Bounds": { "min": [-1,-1,-1], "max": [1,1,1] }`.
`QuerySphere(scene, sceneAssets, center, radius)` and
`RaycastAll(scene, sceneAssets, origin, direction, maxDistance)` scan mesh AABBs
when the mesh is ready, otherwise explicit Bounds if present; they transform all
eight corners into world space, including parents and rotation. Ray direction is
normalized internally; hits are sorted nearest-first and report nonnegative world
distance. Both queries include boundary hits. They are broad-phase AABB queries,
not triangle-accurate tests.

## Per-entity material edits

`Engine::EditEntityMaterial(entity, edit, error)` accepts a callback that changes
individual fields of a resolved `MaterialParameters` copy. The base material
instance must be ready. On first success it creates a scene-owned per-entity
instance clone; later edits reuse it. `SetEntityAlbedoTexture` accepts a ready
texture handle requested from `GetSceneAssets()`. The clone is released on entity
or renderer removal, scene switch and shutdown, before the base scene assets.
`SaveScene` writes only the original authored material-instance UUID, never GPU
handles or runtime edits. Caller-supplied custom constant buffers remain
caller-owned.

Build the editor, ExampleGame and tests with the MSVC preset, then run
`asset_catalog_tests.exe`, `scene_document_tests.exe` and
`material_assets_tests.exe`. The scene codec tests verify that ExampleGame's
references resolve against checked-in metadata. GPU tests use the existing
D3D11 hidden-window setup.
