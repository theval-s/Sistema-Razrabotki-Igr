# Particles from a C++ host

Authoring, effect settings and admission rules are in the user guide, `docs/particles.md`. This page covers the renderer API a C++ host uses. Call it on the engine thread.

```cpp
auto& renderer = engine.GetSubsystem<engine::RenderingSubsystem>()->GetRendering();
// Optional: changing budgets clears the current population. Returns false for invalid limits.
const bool configured = renderer.SetParticleBudget({.maxParticles = 16384, .maxEmitters = 256});
// Add a one-shot burst to an entity that has a ParticleEmitter (even one with rate = 0).
renderer.RequestParticleBurst(entt::to_integral(entity), 64);
const auto& stats = renderer.GetParticleStats(); // borrowed
```

## Budgets

Defaults are **16,384 live particles and 256 emitter slots**. Supported ranges are 1–65,536 and 1–4,096 respectively. An emitter keeps its slot until its last particle dies, even after its entity is gone. Changing either limit clears every live particle.

## Bursts

`RequestParticleBurst(owner, count)` adds births to the emitter whose integral EnTT handle (including its version) is `owner`, on the next frame that advances the simulation. A burst for an entity with no extracted emitter that frame is dropped. Bursts obey the same `maxAlive` and global limits as regular births.

## Telemetry

`stats.alive`, `rejected`, and the per-owner rows describe one completed simulation step. `rejected` counts that step only, not a running total. `peakTileOccupancy` and `multiBatchTiles` describe one completed view. The samples have independent serials (`simulationStep` and `tileFrame`), starting at 1; zero means no sample yet. Readbacks never wait for the GPU and can skip steps under load. Don't use a delayed count to enforce gameplay rules.

## Editor preview

`SetParticlePreview`, `RestartParticlePreview` and `PrewarmParticlePreview` drive the editor's scene preview while the engine isn't playing. Preview time uses the frame's real delta scaled by `speed` (0 < speed ≤ 4). `ResetParticles` clears particles, pending bursts, preview state and telemetry; the engine calls it at scene and Play boundaries.
