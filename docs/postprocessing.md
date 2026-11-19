# Lua fullscreen postprocessing

Fullscreen effects are runtime script-owned passes, not serialized scene settings.
Create their resources and register passes in a live script's `OnStart`, not at
module load or during teardown. A rendering subsystem and a started, live script
owner are required. Registration and resource creation happen outside rendering;
use the callback's context to issue draw commands.

## Shader assets and cooking

Place HLSL and a `.postprocess.json` definition in project content. Both need
their own adjacent `.meta` files, with kinds `shader` and `postProcess`,
respectively. Generate/check metadata as described in [scene-assets.md](scene-assets.md);
keep each sidecar with its source. The definition references the shader's **UUID**,
not its filename. Its optional path is only a diagnostic hint, never a fallback.
The definition itself also has a UUID and can be loaded by UUID or logical path.

For example, `content/effects/retro.postprocess.json`:

```json
{
  "version": 1,
  "shader": {
    "id": "11111111-1111-4111-8111-111111111111",
    "path": "game/effects/retro.hlsl"
  },
  "entryPoint": "PSMain",
  "defines": []
}
```

Replace the example UUID with the ID from `retro.hlsl.meta`. `entryPoint`
defaults to `PSMain`; optional `defines` entries are objects with string `name`
and `value` fields. The definition always describes a pixel shader variant.
Build Game / `sri-build` discovers and cooks catalogued postprocess definitions and
their shader variants; missing or wrong-kind shader references fail cooking.
See [building-games.md](building-games.md) for packaging.

## Minimal retro pass

This example quantizes color. Save the shader as `content/effects/retro.hlsl`:

```hlsl
Texture2D SceneColor : register(t0);
SamplerState PointClampSampler : register(s0);
cbuffer Retro : register(b5) {
    float Levels;
};

float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float4 color = SceneColor.SampleLevel(PointClampSampler, uv, 0);
    color.rgb = floor(color.rgb * Levels + 0.5) / Levels;
    return color;
}
```

Attach a script to a game object (or use an autoload):

```lua
local pass, params

function OnStart()
    local shader = Render.LoadShader("game/effects/retro.postprocess.json")
    params = Render.CreateConstantBuffer {
        shader = shader, name = "Retro", values = { Levels = 6.0 }
    }
    pass = Render.AddFullscreenPass {
        name = "Retro",
        stage = Render.Stage.AfterTonemap,
        order = 0,
        execute = function(ctx, frame)
            ctx:SetPixelShader(shader)
            ctx:SetRenderTarget(frame.outputColor)
            ctx:BindTexture(0, frame.inputColor)
            ctx:BindConstantBuffer(5, params)
            ctx:DrawFullscreen()
        end
    }
end
```

## Replacing the built-in tone mapper

Call `Render.SetBuiltinTonemappingEnabled(false)` in `OnStart` and register your
custom tone mapper at `BeforeTonemap`. Give it an `order` higher than your other
HDR effects (including bloom), so it maps their combined HDR result to `[0, 1]`.
For example, after creating `tonemap.postprocess.json` and its shader metadata:

```lua
function OnStart()
    local shader = Render.LoadShader("game/effects/tonemap.postprocess.json")
    Render.AddFullscreenPass {
        name = "Custom tonemap",
        stage = Render.Stage.BeforeTonemap,
        order = 1000,
        execute = function(ctx, frame)
            ctx:SetPixelShader(shader)
            ctx:SetRenderTarget(frame.outputColor)
            ctx:BindTexture(0, frame.inputColor)
            ctx:DrawFullscreen()
        end
    }
    Render.SetBuiltinTonemappingEnabled(false)
end
```

A simple exposure-based mapper for `content/effects/tonemap.hlsl`:

```hlsl
Texture2D SceneColor : register(t0);
SamplerState PointClampSampler : register(s0);

float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 hdr = max(SceneColor.SampleLevel(PointClampSampler, uv, 0).rgb, 0);
    return float4(1 - exp(-hdr * 1.0), 1); // Exposure = 1.0
}
```

The switch bypasses only Reinhard; presentation copies RGB without an additional
gamma conversion into the existing 8-bit UNORM target. Values outside `[0, 1]`
are clamped there. UI still composites afterward, and `AfterTonemap` / `AfterUI`
passes keep their usual positions. Non-final debug views are unaffected.

