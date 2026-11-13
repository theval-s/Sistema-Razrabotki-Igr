# RmlUi game UI implementation plan

## Goal and status

**Status: integration implemented; automated/build/deployment verification passed.** Checked
implementation items below do not imply that every milestone has passed manual acceptance.

Integrate **RmlUi 6.3** as the engine's player-facing UI system. Authors should build
menus, HUDs, inventories, and settings screens using RML documents, RCSS stylesheets,
and the engine's existing Lua scripts. The result must run in both standalone games
and the editor's Play viewport, with lightweight deployment and no browser runtime.

The phases retain the original design and acceptance criteria as a record. See the
execution record below for implemented deviations and the precise verification status.

### Execution record

- RmlUi dependency and vendored DX11 backend are from the 6.3 tag. Core/FreeType
  are integrated without the RmlUi Lua plugin; an engine-owned named callback bridge
  connects the existing Lua runtime to UI.
- Presentation moved from PresentPass to RenderingSystem, which invokes the native
  overlay after scene composition and presents once. The backend uses a direct D3D11.1
  context-state swap. Reload rolls back on default parser warnings as well as errors.
- Debug hosts enable the initially hidden debugger; F8 toggles it outside text entry.
  Non-Debug builds disable it. Interactive documents request a visible pointer even
  over hover pass-through areas. Editor Escape releases input; P opens pause, I opens
  inventory, and Tab/Space use standard keyboard/button behavior.
- 34 `ui_tests` passed, including Unicode/Cyrillic/supplementary text, fake
  clipboard, native-null IME composition commit/cancel events, DPI and paths.
  Input: 9 passed; Lua: 118 passed, 1 symlink test skipped; asset catalog: 6 passed,
  asset authoring: 8 passed.
- Real graphics: all 10 `ui_render_tests` passed, covering PNG alpha, rounded clipping,
  scissor, gradients, transforms, opacity layers, fonts, state restoration, aborted
  resize, foreign-runtime rejection, and the full scene/overlay/presentation path.
  Empty-scene fullscreen passes now explicitly set triangle-list topology.
- Debug shared editor and static standalone built; RelWithDebInfo editor, ExampleGame
  and deploy built. Portable standalone and editor launched from an unrelated CWD
  and exited cleanly. Standalone HUD, pause, keyboard Resume, inventory, tooltips and
  Cyrillic text were visually checked at 150% DPI; editor Play displayed the HUD.
  A separate installed-SDK consumer compiled, linked and ran against `Engine::Engine`.
  Four applicable shared-engine/editor regression executables also passed.
  [UI performance](docs/ui-performance.md) records the pre-UI binary comparison:
  ExampleGame +1,412,096 bytes and shared Engine.dll +1,420,288 bytes.
- Actual OS IME operation and candidate placement, multi-monitor DPI, and the manual
  acceptance scenarios below remain unverified. Native-null IME tests are not OS tests.

### First-release scope

- Screen-space UI on the engine's existing render surface; multiple documents in
  one RmlUi context, with explicit ordering and modal behavior.
- Text-based layouts, styles, templates, fonts, images, data binding, and Lua events.
- Mouse, keyboard navigation, Unicode text entry, clipboard, and IME integration.
- Correct resizing, DPI scaling, focus, gameplay input blocking, and paused operation.
- Session/instance-owned resources, developer reload, useful errors, and a debugger.
- An example HUD, pause/settings menu, and scrolling inventory with tooltips.
- Shared-engine/editor and static-engine/standalone builds and deployment.

Later extensions: world-space UI, multiple simultaneous render surfaces, engine
render textures inside UI, SVG/Lottie plugins, full gamepad input infrastructure,
and a visual UI designer. Keep keyboard/spatial navigation usable now so a future
gamepad adapter can use the same navigation actions. Dear ImGui is a separate
tooling choice and is not a dependency of this integration.

## 1. Codebase facts at planning time

Read these areas before implementation; this plan reflects the repository inspected
in November 2026.

