# GPU particles

Add **Particle Emitter** to an entity in the Inspector. Assign a reusable effect with the **Effect** picker, or leave it unassigned to use inline emission and the procedural soft disc. The engine ships `engine/particles/smoke.particle-effect.json`; the ExampleGame demo uses it, plus a custom ring drawn by its project shader hook. Appearance `0` is the standard textured particle, `1` is reserved, and IDs `2` and up are project hooks: see [project particle hooks](particle-hooks.md).

Each birth starts at the entity's world position, moving along its local up axis. Already emitted particles stay in world space when the source moves. `rate` is particles per simulation second, `lifetime` is seconds, `speed` is world units per second, and `radius` is the billboard radius in world units. `spread` adds random unit-vector jitter to the launch direction before normalization; `seed` controls this variation. `tint` is straight linear RGB, including HDR values for emissive effects. Scene saves store the emitter settings; they do not store in-flight particles.

Lua has no particle API yet: an emitter emits whenever its entity has the component and a nonzero rate.

## Authoring and preview

In the Content Browser choose **New Particle Effect** to create an effect asset, then select it on an emitter. Its emission settings are shared; **Override effect emission** uses the emitter's inline max alive, rate, lifetime, speed, radius and spread instead. Seed and tint are always per emitter. Edit the effect's settings and texture in the asset editor.

The emitter Inspector's **Scene preview** runs in the viewport outside Play mode: **Play** toggles to **Pause**, while **Stop**, **Restart** and **Prewarm** control the preview. Prewarm simulates two seconds in bounded batches over several displayed frames, including while paused. Choose **Selected emitter** or **All emitters** and a preview speed from 0.01× to 4×. Preview state is transient and resets on Play or scene changes; neither it nor the live population is saved. Changing the effect or seed restarts an active preview; editing emission affects subsequent births. Saving an effect reloads it and restarts the active preview.

### Effect settings

Effect files are version 1 JSON with `version`, `texture` (null or an asset reference with UUID and path), and `settings`. Settings omitted from the object use engine defaults. The smoke asset is a complete example. `appearance`, `maxAlive`, `rate`, `lifetime`, `speed`, `radius` and `spread` control emission. `lifetimeVariation` and `speedVariation` are symmetric fractional ranges around their base values; lifetime variation must be less than 1, speed variation can reach 1. `acceleration` is a world-space vector (e.g. negative Y for gravity), and nonnegative `drag` slows velocity.

`sizeStart` and `sizeEnd` multiply the particle radius over normalized lifetime; `colorStart`/`colorEnd` and `opacityStart`/`opacityEnd` interpolate color and opacity. `fadeIn` is a fraction of lifetime, and `softDistance` controls soft intersection with opaque depth. Colors are nonnegative linear HDR, opacity and fade are in [0, 1]. `randomRotation` rotates the billboard texture, and `blend` is `"alpha"` or `"additive"`. `castShadows` and `shadowDensity` control directional particle shadows.

The effect texture picker selects a Texture2D by asset UUID. The selected image supplies color and alpha for Standard appearance and multiplies the output of Custom appearance hooks. Without a selected image, Standard uses the soft disc and Custom uses its own procedural shape. Arbitrary Texture2D assets are resampled into a fixed 64-layer, 256×256 linear RGBA16F array with one mip and bilinear filtering. Layer zero is the procedural soft-disc fallback; the other 63 layers accommodate external textures per renderer session. Layers are retained until reset, so changing textures does not immediately reclaim a layer.

### Shadows

Particles can cast directional-light transmittance shadows onto opaque scene surfaces using four 512×512 cascades. Shadow billboards face the light, and transmittance is clipped at the opaque receiver depth. Scene lighting bilinearly filters the transmittance map in each cascade and blends cascade transitions. Particles do not receive shadows or self-shadow; point and spot lights do not get particle shadows.

## Budgets and admission

The CPU accumulates fractional births at the authored rate. The GPU updates and compacts survivors first, then admits births up to both the emitter's `maxAlive` and the renderer's global limit. Lowering `maxAlive` lets existing particles finish. Excess requests are discarded that step. When multiple emitters compete for the last slots, existing emitter-slot order determines admission. Removing an emitter stops births and lets its particles drain; a scene or session reset clears everything. Pause freezes birth, motion and age; resizing preserves the population.

The renderer holds at most **16,384 live particles across 256 emitter slots**. A C++ host can change these limits, request bursts and read particle telemetry; the engine SDK documents that API in `sdk/particles.md`.

## Tuning overdraw

The simulation dispatches from a compact live pool. For rendering, projected billboards are depth-sorted, then binned into every coarse screen region they overlap. Each 8×8 fine tile walks its region's candidates in sorted order. **64 particles is a batch size, not a cutoff**: dense tiles continue to a second or subsequent batch instead of developing square holes. Composition is into linear HDR after lighting, with a soft intersection against reverse-Z scene depth. Particles aren't lit by scene lights, so author their colours for the scene's exposure: grey smoke stays as bright in a dark room as in daylight.

The global limit bounds storage, not frame time. If dense, overlapping effects are slow, reduce the overlapping emitters' rates, lifetimes, or screen coverage before raising the global budget. Profile the current textured and shadow-enabled renderer on the target device.