The setting belongs to the calling script. `true` releases **that script's**
bypass; the built-in mapper is enabled by default and whenever no live owner
requests a bypass. Repeated calls are idempotent. Owner teardown, Lua fault, or
session end releases the request automatically. Disabling or removing a pass
does not release it: pair that change with `Render.SetBuiltinTonemappingEnabled(true)`
if you want Reinhard restored. Call the switch outside render callbacks, such
as in `OnStart` or `OnUpdate`; calls during rendering or teardown are rejected.

## Scheduling and commands

Stages run in this order: `BeforeTonemap` on HDR scene color, `AfterTonemap`
after tonemapping and before UI, and `AfterUI` after UI composition. Within a
stage lower integer `order` runs first (default zero); ties run in registration
order. Each pass receives the preceding color as `frame.inputColor` and must
write `frame.outputColor`. Frame also provides `depth`, `normals`,
`inputWidth`, `inputHeight`, `outputWidth`, and `outputHeight`. Schedule changes
are reflected at frame boundaries. Removing a pass or retiring its owner also
cancels callbacks that have not yet run in the current frame.

`ctx:SetPixelShader(shader)`, `ctx:SetRenderTarget(texture)`,
`ctx:BindTexture(slot, texture)`, `ctx:BindConstantBuffer(slot, buffer)`, and
`ctx:DrawFullscreen()` explicitly specify the draw. Texture slots are pixel
register numbers `t0`–`t127`; bind every texture used by the shader, including
input color, depth, or normals. Common engine buffers at `b0` and `b1` and
engine samplers `s0`–`s4` are bound automatically; `b0`–`b4` are reserved.
Do not sample the render target being written. `ctx:ClearRenderTarget(texture,
{r, g, b, a})` clears a writable target (default zero), and
`local width, height = ctx:GetTextureSize(texture)` returns two numbers.

Load an additional texture during setup with
`Render.LoadTexture("game/textures/noise.png")` (or its catalog UUID), then bind
the returned handle with `ctx:BindTexture`. This blocking load retains a read-only
snapshot of the texture's GPU view until owner retirement. It survives scene
asset unloading; load it again to pick up an asset change.

For multi-draw effects, create intermediate targets during setup with
`Render.CreateRenderTarget { scale = 0.5, format = "RGBA16F" }` or fixed
`{ width = 320, height = 180, format = "RGBA8" }`. Omit dimensions and scale
for full viewport size. Scale is positive and viewport-relative, rounded up
with a minimum of one pixel; relative targets resize with the viewport. Fixed
width and height must both be positive integers and cannot be combined with
scale. Formats are `RGBA16F` (default), `RGBA8`, `R16F`, and `R8`. Draw to an
intermediate, then bind it as an input while drawing to a different target,
ultimately writing `frame.outputColor`.

## Parameters and lifetime

`Render.CreateConstantBuffer { shader = shader, name = "Retro", values = {...} }`
reflects a named HLSL cbuffer at `b5`–`b13`. Bind it at its declared register,
with the same pixel shader used to create it. Create a separate buffer for a
different shader, even when its fields have the same names.
`buffer:Set { Levels = 8 }` updates selected fields; `buffer:Set("Levels", 8)`
updates one. Updates are partial and atomic: invalid names, types, or shapes
reject the update without applying its other fields. Supported reflected fields
include boolean and numeric scalars, vectors, matrices, and arrays thereof.
Use dense 1-based numeric tables for vectors, matrices and arrays: matrices
are rows of columns, arrays add an outer dimension. A flat table of the exact
total component count is also accepted. Values must match the reflected type
and shape.

Keep shader, buffer, target, and pass handles only within their owning script
session. `pass:SetEnabled(false)` bypasses it; `pass:SetEnabled(true)` enables
it and clears its recorded error. `pass:Remove()` removes it. Owner teardown,
fault, or session end revokes callbacks and cleans up owned resources; handles
cannot be shared with another script owner or reused in a later session.
The context and all frame textures are borrowed for that execution only: never
save them for a later callback. Owned intermediate targets persist for the owner.

If a callback fails or does not write output, the pass is disabled and its input
is passed through; `pass:GetError()` reports the error while the owner remains
usable. Lua callback errors also fault the script owner, so do not assume that
re-enabling a pass will recover a faulted script. GPU device failures follow
the renderer's device-error path rather than normal pass bypass. Non-final
renderer debug views bypass these postprocess stages.