| Area | Existing files and implications |
| --- | --- |
| Dependencies | `vcpkg.json` pins Lua 5.4.8. The checked-out `external/vcpkg/ports/rmlui/vcpkg.json` provides 6.3 with FreeType and optional Lua/SVG/Lottie features. Verify the version actually resolved by the manifest baseline. |
| Engine builds | `engine/CMakeLists.txt` builds both `engine` and `engine_static`. Add UI sources/dependencies to both, with exported engine-facing types and private third-party implementation details. |
| Frame scheduling | `engine/src/engine.cpp` runs subsystem Update in registration order, then controllers/fixed steps, then LateUpdate, then Render. Shutdown is in reverse registration order. |
| Timing | `FrameContext` already has real and simulation time. UI must use `realTimeSeconds` / `realDeltaSeconds`, including while paused. |
| Presentation | `engine/src/rendering/passes/PresentPass.cpp` draws the final scene and debug lines, then calls `context.Present()`. UI must be drawn after scene composition and before this presentation. |
| Resize | `RenderingSubsystem` defers resize to Render and skips drawing on invalid/minimized surfaces or failed resize. UI must follow this behavior. |
| Input | `InputSubsystem` latches keyboard/mouse state and has priority-based blocking contexts. It does not expose an ordered text/IME event stream. Frame snapshots alone are insufficient for UI input. |
| Standalone host | `Win32InputHost.cpp` pumps queued messages and polls mouse position. `engine/src/window.cpp` also hard-codes Escape to quit. Sent window messages and IME cannot be handled only by inspecting `PeekMessage`. |
| Editor host | `editor/src/viewport_widget.cpp` forwards input, suppresses autorepeat, releases input on Pause, and reserves Escape for releasing game input. Text input and paused UI require deliberate host changes. |
| Lua | `LuaRuntime.cpp` owns the sol state, per-instance environments, protected calls, instance/session IDs, and callback/scene-mutation guards. `ScriptSubsystem::Update` skips gameplay callbacks when paused. |
| Session lifecycle | Play/StartGame creates a new Lua state. Stop/EndPlay destroys entity scripts, then autoloads, then the state. Level changes retain autoloads; save-game loading can recreate a scripting session. |
| Assets | `AssetCatalog` has UUIDs and logical paths; `AssetPathResolver` supports `game/...` and `engine/...`. UI files/fonts are not recognized catalog types yet. |
| Textures | `TextureImporter.cpp` uses DirectXTK/WIC and forces sRGB for WIC textures. Its existing output must not be assumed compatible with RmlUi's alpha/color convention. |
| Hosts | Register subsystems in `editor/src/main_window.cpp` and `example_game/src/main.cpp`. The example already calls StartGame, currently without autoloads. |
| Tooling/tests | LuaLS definitions live in `editor/core/src/lua_definitions.cpp`. Tests are in `tests/CMakeLists.txt`; several executables are not registered with CTest. |

## 2. Architecture decisions

### 2.1 Runtime and renderer

Add a proposed `UiSubsystem` in `engine/include/engine/ui/` and
`engine/src/ui/`. It owns contexts, documents, model registrations, input routing,
and deferred UI operations. Keep RmlUi and sol implementation types behind private
headers/PImpl; public handles must not expose raw RmlUi element pointers.

- Use RmlUi Core, FreeType, and the optional developer Debugger.
- Start from the upstream **6.3 DX11 renderer**, adapting asset loading and render
  target integration. Retain its clip masks, transforms, and layer support rather
  than silently reducing the renderer to textured rectangles.
- Implement engine-specific RmlUi system/file interfaces for time, logging, paths,
  clipboard, cursors, and text-input activation. Host-specific operations are
  supplied by the Win32/Qt host; the engine must remain independent of Qt.
- The renderer uses the existing D3D11 device and context. It does not create its
  own window, swap chain, message loop, or presentation call.
- RmlUi has process-global initialization/interfaces. Initially support one active
  UI runtime per process, explicitly reject a conflicting second owner, and support
  clean sequential initialization/shutdown. Do not silently replace global interfaces.
