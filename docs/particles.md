# GPU particles

In the Inspector, add **Particle Emitter** to an entity and press Play. Appearance `0` is smoke; `1` is fire. The default ExampleGame scene has both. The reusable `engine/scenes/smoke.scene.json` and `engine/scenes/fire.scene.json` assets each contain one emitter with its own settings. See [project particle hooks](particle-hooks.md) for custom simulation and shading.

Each birth starts at the entity's world position, moving along its local up axis. Already emitted particles stay in world space when the source moves. `rate` is particles per simulation second, `lifetime` is seconds, `speed` is world units per second, and `radius` is the billboard radius in world units. `spread` adds random unit-vector jitter to the launch direction before normalization; `seed` controls this variation. `tint` is straight linear RGB, including HDR values for emissive effects. Scene saves store the emitter settings; they do not store in-flight particles.

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

The global limit bounds storage, not frame time. At 1080p on a GeForce RTX 5070 Ti, the tile-rendering pass alone took about **1.6 ms median** for 16,384 billboards of 5-pixel radius spread over a grid. With the same count at 8-pixel radius and centers packed into a 16×16-pixel region, it took **6.4 ms** (15 GPU timestamp samples, shipping shader optimization). Target-device results will differ. If peak occupancy is high, reduce the overlapping emitters' rates, lifetimes, or screen coverage before raising the global budget.
