# Material assets: runtime and editor contract (JSON version 1)

The renderer's MaterialDesc, Material, MaterialInstance and GPU handles are
unchanged. The asset layer adds two disk-backed types:

| Asset value | Runtime field | Authoring field |
| --- | --- | --- |
| MaterialAsset | MaterialHandle material | MaterialDefinition definition |
| MaterialInstanceAsset | MaterialInstanceHandle instance | MaterialInstanceDefinition definition |

The definition keeps logical asset names and **sparse authored values**, not GPU
IDs or already-expanded defaults. Use it to populate and save an editor inspector.
Do not serialize Material, ShaderSet, MaterialParameters or raw handle IDs.

## Material file

For example, game/materials/stone.material.json:

~~~json
{
  "version": 1,
  "shaderFile": "game/Stone.hlsl",
  "hooks": ["GBufferVS", "GBufferPS", "ShadowVS"],
  "defaults": {
    "albedoTexture": "game/textures/stone.png",
    "specularColor": [0.25, 0.25, 0.25],
    "shininess": 32
  }
}
~~~

Version and hooks are required. Defaults and its individual fields are optional.
An empty hooks array selects the built-in shaders; shaderFile can then be omitted
and is not read. With any hook present, shaderFile is required.

All hook functions live in that **one** HLSL file. Only the declared stages are
compiled; the renderer falls back to its built-in stage for every undeclared hook:

| JSON hook | Compilation defines | Function |
| --- | --- | --- |
| GBufferVS | MATERIAL_GBUFFER_VS=1, MATERIAL_HAS_HOOK=1 | void PatchGBufferVertex(inout MaterialVertex vertex) |
| GBufferPS | MATERIAL_GBUFFER_PS=1, MATERIAL_HAS_HOOK=1 | void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) |
| ShadowVS | MATERIAL_SHADOW_VS=1, MATERIAL_HAS_HOOK=1 | void PatchShadowVertex(inout MaterialVertex vertex) |

Include engine/MaterialTemplate.hlsl and guard **each** function with its stage:

~~~hlsl
#include "engine/MaterialTemplate.hlsl"

#if defined(MATERIAL_GBUFFER_VS)
void PatchGBufferVertex(inout MaterialVertex vertex) {}
#elif defined(MATERIAL_GBUFFER_PS)
void PatchGBufferSurface(GBufferInput input, inout MaterialSurface surface) {
    surface.shininess = 64;
}
#elif defined(MATERIAL_SHADOW_VS)
void PatchShadowVertex(inout MaterialVertex vertex) {}
#endif
~~~

The stage defines choose the correct template types and entry point. Do not reference
PS-only types while compiling a VS. Unknown or duplicate hooks are errors.

## Material instance file

For example, game/materials/polished.material-instance.json:

~~~json
{
  "version": 1,
  "material": "game/materials/stone.material.json",
  "parameters": {
    "shininess": 96
  }
}
~~~

Version and material are required. Parameters is optional. Material must
reference a material, not another instance; instance chains are not supported.

At upload time the loader copies Material::defaultParameters, then replaces only
the fields present in parameters. The base material is never modified. In this
example the texture and specular color are inherited, while shininess is 96.
The runtime MaterialInstance::parameters is a resolved snapshot; the asset's
definition.parameters still contains only the authored shininess override.

| Authored field | Meaning |
| --- | --- |
| Missing albedoTexture | Inherit the base texture |
| "albedoTexture": "game/textures/other.png" | Load and use that texture |
| "albedoTexture": null | Clear the texture handle; the renderer uses its fallback texture |
| Missing specularColor / shininess | Inherit that field |
| "specularColor": [0, 0, 0] / "shininess": 0 | Explicit zero override, **not** inheritance |

For a material, absent defaults use the existing renderer defaults: no explicit
texture, specular color [0.25, 0.25, 0.25], shininess 32.
Colors have exactly three finite non-negative floats, shininess is a finite
non-negative float. The color override replaces the whole vector.

CustomBuffer is caller-owned GPU state at b8, so it has no JSON representation.
Set it at runtime through RenderingResourceManager::SetMaterialInstanceParameters
when the host owns such a buffer. Version 1 covers the current renderer's single
albedo slot and PBR fields; it does not introduce arbitrary uniforms/texture slots.

## Names and paths

JSON is UTF-8 (a BOM is accepted). All references are **logical names relative to
the executable's asset root**, independent of the current directory and of the
containing JSON file. Absolute paths, traversal (.. / .), empty path segments
and invalid filename characters are rejected.

* A material name without an extension gains .material.json.
* An instance name without an extension gains .material-instance.json.
* A name with an extension is used as given, including a plain .json name.
* Extensionless and explicitly suffixed requests share the same asset record.
  Names remain ASCII case-insensitive and normalize backslashes to forward slashes.