- Hosts opt in by registering UiSubsystem. Existing headless engine/Lua tests and
  applications without UI must still work.

### 2.2 Lua integration: an engine-owned sol2 facade

Use the **existing Lua 5.4.8 state** and expose a small `UI` API through sol2.
Use RmlUi's official Lua plugin and Lua data-model implementation as references,
but do not enable the full plugin by default in this design.

Reason: our script instances have their own environments, ownership, protected-call
semantics, and deferred scene operations. A direct global document-script runtime
would need additional adaptation around all of these anyway. A narrow facade also
gives developers and agents a stable, documented engine API.

- Behavior lives in ordinary engine `.lua` scripts/autoloads. RML contains structure
  and data-binding expressions, not independently executed Lua script blocks.
- Track the active script instance through the existing runtime callback scopes.
  UI creation/registration requires a live, started owner, normally in OnStart.
  Calls through required module functions inherit that caller's ownership.
- Store callbacks in owner-aware runtime records, using protected functions.
  Route UI invocation through LuaRuntime's callback guard, reentrancy checks,
  fault policy, and diagnostics; do not call arbitrary sol functions from rendering.
- An absent UiSubsystem yields a useful unavailable error through UI creation APIs.

### 2.3 Scheduling and ownership

Suggested host registration order:

```text
Win32InputHost                     # standalone only
InputSubsystem
RenderingSubsystem
UiSubsystem
ScriptSubsystem
gameplay/editor camera/physics/render-extraction subsystems in their required order
```

This initializes the device before UI, routes UI input before gameplay readers,
and shuts scripting down before UI and UI before rendering. Preserve the existing
physics and extraction ordering constraints when updating each host.

Frame flow:

1. The host queues ordered input, without executing Lua from OS/Qt callbacks.
2. InputSubsystem latches the frame. UiSubsystem consumes its UI events and
   establishes gameplay capture/blocking before ScriptSubsystem and native readers.
3. RmlUi listeners copy event payloads into a queue. Dispatch those callbacks in
   UiSubsystem::Update, outside RmlUi event traversal, through LuaRuntime guards.
   Apply queued document close/reload operations at a safe boundary.
4. Gameplay updates normally. UI event callbacks remain available during pause;
   ordinary gameplay OnUpdate/OnFixedUpdate remain paused.
5. UiSubsystem::LateUpdate publishes dirty model values and calls context Update
   with real-time timing. Events produced here are queued for the next Update;
   do not introduce Lua callbacks into the unguarded LateUpdate/Render phases.
6. The renderer draws the scene, UI, and then presents exactly once. UiSubsystem's
   own Subsystem::Render must not draw a second time.

Lifetime rules:

- Documents, models, and subscriptions belong to the script instance that created
  them. Entity-owned UI expires with that instance; autoload-owned HUD/menu UI can
  survive level changes. UI must not be indiscriminately cleared by ResetSimulation.
- UI handles include slot/generation and session identity. Closed document handles,
  pre-reload element handles, destroyed-owner handles, and previous-session handles
  must fail safely; successful reload preserves the public document handle.
- OnDestroy may explicitly clean up UI; remaining resources are automatically
  released after OnDestroy, including when it fails. Reject creation during teardown.
- EndPlay disables new UI event dispatch, runs script teardown, removes documents,
  subscriptions and models, and releases every Lua reference **before** state reset.
  Cover failed startup and save-game session replacement, not just normal Stop.
- UI events may request scene changes/entity destruction. Reuse existing deferred
  scene-command boundaries; never mutate the scene underneath a UI traversal.
- Runtime UI is reconstructed from scripts/model data, not serialized into scene
  snapshots or SaveGame as document pointers. Game data may still use existing
  persistence hooks.

## 3. Author-facing contract

The exact API is published in LuaLS definitions and [the Lua facade guide](docs/ui-lua-facade.md).
Typical usage is:

```lua
local hud, pauseMenu, model

function OnStart()
    model = assert(UI.CreateModel("hud", {
        health = 100,
        player_name = "Player",
    }))

    hud = assert(UI.LoadDocument("game/ui/hud.rml"))
    hud:Show("hud")

    pauseMenu = assert(UI.LoadDocument("game/ui/pause_menu.rml"))
    pauseMenu:On("resume", "click", function(event)
        Time.SetPaused(false)
        pauseMenu:Hide()
    end)
end

function OpenPauseMenu()
    pauseMenu:Show("modal")
    Time.SetPaused(true)
end

function OnDestroy()
    -- Optional: automatic owner cleanup is still guaranteed.
    if pauseMenu then pauseMenu:Close() end
    if hud then hud:Close() end
end
```

`Time.SetPaused` is implemented alongside `Time.IsPaused`; editor pause indicators
observe engine pause changes. Pausing takes effect for
subsequent simulation scheduling, not retroactively for work already performed.

Minimum API behavior:

- `UI.CreateModel(name, initialValues)` and `UI.LoadDocument(pathOrReference)` return
  a handle or `nil, error`. Create models before loading documents that use them.
- Model names are unique in the context. Reject duplicate registration rather than
  replacing another owner's model. Define model/document dependencies so closing a
  document does not remove a model still used by another document. Initially require
  documents and their models to have the same owner; share HUD state through that
  autoload's methods. Destroy dependent documents before their model storage.
- `model:Set(name, value)` copies a top-level value and dirties that variable;
  `model:Get(name)` returns a copy. Mutating the original Lua table is not an implicit
  update. RmlUi form bindings write to the same backing model.
- Support strings, booleans, finite numbers, objects, and dense arrays for inventory
  lists. Reuse StateValue/Lua conversion infrastructure where appropriate, with
  deliberate validation of unsupported values and cycles.
- Define types/shapes at model creation. Array length can change, including to zero;
  replacing a nonempty array with `{}` retains its declared array type. Provide a
  documented schema/type mechanism for arrays that are empty at creation. Avoid
  dangling reflected pointers when a backing array or value tree reallocates.
- Documents load hidden. `Show("hud")` is noninteractive and passes input through;
  `Show("interactive")` captures only interaction with UI; `Show("modal")` blocks
  gameplay keyboard/mouse, brings the document forward, and establishes focus.
  Mode must affect actual hit testing/focus, not just a cosmetic flag.
- `Hide`, `Close`, `IsValid`, and `IsVisible` have deterministic behavior. Close is
  idempotent. Hiding/closing a modal releases its input and restores eligible focus.
- `doc:On(elementId, eventName, callback)` returns a disconnectable subscription.
  Keep it registered even if the caller discards the returned Lua handle; owner or
  document destruction disconnects it. Event payloads are copied values, not pointers.
- Provide a small element facade, found by ID, for escaped text, form value,
  class toggles, focus, and property changes. Distinguish plain text from intentional
  RML insertion. Prefer data binding for frequently changing display values.
- Developer reload retains the public document handle, models, visibility and mode;
  invalidates old element handles; reattaches ID-based subscriptions to the new DOM.
  Missing subscribed IDs produce diagnostics, not dangling callbacks.

Use standard RmlUi `data-model`, `{{...}}`, `data-if`, `data-for`, and `data-value`
syntax. Keep the initial event API explicit in Lua instead of inventing another
markup scripting language.

## 4. Implementation phases

### Phase 1 — Dependency and build plumbing

- [x] Add RmlUi 6.3 through the manifest, using FreeType and leaving SVG/Lottie/full
  Lua plugin features disabled initially. Confirm the resolved baseline/version;
  keep the existing Lua override and avoid unrelated dependency upgrades.
- [x] Link Core and developer Debugger as appropriate into both engine targets.
  Keep debugger activation developer-only. Check the actual exported CMake targets.
- [x] Obtain the DX11 renderer and all required shader/support files from the same
  6.3 tag. If the port does not install usable backend sources, vendor that small
  backend under `external/rmlui-backend/`, with version/provenance and MIT notices.
  Compile against the vcpkg core; do not compile a second RmlUi core.
