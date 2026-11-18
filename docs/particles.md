# GPU particles

Add **Particle Emitter** to an entity in the Inspector. Assign a reusable effect with the **Effect** picker, or leave it unassigned to use inline emission and the procedural soft disc. The shipped `engine/particles/smoke.particle-effect.json` is referenced by `engine/scenes/smoke.scene.json` and the ExampleGame demo; the demo also includes a custom ring using its project shader hook. Appearance `0` is the standard textured particle. Legacy inline appearance `1` migrates to `0`; it is not a fire appearance, and effect definitions reject `1`. See [project particle hooks](particle-hooks.md) for IDs `2` and up.

Each birth starts at the entity's world position, moving along its local up axis. Already emitted particles stay in world space when the source moves. `rate` is particles per simulation second, `lifetime` is seconds, `speed` is world units per second, and `radius` is the billboard radius in world units. `spread` adds random unit-vector jitter to the launch direction before normalization; `seed` controls this variation. `tint` is straight linear RGB, including HDR values for emissive effects. Scene saves store the emitter settings; they do not store in-flight particles.

## Authoring and preview

In the Content Browser choose **New Particle Effect** to create an effect asset, then select it on an emitter. Its emission settings are shared; **Override effect emission** uses the emitter's inline max alive, rate, lifetime, speed, radius and spread instead. Seed and tint are always per emitter. Edit the effect's settings and texture in the asset editor.

The emitter Inspector's **Scene preview** runs in the viewport outside Play mode: **Play** toggles to **Pause**, while **Stop**, **Restart** and **Prewarm** control the preview. Prewarm simulates two seconds in bounded batches over several displayed frames, including while paused. Choose **Selected emitter** or **All emitters** and a preview speed from 0.01× to 4×. Preview state is transient and resets on Play or scene changes; neither it nor the live population is saved. Changing the effect or seed restarts an active preview; editing emission affects subsequent births. Saving an effect reloads it and restarts the active preview.

### Effect settings

Effect files are version 1 JSON with `version`, `texture` (null or an asset reference with UUID and path), and `settings`. Settings omitted from the object use engine defaults. The smoke asset is a complete example. `appearance`, `maxAlive`, `rate`, `lifetime`, `speed`, `radius` and `spread` control emission. `lifetimeVariation` and `speedVariation` are symmetric fractional ranges around their base values; lifetime variation must be less than 1, speed variation can reach 1. `acceleration` is a world-space vector (e.g. negative Y for gravity), and nonnegative `drag` slows velocity.

`sizeStart` and `sizeEnd` multiply the particle radius over normalized lifetime; `colorStart`/`colorEnd` and `opacityStart`/`opacityEnd` interpolate color and opacity. `fadeIn` is a fraction of lifetime, and `softDistance` controls soft intersection with opaque depth. Colors are nonnegative linear HDR, opacity and fade are in [0, 1]. `randomRotation` rotates the billboard texture, and `blend` is `"alpha"` or `"additive"`. `castShadows` and `shadowDensity` control directional particle shadows.

The effect texture picker selects a Texture2D by asset UUID. The selected image supplies color and alpha for Standard appearance and multiplies the output of Custom appearance hooks. Without a selected image, Standard uses the soft disc and Custom uses its own procedural shape. Arbitrary Texture2D assets are resampled into a fixed 64-layer, 256×256 linear RGBA16F array with one mip and bilinear filtering. Layer zero is the procedural soft-disc fallback; the other 63 layers accommodate external textures per renderer session. Layers are retained until reset, so changing textures does not immediately reclaim a layer.

### Shadows

Particles can cast directional-light transmittance shadows onto opaque scene surfaces using four 512×512 cascades. Shadow billboards face the light, and transmittance is clipped at the opaque receiver depth. Scene lighting bilinearly filters the transmittance map in each cascade and blends cascade transitions. Particles do not receive shadows or self-shadow; point and spot lights do not get particle shadows.

## Admission and telemetry

The CPU accumulates fractional births at the authored rate. The GPU updates and compacts survivors first, then admits births up to both the emitter's `maxAlive` and the renderer's global limit. Lowering `maxAlive` lets existing particles finish. Excess requests are discarded that step. When multiple emitters compete for the last slots, existing emitter-slot order determines admission. Removing an emitter stops births and lets its particles drain; a scene or session reset clears everything. Pause freezes birth, motion and age; resizing preserves the population.

```cpp
auto& renderer = engine.GetSubsystem<engine::RenderingSubsystem>()->GetRendering();
// Optional: changing budgets clears the current population. Returns false for invalid limits.
const bool configured = renderer.SetParticleBudget({.maxParticles = 16384, .maxEmitters = 256});
// Add a one-shot burst to an entity that has a ParticleEmitter (even one with rate = 0).
renderer.RequestParticleBurst(entt::to_integral(entity), 64);
const auto& stats = renderer.GetParticleStats(); // borrowed; query on the engine thread
```

Defaults are **16,384 live particles and 256 emitter slots**. Supported ranges are 1–65,536 and 1–4,096 respectively. `stats.alive`, `rejected`, and the per-owner rows describe one completed simulation step. `peakTileOccupancy` and `multiBatchTiles` describe one completed view. The samples have independent serials (`simulationStep` and `tileFrame`); zero means no sample yet. Readbacks never wait for the GPU and can skip steps under load. Don't use a delayed count to enforce gameplay rules.

## Tuning overdraw

The simulation dispatches from a compact live pool. For rendering, projected billboards are depth-sorted, then binned into every coarse screen region they overlap. Each 8×8 fine tile walks its region's candidates in sorted order. **64 particles is a batch size, not a cutoff**: dense tiles continue to a second or subsequent batch instead of developing square holes. Composition is into linear HDR after lighting, with a soft intersection against reverse-Z scene depth.

The global limit bounds storage, not frame time. If peak occupancy is high, reduce the overlapping emitters' rates, lifetimes, or screen coverage before raising the global budget. Profile the current textured and shadow-enabled renderer on the target device.

## Renderer integration

`ParticlePass` bridges engine-owned assets and shaders to private D3D11 helpers for simulation, texture resampling, tile composition, and light-view shadows. Those helpers use native calls for structured-buffer/UAV work and per-slice targets that `RenderingContext` does not currently wrap. Resources are held by `ComPtr`; returned views are borrowed. Each helper documents its clobbered bindings, and `RenderingSystem` clears native state and restores engine constant buffers/samplers after the particle shadow pass, including resetting the wrapper's input-layout cache. Ordinary passes continue using the engine wrappers.
