# Rendering diagnostics API contract

For the editor workflow and debug-view interpretation, see [rendering diagnostics](rendering-diagnostics.md).

## Ownership and lifecycle

`RenderingSystem::GetDiagnostics()` and `RenderingSubsystem::GetDiagnostics()` return an owned `RenderingDiagnostics` value. Its strings, native filesystem path and optional failure survive renderer shutdown or destruction. Access to the renderer and lifecycle methods is engine-thread-only; a copied snapshot does not make renderer access thread-safe. `RenderingSystem::GetResources()` is a low-level borrowed `const RenderingResources&`, tied to the system and its current resource lifecycle; do not retain it across shutdown, reinitialization or destruction as a diagnostics snapshot.

| Lifecycle | Meaning |
| --- | --- |
| `Uninitialized` | No active renderer. |
| `Ready` | Required resources and passes are ready. |
| `SurfaceUnavailable` | Surface temporarily unavailable, including zero extent; GPU work is suspended. |
| `Failed` | A rendering operation failed; the first failure is latched. |

Initialization with a missing window fails; a window with zero extent returns false with `SurfaceUnavailable` before GPU startup. The host must retry full engine initialization when the initial surface becomes drawable: resize restoration applies only after successful initialization. A successfully initialized surface can suspend and resume on the same window. A different window requires shutdown and initialization.

Shutdown preserves a latched failure; a fresh explicit initialization attempt clears it. After a runtime failure the host must shut down and initialize again, rather than retry frames or resize for recovery. Automatic device recovery is not provided. Partial startup cleans itself up; engine shutdown quiesces asset workers before releasing their GPU resources.

`RenderingFailure` owns operation, pass, summary, detail, path, optional native error and device-removal reason, and optional shader stage, resolved entry, target and defines. The original failure is preserved through cleanup; a removal reason supplements the initiating error. `RenderingException::GetFailure()` provides the structured details; `what()` is only the summary. Asset loading and catalog failures are separate from renderer lifecycle failures.

## Caller migration

`ShaderContext::CompileCpu` and `CompileSource` return `CompiledShader`; `CompileShader` and `FinalizeOnDevice` return `ShaderContext`. These are concrete values rather than optionals: catch `RenderingException` at the appropriate host or importer boundary and use `GetFailure()` for full diagnostics. `CompiledShader` retains source and variant context through device finalization. Asset importers translate failures into their existing asset error boundary; an individual asset failure does not by itself mark the renderer failed.

`GetDebugView()` / `SetDebugView()` are available on both system and subsystem. The setter returns false for an invalid enum without changing the selection; valid selections require no resource recreation. Fresh initialization resets to `Final`; surface suspend/restore retains the selection. The editor enables its selector only when ready.