- [x] Add proposed UI source directories, logging category, and build/install rules.
  Retain the existing engine API/export conventions and isolate third-party warnings.
- [x] Verify both shared/static engine builds and a Qt-free standalone configuration.

**Exit:** reproducible dependency resolution and linkable engine UI scaffolding.

### Phase 2 — Native runtime and DX11 vertical slice

- [x] Implement UiSubsystem, system/file interfaces, and a main context. Provide an
  internal/native document load path so rendering can be verified before Lua work.
- [x] Refactor final presentation into scene composition, UI rendering, then Present.
  A small explicit overlay hook/pass is sufficient. Do not add a second presentation
  path or a dependency from low-level rendering code onto Lua.
- [x] Use the existing device and target; audit any D3D11.1 interfaces required by
  the upstream backend and report initialization failure cleanly if unsupported.
- [x] Preserve/restore or invalidate the D3D state touched by UI, including state
  caches in RenderingContext. Check render targets, viewport, geometry shader,
  blend/depth/scissor state, and SRV/RTV hazards.
- [x] Composite UI after the scene's tone mapping. Establish an explicit color-space
  and premultiplied-alpha policy for colors, fonts, imported images, and the target.
  Test transparent colored edges; do not blindly reuse forced-sRGB scene textures.
- [x] Add resize/DPI updates, skip zero-area rendering, and cooperate with failed
  resize/retry. Release any back-buffer references before ResizeBuffers.
- [x] Ensure full cleanup on partial initialization failure, normal shutdown, and
  sequential engine lifetimes. Detach render hooks before destroying UI objects.

**Exit:** a styled RML document with a font and transparent image draws correctly over
the scene, resizes, and leaves scene rendering intact in both hosts.

### Phase 3 — Assets, fonts, and resource ownership

- [x] Add catalog kinds/codecs/extension recognition for UI documents (`.rml`),
  stylesheets (`.rcss`), templates (`.rmlt`), and fonts (`.ttf`, `.otf`). Extend metadata
  generation and relevant content-browser filters/type labels. These need catalog
  identities, not necessarily new asynchronous GPU AssetSystem tables.
- [x] Resolve top-level authored references by UUID. Allow explicit logical-path
  convenience loads by resolving a catalog entry; never use AssetReference.path as
  a fallback when its UUID is missing.
- [x] Resolve stylesheet/template/image/font references relative to their source
  document or stylesheet, following RmlUi URL semantics. Normalize through engine
  roots, allow normal `../` references within a root, and preserve the project's
  canonical root-containment rules. CWD must not affect asset lookup.
- [x] Use the shared catalog and UI-owned caches/resources so autoload UI survives
  unloading scene-scoped assets. Do not retain catalog entry pointers across scans.
- [x] Replace the backend's TGA-only loader with support for at least PNG. Reuse
  DirectXTK/WIC or a shared decoder where practical, with the UI color/alpha policy.
  Resolve files through engine assets, then create compatible UI-owned GPU resources.
- [x] Define ownership for loaded image textures versus Rml-generated font/effect
  textures. Never reinterpret an engine TextureHandle as a backend texture handle or
  release the same GPU object from two owners.
- [x] Supply a licensed default font with Latin and Cyrillic coverage and record its
  license. Document fallback font registration and the tested language coverage.
  Evaluate the HarfBuzz font-engine path separately for complex shaping/RTL; FreeType
  alone is not a promise of complete international text support.
- [x] Copy/install UI files, fonts, images, Lua scripts, sidecars, backend shader
  resources if needed, and license files in example/editor deployment. Clear old
  project caches when switching content roots.

**Exit:** nested relative resources and project paths containing non-ASCII characters
work in the build tree and deployed application, including after a level change.

### Phase 4 — Ordered input and gameplay capture

