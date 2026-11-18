# Project particle hooks

To override particle behavior, add `shaders/ParticleHooks.hlsli` under the project's content root, normally `<project>/content/shaders/ParticleHooks.hlsli`. In a standalone loose layout using `game/`, use `game/shaders/ParticleHooks.hlsli` beside the executable. Both represent the same logical asset. There is one override per project; no metadata or engine source changes are needed. See [`example_game/shaders/ParticleHooks.hlsli`](../example_game/shaders/ParticleHooks.hlsli) for a complete appearance ID `2` example.

Implement all three functions. Unused simulation functions can have empty bodies. The engine supplies this type before including your file:

```hlsl
struct Particle {
    float4 positionAge;       // world position, age in seconds
    float4 velocityLifetime;  // world velocity, lifetime in seconds
    float4 colorSize;         // linear HDR tint, world-space radius
    uint4 meta;               // owner slot, appearance ID, identity/seed, reserved
};

void ParticleSpawn(inout Particle p);
void ParticleUpdate(inout Particle p, float dt);
float4 ParticleAppearance(Particle p, float2 uv, float life01);
```

- `ParticleSpawn` runs after the engine admits a birth and fills in emitter defaults.
- `ParticleUpdate` runs after the engine advances age and position for the frame. Set `p.positionAge.w >= p.velocityLifetime.w` to kill the particle before it enters the next live pool. Velocity changes affect subsequent integration steps.
- Spawn and update may change appearance ID. The engine restores the owner, identity/seed and reserved lane after either call. Allocation, caps, counters and buffer access are engine-owned. Keep particle values finite and use only plain functions and constants: do not declare resource bindings or depend on engine pass globals.
- Appearance IDs `0` and `1` use built-in shading. For IDs `2` and up, `ParticleAppearance` receives centered billboard coordinates (`uv` is typically in `[-1, 1]`, with `(0, 0)` at the center) and normalized age `life01`. Return **straight** linear HDR RGB and coverage alpha in `[0, 1]`; the renderer applies alpha as it composites the depth-ordered particles. Without an override, custom IDs use a soft radial fade.

Relative `#include` directives are resolved beside the file containing the directive; explicit `game/` and `engine/` shader paths also work. In development, restart the renderer/editor after adding, editing or removing the override to recompile the passes. A present override that fails to compile is reported as a shader error. `sri-build` compiles the same passes against the project's override and bakes them into the shader library. Both packed and loose cooked builds use that bytecode without reading HLSL; recook to ship changes.
