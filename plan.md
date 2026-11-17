# Next milestone: rendering diagnostics and debug views

## Goal and agreed scope

Implement these two features:

1. **Clear rendering failure reporting:** explain renderer startup failures and
   common reasons that scene objects are not being drawn, with actionable details
   in the editor and its existing log.
2. **Render debug views:** a viewport selector for **Final**, **Albedo**,
   **Normals**, and **Depth**, using the existing deferred-rendering outputs.

The user selected these as a relatively contained engine-quality milestone.
Clip editing, animation controllers, a general project-validation panel, a full
profiler, shader hot reload, and automatic graphics-device recovery are separate
work. Keep this implementation focused on diagnosis and reliable presentation.

The user permits subagents. Use Astra or do the design yourself when shaping
public engine APIs; implementation and tests can be delegated after agreeing on
the contracts. Inspect the working tree and applicable guidance before editing.
Preserve unrelated changes. Commit only if explicitly requested.

## Important context: the viewport regression that was just fixed

The editor's `ViewportWidget` is a native HWND with `WA_PaintOnScreen` and a DXGI
flip-discard swap chain. Qt-painted child widgets over that surface caused a
black viewport and accumulating text trails.

The fix already in the codebase:

- `editor/src/viewport_labels.cpp` rasterizes text offscreen and composites it
  with DX11 SpriteBatch.
- `RenderingSubsystem::SetHostOverlayCallbacks()` renders host diagnostics after
  the scene and before the engine UI, into the same target before the one Present.
- Skeleton/Audio controls live in the sibling toolbar above the native viewport.

**Do not put a QWidget/QLabel/QPainter overlay over the native viewport.** A status
banner must be a sibling in its container layout, or replace the viewport area
when rendering is unavailable. The fatal-error UI must work without a renderer,
so use ordinary Qt UI outside the native surface for that case.

The combined Test1/engine asset catalog passed validation during the investigation.
The recent regression was a presentation/compositing issue. Still report catalog
errors accurately: duplicate metadata, shader compilation, and presentation
failures are distinct causes and should retain their original diagnostic details.

## Code map and verified starting points

### Renderer lifecycle and error paths

- `engine/src/rendering/RenderingSystem.cpp`
  - `Initialize()` currently calls a void `InitializePasses()` and then sets
    `ready_ = true`.
  - `Reinitialize()` already checks pass resize results, but some implementations
    currently return success without validating the newly created resources.
- `engine/include/engine/rendering/passes/RenderPass.hpp`
  - Initial `Initialize()` returns `void`; resize hooks return `bool`.
- `engine/src/rendering/passes/`
  - Inspect GBuffer, shadow, light, selection-mask, and present passes together.
  - `PresentPass::Initialize()` currently exits on back-buffer-view failure.
- `engine/src/rendering/RenderingResourceManager.cpp`
  - `CompileShader()` calls `std::exit` on compilation failure.
  - `checkResult()` calls `std::exit` on resource-creation failures.
- `engine/src/rendering/resources/ShaderContext.cpp`
  - Shader APIs return optionals and log errors; preserve compiler output, source
    filename, shader stage/entry point, and relevant defines when propagating errors.
- `engine/src/rendering/RenderingContext.cpp`, `GraphicalState.cpp`, and
  `RenderingResources.cpp`
  - Audit device/swap-chain, state, buffer, texture/view, resize, and Present results.
- `engine/src/rendering/RenderingSubsystem.cpp`
  - Coordinates surface changes, rendering, overlays, and per-frame debug cleanup.
- `engine/src/engine.cpp`
  - `Engine::Initialize()` unwinds previously initialized subsystems on failure.
    The subsystem whose initialization fails must clean up its own partial work.

### Editor and existing diagnostics

- `editor/src/main_window.cpp`: startup, viewport container/toolbar, engine tick,
  status updates, and Log dock. A failed startup does not start the engine tick timer.
- `editor/core/include/editor/viewport_tool_bar.hpp`: existing Left/Center/Right
  sections for controls outside the native render surface.