- [x] Add an ordered UI-facing event stream alongside the current gameplay state.
  Events include pointer position at the time of the event, button transitions,
  wheel, logical key transitions/repeats, modifiers, Unicode text, focus and IME data.
  Keep gameplay physical-key bindings and per-frame edge semantics intact.
- [x] Feed each native event once. In Win32, coordinate the message pump/window
  procedure so sent focus/IME messages are covered and queued messages are not
  duplicated. Keep physical keys separate from layout-dependent text translation.
- [x] Remove the window procedure's hard-coded Escape-to-quit behavior; quitting and
  opening/closing menus belong to application input policy.
- [x] In Qt, forward key repeats to UI while retaining existing gameplay deduplication.
  Ensure Tab/Shift+Tab reaches UI instead of accidentally moving focus to editor docks.
- [x] Route UI before gameplay and use priority blocking contexts/capture state.
  Consume the activating click/key and its release even when a modal opens and
  closes in the same frame. Retain consumed edges for that frame and suppress held
  inputs until release so menu actions never also fire weapons or move the camera.
- [x] Distinguish passive HUD, interactive UI hit regions, focused text entry,
  pointer capture while dragging, and modal capture. A full-screen body must not
  accidentally consume every world click. Test dragging beyond an element's bounds.
- [x] Expose cursor/text-input requests to hosts. Release relative mouse lock while
  menus need a pointer; restore it only according to host/game focus policy.
  Prevent ExampleGameLogic from recapturing the pointer on a consumed UI click.
- [x] On focus loss/Stop, clear keys, repeats, pointer capture, text composition,
  and queued input consistently. Do not synthesize a click at (0, 0).

**Exit:** buttons, sliders, scroll areas and keyboard navigation work; UI interaction
does not leak into gameplay or editor selection/camera controls.

### Phase 5 — Text entry, DPI, and paused editor behavior

- [x] Implement Unicode text delivery independently of key codes, including Win32
  UTF-16 surrogate handling, dead keys, autorepeat, and modifier shortcuts.
- [x] Implement clipboard operations and text-field activation/caret reporting.
- [x] Wire RmlUi's IME interface to native Win32 composition/result messages and Qt
  inputMethodEvent/inputMethodQuery, including candidate/caret position, cancellation,
  and avoiding double-insertion of committed text.
- [x] Choose one coordinate convention: physical surface pixels for Rml context
  dimensions and pointer events, with RmlUi dp-ratio for authored `dp` units.
  Convert Qt logical coordinates once; convert caret rectangles back for host IME.
  Handle DPI changes even when pixel dimensions happen to remain unchanged.
- [x] Separate paused simulation from permission to interact with focused game UI.
  The current ViewportWidget setPaused/captureInput policy blocks paused UI; adapt it
  so a focused/clicked Play viewport can operate menus without resuming gameplay.
- [x] Preserve an explicit editor release-input shortcut and Stop/Pause shortcuts.
  Existing Escape-to-release takes precedence in the editor; document this and use
  a non-conflicting pause action in the example. Reacquisition clicks retain the
  existing no-stray-game-click behavior.
- [x] Synchronize the editor toolbar/PlaySession view with engine pause events,
  including pause/resume initiated from Lua. Avoid feedback loops.

**Exit:** editable fields accept/paste Unicode and tested IME input in both hosts;
menu animation and Resume work while simulation remains paused; DPI hit testing and
IME placement agree with rendering.

### Phase 6 — Lua models, documents, callbacks, and teardown

- [x] Implement the API in section 3, proposed `LuaUiBindings.cpp` and private
  owner-aware runtime support. Keep the Lua state private to LuaRuntime.
- [x] Establish an owner scope for lifecycle callbacks, script-method calls, and UI
  callbacks; reject new UI registrations outside a valid owner or during teardown.
- [x] Implement stable document/element/model/subscription handles, protected event
  dispatch, and copied event payloads. Auto-disconnect after owner failure/destruction
  so stale or faulted scripts are not invoked repeatedly. A faulted owner's modal
  must be closed at a safe boundary so it cannot leave gameplay input blocked forever.