* Shader names follow the existing shader resolver:
  game/Stone.hlsl -> &lt;exe&gt;/game/shaders/Stone.hlsl;
  engine/Foo.hlsl -> &lt;exe&gt;/engine/shaders/Foo.hlsl.
* Texture references follow LoadTexture: an image filename is used directly;
  a folder name resolves to its texture.png.

For example, when saving game/materials/polished.material-instance.json, do not
write ../textures/stone.png. Write the logical reference game/textures/stone.png.
The editor is responsible for placing/exporting JSON and dependencies under the
runtime asset root. This change does not redefine the editor's project/content root.

Example JSON files in example_game/materials/ are copied to &lt;exe&gt;/game/materials
by ExampleGameAssets.

## Runtime API

~~~cpp
auto& assets = engine.GetAssets();
auto& resources = rendering.GetResourceManager();

// Boot-time blocking load:
auto material = assets.LoadMaterial("game/materials/default");
auto instance = assets.LoadMaterialInstance("game/materials/glossy");
if (instance) {
    drawItem.materialInstance = instance->instance;
}

// Streaming: never wait for a dependency on the main thread:
auto handle = assets.RequestMaterialInstance("game/materials/glossy");
// Later, after AssetSystem::Update (normally driven by Engine::Tick):
if (assets.IsReady(handle)) {
    auto loaded = assets.Peek(handle);
    drawItem.materialInstance = loaded->instance;
}

// A material also has the renderer's automatically-created default instance:
if (material) {
    auto defaultInstance = resources.GetDefaultMaterialInstance(material->material);
}
~~~

The existing GetState&lt;T&gt;, IsReady, Peek, deduplication and generational handle
contracts also apply to MaterialAsset and MaterialInstanceAsset.
As before, failure retires a record/handle: observe AssetFailedEvent for the
failure reason and explicitly request again to retry. Name lookup after retirement
returns Unloaded, and a stale handle never becomes readable after later retries.

CPU workers read/validate JSON and compile declared HLSL stages. The main thread
requests and waits **asynchronously** for referenced assets, uploads GPU resources
and publishes Ready only after every dependency is ready. Blocking Load* uses
the same pipeline but waits for those jobs explicitly. Events are the existing
AssetLoadedEvent / AssetFailedEvent, with new kinds Material and MaterialInstance.
The failing dependency and its parent each report failure. Failed shader compilation
or JSON/dependency loading does not call std::exit.

All registry calls are main-thread calls. GPU resources remain owned by
RenderingResourceManager; the asset values are thin records like existing mesh and
texture assets. Do not manually destroy resources obtained from the asset registry.
Eviction/hot reload remain outside the existing AssetSystem contract. A changed JSON
file is not automatically reloaded, and existing instances are not live bindings to
later default changes.

## Editor read/write API

Internally, nlohmann/json handles parsing and serialization, and
json-schema-validator checks the versioned document schema (fields, references,
hooks and parameter ranges). Both dependencies are managed through vcpkg.

The public engine/assets/MaterialDefinition.hpp has no JSON-library dependency:

~~~cpp
engine::MaterialInstanceDefinition edited;
std::string error;
if (!engine::ParseMaterialInstanceDefinition(fileBytes, edited, error)) {
    // Show error in the inspector.
}

edited.parameters.shininess = 80;       // create/update an override
edited.parameters.shininess.reset();   // remove the override, inherit again
edited.parameters.albedoTexture = "";  // serialize as JSON null (explicit clear)
edited.parameters.albedoTexture.reset(); // remove the override, inherit again

std::string fileBytesToSave;
if (engine::SerializeMaterialInstanceDefinition(edited, fileBytesToSave, error)) {
    // Save these UTF-8 bytes using the editor's file/project API.
}
~~~

The material equivalents are ParseMaterialDefinition / SerializeMaterialDefinition.
The serializer writes version 1, preserves sparse overrides and validates the result.
Both parsing and serialization leave the caller's output unchanged on failure.
Unknown fields, duplicate keys, unsupported versions and malformed values are errors
(including customBuffer); do not silently drop inspector data.

## Checks

Enable SRI_BUILD_TESTS=ON, build material_assets_tests, then:

~~~text
build/windows-msvc/bin/Debug/material_assets_tests.exe
build/windows-msvc/bin/Debug/material_assets_tests.exe --gtest_filter=MaterialDefinitions.*
~~~

GoogleTest is installed through vcpkg. Run the executable directly, optionally
using --gtest_filter to select tests.
MaterialDefinitions tests the codecs, null/absence semantics, malformed inputs
and UTF-8; AssetTable tests repeated failed-handle reuse. MaterialAssetsIntegration exercises a
hidden D3D11 surface, all/partial shader hooks, synchronous and asynchronous dependency
graphs, inheritance, deduplication, failures/retry and shutdown with pending work.
It requires the same D3D11 hardware/debug layer as the current renderer and reports
GTEST_SKIP() if that initialization is unavailable.