- `editor/src/log_panel.cpp`, `log_model.cpp`: existing channel/severity filtering
  and full-message details. Reuse these instead of creating another log console.
- `editor/src/main.cpp::addFileLog()` already installs a rotating file sink at
  `<editor executable directory>/logs/editor.log` before modules/engine startup.
  It currently converts the filename through the local code page.
- `engine/include/engine/core/Log.hpp`: shared logging channels and `AddFileSink()`.
- `engine/include/engine/assets/AssetEvents.hpp`: existing loaded/failed events.
- `engine/include/engine/assets/AssetSystem.hpp`: typed asset states and catalog
  diagnostics; inspect these before adding any new status API.

### Debug-view inputs

- `RenderingResources::gBuffer` already contains albedo, normal, material, depth,
  and lit-result textures.
- `GBufferPass.cpp`: albedo is RGBA8 UNORM, normals RGBA16 float, and depth has an
  R32 float SRV over a typeless depth texture.
- `GBufferVS.hlsl` outputs world-space normals. `GBufferPS.hlsl` stores normalized
  normals encoded as `normal * 0.5 + 0.5`.
- `PresentPass.cpp` and `engine/shaders/PresentPass.hlsl` are the natural place to
  choose the displayed source. Final currently uses Reinhard tone mapping.
- Depth is **reverse-Z**. Check `FPSCamera.cpp`, `Common.hlsl`, and depth-state/
  clear code before writing visualization math; do not use a conventional-Z formula.

## Part 1 — clear rendering failure reporting

### 1. Establish a small engine-owned diagnostics contract

Expose an owned, read-only status snapshot through the rendering subsystem. The
exact names are a design decision, but consumers need:

- Lifecycle/readiness: not initialized, ready, surface temporarily unavailable,
  or failed.
- Failure operation/pass, a concise summary, and detailed text.
- Shader/resource path and native error code when applicable.

Keep Qt types out of engine headers. Make ownership/lifetime explicit. Preserve
the initiating failure through cleanup so the editor can query it after
`Engine::Initialize()` returns false. Clear obsolete failures only at a defined
new attempt or successful recovery boundary.

Represent scene/loading information separately from fatal renderer status.
An empty scene, missing scene camera, or one failed mesh is not proof of a failed
graphics pipeline. Prefer existing asset state/events over a second asset database.

### 2. Make initialization failures recoverable by the host

- Propagate pass initialization success/failure all the way to
  `RenderingSystem::Initialize()`, `RenderingSubsystem`, and the editor.
- Set renderer readiness only after every required resource/pass is usable.
- Replace process termination in the relevant rendering startup/resource paths
  with a consistent recoverable error mechanism. Update callers together.
  Simply returning an empty handle while callers continue drawing is insufficient.
- Preserve the original shader compiler message or HRESULT, plus which operation
  and resource failed. Do not reconstruct errors by parsing formatted log messages.
- Unwind partial initialization safely. Destroying a partially initialized pass,
  calling shutdown after failure, and cleaning up after a failed resize must be safe.
- Audit required constant buffers, textures, RTV/SRV/DSV views, pipeline states,
  shaders, and back-buffer views, not just device creation.
- Keep frame rendering guarded when initialization/resize fails. Minimized or
  zero-sized surfaces are a normal suspended condition, not a fatal error.
- Check presentation failures. If a device is removed/reset, report the result
  and removal reason where available and stop unsafe rendering. Automatic recovery
  is not required for this milestone.
- Emit logs on meaningful state changes, not the same error every frame.

Changing return types or adding error/result overloads is acceptable after auditing
all callers and tests. Keep the design narrow; a renderer-wide exception/error
framework is not itself the objective.

### 3. Surface actionable status in the editor

Add a compact status area outside the native viewport. It must be populated
immediately on startup failure, even when the engine tick timer never starts.

Suggested priority/behavior:

| Situation | Presentation |
|---|---|
| Device, required shader, pass, or resource failure | Persistent error with failing operation/path and Show Log |
| Invalid catalog or duplicate UUID | Specific catalog error, including implicated paths |
| Scene renderer assets still preparing | Nonfatal loading status |
| Referenced mesh/material failed | Warning with asset identity/path and detailed cause |
| No drawable scene items after loading settles | Informational empty-scene status |
| No scene camera in Edit | Recognize the valid editor-camera fallback; do not report renderer failure |
| No scene camera in Play | Explain the existing fallback behavior accurately |
| Healthy rendering | Compact/hidden status rather than a permanent warning |

Use requested scene-renderer assets, not every asset in the project, for loading
and failure summaries. Distinguish submitted drawable items from pixels actually
visible on screen. Do not infer lighting/renderer failure merely from black pixels.
Reset scene-related summaries on replacement and clear failures after successful
asset repair/reload. Avoid filesystem scans or repeated decoding on every frame.

Provide **Show Log** that reveals the existing Log dock and relevant details/filter
without accidentally hiding the underlying error. Provide **Open Log File/Folder**
using the actual active sink path.

### 4. Improve the existing file-log workflow

- Retain bounded rotating logs and add an obvious current-session boundary with
  engine version, executable path, and selected project path.
- Reuse or consolidate with `engine::log::AddFileSink()` after inspecting its path
  handling; do not install a duplicate sink.
- Make non-ASCII paths work. Handle an unwritable executable directory with a
  clearly reported writable application-log location where practical.
- Flush errors promptly. Log-file failure should itself be visible in the UI/
  console while leaving the editor usable.
- Keep log-path discovery in one place so Open Log points at the correct file.

## Part 2 — render debug views

### 1. Define transient renderer state

Introduce a backend-neutral enum such as `RenderDebugView { Final, Albedo, Normals,
Depth }` and a narrow getter/setter on the rendering subsystem/system.

- Default to Final. Treat the selection as editor/runtime inspection state,
  excluded from scenes, save games, and scene undo history.
- Validate enum values at the boundary. Prefer resetting to Final on editor launch
  for the first version rather than introducing saved preferences.
- Changing mode must not recreate the device/swap chain or reload assets.

### 2. Implement presentation from existing textures

| View | Required output |
|---|---|
| Final | Existing lit scene and tone-mapping behavior |
| Albedo | Stored surface color without lighting or Reinhard tone mapping |
| Normals | Existing encoded world-space normal RGB; do not encode it a second time |
| Depth | Documented, readable linear view-space depth visualization |

For Depth, reconstruct positive camera-space depth using the engine's actual
reverse-Z projection. Recommended initial mapping: near geometry bright, far
geometry dark, normalized over the camera near/far range. Handle cleared/background
depth separately as black and avoid NaN/division-by-zero results. Label the mode
clearly enough that users know this is visualized linear depth, not raw depth values.

Prefer extending `PresentPass.hlsl` with a uniform or shader variants and keep its
existing metadata identity. If an additional shader asset is genuinely necessary,
give it a fresh UUID and include it in asset/deployment checks.

Initially keep pass scheduling intact and select the displayed texture at
presentation. Skipping lighting/shadow work is a separate optimization. A debug
view does not make a fatally uninitialized renderer safe to use.

Bind the correct shader resources/constants on every mode switch, including back
to Final. Audit DSV/SRV hazards when sampling depth. Maintain C++/HLSL buffer packing
if adding visualization parameters. Do not introduce global gamma/color-management
changes as a side effect of adding the views.

Keep gizmos, host diagnostic text, and engine UI functional through the established
overlay ordering and single Present. Recommended initial policy: selection outlines
in Final only, so they do not alter the diagnostic data image. Explain this policy
in the UI/docs and test image values with other overlays disabled or excluded.

### 3. Add the editor selector

Put **Final / Albedo / Normals / Depth** in the existing viewport toolbar. Give each
mode a short tooltip explaining its meaning. Disable the selector when rendering
is unavailable; changing it should not dirty the scene or change Play state.
Ensure controls remain accessible when the error/status area is visible.