- [x] Implement RmlUi data-variable definitions over stable backing storage, explicit
  dirty updates, two-way form values, and dynamic list lengths. Document the array
  index convention at the Lua/Rml expression boundary.
- [x] Integrate instance destruction, EndPlay, failed BeginPlay, LoadGame replacement,
  and normal engine shutdown. Autoload documents survive ordinary level transitions;
  entity-owned documents do not. Close/reload during callbacks is deferred safely.
- [x] Add the proposed Time.SetPaused binding with existing engine pause semantics.
- [x] Extend generated LuaLS definitions and error context with source document,
  element/event, script owner, and Lua traceback where available.

**Exit:** Lua controls a HUD and list, UI writes back a form value, Resume works during
pause, and repeated Play/Stop/level/load transitions leave no stale callbacks.

### Phase 7 — Reload, debugger, and agent-friendly authoring

- [x] Add a developer command/API to reload a document and its referenced styles.
  Load a hidden replacement and verify it before swapping; failed reload preserves
  the last working document. RmlUi can recover from some parse errors, so a non-null
  LoadDocument result alone must not be treated as complete validation.
- [x] Preserve models and script state; rebind ID-based subscriptions and invalidate
  old element handles. Define focus/scroll preservation as best effort by stable IDs.
  Suppress callback delivery from the candidate until the replacement is committed.
- [x] Handle stylesheet/template cache invalidation deliberately, since these caches
  can affect multiple documents. Record dependencies for reload; do not destroy live
  model storage while a replacement or another document still references it.
- [x] Expose the RmlUi debugger in development builds, with a shortcut that does not
  steal text input. Route its logs into the existing engine/editor logging system.
- [x] Ship a minimal base stylesheet and reusable button/panel/list/tooltip styles.
  RmlUi has no browser-style default control theme to rely on.
- [x] Document the supported RCSS subset, project paths, API, data bindings, modal
  behavior, asset requirements, hot-reload limitations, and a preview/screenshot
  workflow in `docs/ui.md`; link it from README and scripting docs.

Automatic debounced file watching can follow the explicit reload command. Live Lua
code reload and font-atlas replacement are separate features; document their actual
restart/reload requirements instead of implying they work automatically.

**Exit:** an author/agent can edit text assets, preview/reload them, inspect layout,
and diagnose invalid input without rebuilding the engine.

### Phase 8 — Example content, verification, and deployment

- [x] Add `example_game/ui/` and `example_game/scripts/ui/` content with metadata.
  Register a UI autoload through the existing StartGame autoload argument; provide
  equivalent instructions/content for opening it in the editor.
- [x] Demonstrate a passive HUD with bound values, a modal pause/settings menu with
  buttons/slider/text field, and an inventory with a dynamic scrolling list/tooltips.
  Include a second RCSS theme to prove that layout/behavior can be restyled.
- [x] Exercise fonts, transparent PNG edges, rounded clipping, gradients, and simple
  transitions. Test advanced DX11 filters separately; upstream documents open issues
  there, so report the tested feature set rather than declaring every effect verified.
- [x] Add the focused tests and run the checks below. Record measured release binary
  and runtime memory deltas and CPU/GPU UI cost for representative example screens;
  include machine/build details rather than promising a universal budget.
- [x] Verify runtime DLL/assets/licenses in a portable build launched from another
  working directory, and verify SDK/public-header consumption after installation.

**Exit:** both hosts demonstrate the documented API, the regression checks pass,
and the UI works outside the developer build directory.

## 5. Verification strategy

Use meaningful behavior tests, not tests that merely duplicate wrappers. Add a
proposed `ui_tests` target; use an injected/null RmlUi render interface for logic
tests where practical, and real DX11/manual integration checks for graphics.

### Automated coverage

- Document-handle invalidation on close, owner destruction and session replacement;
  element-handle invalidation on reload; idempotent close; cleanup after failed
  startup and failed callbacks, including release of modal input blocking.
- No callback execution after Lua state destruction; protected callback errors have
  useful context; callbacks that close their document or destroy their owner are safe.
