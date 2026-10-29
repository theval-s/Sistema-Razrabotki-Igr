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
within the same file. The codec supports Transform, MeshRenderer, Camera and
DirectionalLight; it rejects unknown fields/components, duplicate keys, cycles,
invalid numbers, and bad active camera/light selections before scene switching.

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
ExampleGame loads `game/scenes/demo.scene.json` at startup for its file-backed
meshes, materials and sun; its procedural ground, enchanted cube and animated
point/spot lights remain host-owned. CMake copies the scene, sidecars and source
assets under the executable's `game/` directory.

Build the editor, ExampleGame and tests with the MSVC preset, then run
`asset_catalog_tests.exe`, `scene_document_tests.exe` and
`material_assets_tests.exe`. The scene codec tests verify that ExampleGame's
references resolve against checked-in metadata. GPU tests use the existing
D3D11 hidden-window setup.
