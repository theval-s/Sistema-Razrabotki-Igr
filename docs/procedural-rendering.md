# Lua procedural rendering

Procedural passes draw geometry generated in a vertex shader from
`SV_VertexID` and `SV_InstanceID`. They use triangle lists, no vertex buffers
and no face culling. This is useful for small custom batches without creating
mesh assets or adding engine-specific entities. There is no raw buffer or mesh
API here.

## Pipeline assets

Place an HLSL shader and a `.pipeline.json` definition in project content.
Give each an adjacent `.meta` sidecar: kind `shader` for HLSL and
`renderPipeline` for the pipeline. See [scene assets](scene-assets.md) for
metadata and UUIDs. The pipeline's shader UUID identifies one HLSL asset for
both stages; `path` is a diagnostic hint, not a fallback. For example:

```json
{
  "version": 1,
  "shader": {
    "id": "11111111-1111-4111-8111-111111111111",
    "path": "game/effects/billboards.hlsl"
  },
  "vertexEntryPoint": "VSMain",
  "pixelEntryPoint": "PSMain",
  "defines": [],
  "blend": "Opaque",
  "depth": "ReadWrite"
}
```

Replace the UUID with the shader sidecar's ID. Entry points default to
`VSMain` and `PSMain`; optional `defines` are `{ "name": "...", "value":
"..." }` string pairs. Blend defaults to `Opaque`, and may also be `Alpha`
(straight alpha) or `Additive`. Additive sums source and destination RGB
without alpha weighting and preserves destination alpha; multiply RGB by
alpha in the shader if desired. Depth defaults to `ReadWrite`; `ReadOnly`
performs the reverse-Z hardware depth test without writing depth, and
`Disabled` disables depth testing. For cutouts, use `clip()` in the pixel
shader with `Opaque` and `ReadWrite`.

Build Game / `sri-build` discovers these definitions and cooks both shader
entry points, including their defines and includes. Missing or wrong-kind
shader UUIDs fail validation.

### Editing in the editor

Double-click a `.pipeline.json` asset in the Content Browser (or choose
**Edit...**) to edit its shader, vertex/pixel entry points, defines, blend and
depth settings. **New > Render Pipeline...** creates a definition and its
metadata sidecar. The shader picker accepts shader assets and drag-and-drop.
An unresolved shader UUID is shown and preserved until you choose a replacement.
Saving an existing pipeline preserves its asset UUID.

Edit pipelines outside Play, then start Play again to load the updated settings.
The dialog validates the definition; shader compilation and stage compatibility
are checked when the pipeline is loaded.

## Two camera-facing cutouts

Save the following as `content/effects/billboards.hlsl`. The `EngineView`
declaration matches the engine's `Common.hlsl` layout. Positions are world
space; expanding in view space makes the quads face the camera. The vector
arrays are separate because reflected struct arrays are not supported.

```hlsl
#pragma pack_matrix(row_major)
struct EngineViewData {
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
    float4x4 invViewProj;
    float3 cameraPos;
    float2 viewportSize;
};
cbuffer EngineView : register(b1) { EngineViewData View; };
cbuffer Billboards : register(b5) {
    float4 CentersAndSize[2]; // xyz world center, w half-size
    float4 Colors[2];
};
Texture2D Image : register(t0);
SamplerState LinearClampSampler : register(s1);

struct VertexOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VertexOut VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)
    };
    float2 corner = corners[vertex];
    float4 center = CentersAndSize[instance];
    float4 viewPos = mul(float4(center.xyz, 1), View.viewMatrix);
    viewPos.xy += corner * center.w;
    VertexOut result;
    result.position = mul(viewPos, View.projectionMatrix);
    result.uv = float2((corner.x + 1) * 0.5, (1 - corner.y) * 0.5);
    result.color = Colors[instance];
    return result;
}

float4 PSMain(VertexOut input) : SV_Target {
    float4 color = Image.Sample(LinearClampSampler, input.uv) * input.color;
    clip(color.a - 0.5);
    return color;
}
```

Create the pipeline above as `content/effects/billboards.pipeline.json`, and
provide a catalogued texture at `content/textures/billboard.png`. Attach this
script to a game object or use an autoload:

```lua
local pipeline, image, params, pass

function OnStart()
    pipeline = Render.LoadPipeline("game/effects/billboards.pipeline.json")
    image = Render.LoadTexture("game/textures/billboard.png")
    params = Render.CreateConstantBuffer {
        shader = pipeline.vertexShader, name = "Billboards",
        values = {
            CentersAndSize = { { -1, 1, -4, 0.5 }, { 1, 1, -4, 0.75 } },
            Colors = { { 1, 1, 1, 1 }, { 1, 0.7, 0.5, 1 } }
        }
    }
    pass = Render.AddProceduralPass {
        name = "Billboards", order = 0,
        execute = function(ctx, frame)
            ctx:SetPipeline(pipeline)
            ctx:BindTexture(0, image) -- pixel stage by default
            ctx:BindConstantBuffer(5, params)
            ctx:DrawProcedural(6, 2)
        end
    }
end
```

Place the centers in front of the active camera for your scene. Change the
texture and positions as needed. Each reflected array update supplies the
whole array as a dense 1-based Lua table; this approach is intended for small
batches. For example, `params:Set { Colors = { {1, 1, 1, 1}, {1, 0, 0, 1} } }`
updates both colors. Reflected scalars, vectors, matrices and arrays use the
same conventions as [postprocessing parameters](postprocessing.md#parameters-and-lifetime).

## Scheduling, commands and lifetime

Procedural passes run only in the Final debug view: after opaque geometry and
sky, before particles, HDR postprocessing, tone mapping and UI. Lower `order`
runs first (default zero); ties run in registration order. The engine chooses
the scene color target, depth target and viewport;
the callback cannot change them. `frame` contains only `width` and `height`.
Depth writes are visible to particles and later depth sampling, but these draws
do not write G-buffer normals or cast shadows. There is no sorting guarantee
across particles or translucent batches.

`Render.LoadPipeline(uuidOrPath)` returns a pipeline with read-only
`vertexShader` and `pixelShader` handles. Create each custom constant buffer
from the shader stage that declares it, at `b5`–`b13`, and bind its declared
slot with `ctx:BindConstantBuffer(slot, buffer)`; the stage is inferred from
the reflected shader. If both stages use a custom cbuffer, create and update
a separate buffer for each stage, then bind both. Buffers must come from the
active pipeline's exact stage shaders. Frame `b0`, camera `b1` and engine samplers `s0`–`s4`
are bound automatically to both stages; their buffer layouts must match
`Common.hlsl`. Registers `b2`–`b4` are forbidden in procedural shaders.
Bind textures with
`ctx:BindTexture(slot, texture, "Pixel")` or `"Vertex"`; the stage defaults
to `"Pixel"`. `ctx:GetTextureSize(texture)` returns width and height.
`ctx:DrawProcedural(vertexCount, instanceCount)` defaults the instance count
to one; either count being zero is a no-op. Nonempty draws require a vertex
count divisible by three. Keep the instance count within the arrays declared
by your shader; the engine cannot infer shader indexing limits.

Create resources and register passes during setup, such as `OnStart`.
`Render.LoadTexture` loads a texture by UUID or logical path. Pass handles offer
`Remove()`, `SetEnabled(boolean)` and `GetError()`, as for fullscreen passes.
Owner stop or fault releases its resources and callbacks; handles cannot be
shared between owners or sessions. The context expires at the end of its
callback. If a callback faults, its pass is disabled and the Lua fault retires
the owner; draws already issued by that callback remain in the frame.