- Autoload UI survives OpenLevel; entity UI is removed; LoadGame recreates session UI.
- UI events run while paused while gameplay lifecycle updates remain paused.
- Model scalar/list updates, two-way form updates, empty arrays, rejected bad values,
  and backing-storage reallocation without invalid references.
- Input event order, repeats, multiple transitions in one frame, same-frame modal
  open/close, held-key suppression, pointer capture and focus loss.
- Asset metadata round trips, relative resources, missing files/UUIDs, canonical
  containment, non-ASCII paths and CWD-independent resolution.
- Successful and failed reload, retained model/subscriptions, stale element handles.

Run existing `input_tests`, `lua_runtime_tests`, `asset_catalog_tests`, and
`asset_authoring_tests` alongside new tests. Add focused rendering/editor regression
coverage where the final presentation and host input behavior change.

### Build commands

Run from a suitable MSVC developer environment. The example preset disables the editor:

```powershell
cmake --preset windows-example-game -DSRI_BUILD_EDITOR=OFF -DSRI_BUILD_TESTS=ON
cmake --build build/windows-example-game --config Debug --target ExampleGame ui_tests ui_render_tests input_tests lua_runtime_tests asset_catalog_tests asset_authoring_tests

& "build/windows-example-game/bin/Debug/ui_tests.exe"
& "build/windows-example-game/bin/Debug/ui_render_tests.exe"
& "build/windows-example-game/bin/Debug/input_tests.exe"
& "build/windows-example-game/bin/Debug/lua_runtime_tests.exe"
& "build/windows-example-game/bin/Debug/asset_catalog_tests.exe"
& "build/windows-example-game/bin/Debug/asset_authoring_tests.exe"

cmake --preset windows-vs2022 -DSRI_BUILD_EDITOR=ON -DSRI_BUILD_TESTS=ON
cmake --build --preset windows-debug --target editor ExampleGame ui_tests ui_render_tests
cmake --build --preset windows-release --target editor ExampleGame deploy
```

`ui_tests` and `ui_render_tests` are registered with CTest. Do not assume CTest runs
every other existing executable. Check actual output paths
if build configuration is changed.

### Manual acceptance

1. Standalone and editor Play show the same UI without Qt dependencies in standalone.
2. Repeated Play/Stop, level changes, save/load, focus loss, resize and minimize/restore
   produce no stale handles, duplicate callbacks, or D3D debug-layer errors.
3. Modal clicks/keyboard input never reach gameplay; passive HUD areas pass through;
   dragging, Tab navigation, text selection and clipboard behave correctly.
4. Pause stops simulation while menu transitions, input and Resume still work. Editor
   release-input and Stop remain available, and editor controls stay usable.
5. Latin/Cyrillic, surrogate-pair text and a tested IME composition flow work in both
   hosts. Test at 100%, 150%, and 200% DPI and when moving between monitors.
6. Transparent images/fonts have correct edges and colors; rounded/transformed clips
   and scene/debug rendering remain correct before and after UI rendering.
7. Editing/reloading a document/theme works; malformed replacements preserve the
   current screen with actionable diagnostics; missing assets do not crash the game.
8. A portable deployment loads all UI assets from another working directory.

## 6. Upstream references

- RmlUi project and backend capability matrix: <https://github.com/mikke89/RmlUi>
- Pinned release: <https://github.com/mikke89/RmlUi/releases/tag/6.3>
- Integration: <https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/integrating.html>
- Render interface: <https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html>
- Data bindings: <https://mikke89.github.io/RmlUiDoc/pages/data_bindings.html>
- Lua integration reference: <https://mikke89.github.io/RmlUiDoc/pages/lua_manual/getting_started.html>
- Supported styles: <https://mikke89.github.io/RmlUiDoc/pages/rcss/property_index.html>
- IME: <https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/ime.html>

Use the documentation and backend sources that match the pinned version. Repository
master and online documentation may move ahead of 6.3 during implementation.