## Verification and acceptance criteria

Use small deterministic fixtures and temporary copies for failure tests. Never
damage checked-in shader files or Test1 to provoke errors.

### Failure reporting

- Missing and invalid required shaders fail startup gracefully with pass, path,
  stage/variant, and compiler/read details available to both UI and log.
- A deterministic resource-creation failure after partial initialization unwinds
  safely; failure tests do not call `std::exit`, access invalid handles, or crash.
- Failed startup/shutdown and a subsequent valid initialization attempt are safe
  within the supported lifecycle contract.
- Duplicate UUIDs across game and engine roots produce their actual catalog error.
- Pending, failed, repaired, and removed scene asset references update status without
  frame-by-frame log spam or stale messages after scene replacement.
- Empty Edit scenes and the editor camera are not labeled as fatal failures.
- The fatal banner and Show Log work when rendering never initialized.
- Log opening works for non-ASCII paths; no duplicate sink/messages are introduced.

### Debug views and presentation

- GPU pixel-readback checks exercise all four modes using known surfaces/normals
  and objects at multiple camera-space depths.
- Final retains its existing output; Albedo remains meaningful with lights disabled;
  Normals match the documented world-space encoding; Depth is ordered correctly
  under reverse-Z and responds correctly to camera near/far changes.
- Background pixels are defined and finite in every mode.
- Switching repeatedly, resizing, minimizing/restoring, and returning to Final
  do not produce stale bindings, trails, or a black presentation surface.
- Include a skinned mesh as well as static geometry in integration coverage.
- New status controls create no Qt-painted children over the native viewport.
- Preserve the existing native label, input, and engine UI rendering checks.

Relevant existing test targets: `viewport_overlay_tests`, `viewport_input_tests`,
`ui_render_tests`, `rendering_skinning_tests`, and `log_model_tests`. Add focused
diagnostics/debug-view tests where they fit; use WARP for deterministic GPU checks
and a small checked-in scene for the default integration case. Optional Test1
coverage is supplementary, not the only way to exercise the new functionality.

## Suggested implementation order

1. Inspect the current code and settle error ownership, lifecycle, and debug-view
   APIs. Record any necessary contract changes before splitting work.
2. Implement engine failure propagation and partial-resource cleanup; add failure
   tests before connecting the editor UI.
3. Implement status/log navigation using those structured results.
4. Implement the four presentation modes, then the toolbar selector.
5. Run targeted tests, native rendering checks, and a manual editor pass; update
   developer/user documentation with diagnostic meanings and debug-view semantics.

After APIs are fixed, the diagnostics/editor UI and debug-view presentation work
can be delegated separately. Assign ownership of shared files such as
`main_window.cpp`, `RenderingSubsystem.hpp`, and CMake files to avoid collisions.

## Build and handoff

Use the existing configured Windows build when available. Inspect its cache first;
enable tests/editor only if needed. Example commands from the repository root:

```powershell
cmake --preset windows-vs2022 -DSRI_BUILD_TESTS=ON
cmake --build build/windows-vs2022 --config Debug --target editor viewport_overlay_tests viewport_input_tests ui_render_tests rendering_skinning_tests log_model_tests --parallel 4
ctest --test-dir build/windows-vs2022 -C Debug -R '^(viewport_overlay_tests|viewport_input_tests|ui_render_tests|rendering_skinning_tests|log_model_tests)$' --output-on-failure
```

Include any new test targets in those commands. For the optional real-project
native viewport check:

```powershell
$env:SRI_TEST_PROJECT = "C:\Work\Gamedev\Projects\Test1\Test1\Test1.sriproject"
ctest --test-dir build/windows-vs2022 -C Debug -R '^viewport_overlay_tests$' --output-on-failure
```

Report the implemented behavior, public API changes, exact verification performed,
and any remaining manual checks. Update documentation and run `git diff --check`.
Do not report a successful build or passing headless unit tests as proof that the
native editor presentation was visually checked.
