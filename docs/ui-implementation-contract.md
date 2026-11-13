# UI native integration contract

This reference describes the native UI boundary, ownership rules and host integration.
See [game UI authoring](ui.md) for RML/RCSS setup and [the Lua facade](ui-lua-facade.md)
for script APIs.
Public UI headers contain neither sol nor Rml types and follow the engine's exported,
out-of-line PImpl convention. C++20 and the existing engine SDK ABI/toolchain apply;
these STL-bearing interfaces are not a cross-compiler binary protocol.

## Ownership, memory, and deferred work

`UiSubsystem` owns exactly one Rml runtime/context and its interfaces. Acquire a
process-global exclusive lease before setting any Rml global interfaces. A second
owner fails initialization without changing globals. Unwind every partial setup;
release the lease last. Sequential runtimes must work. Null rendering still owns
this lease and uses real Core/layout/data binding with a no-op render interface.

Each initialized subsystem can host multiple UI sessions, including independent
native sessions. Allocate process-unique nonzero 64-bit session IDs (not Lua's local
session counter); never recycle or wrap. Typed handles have a 64-bit session and
32-bit slot/generation, with an intended 16-byte value layout. Default is empty.
Generation overflow retires the slot permanently. No reinterpretation of InstanceId,
Rml pointers, engine texture handles, or handle types. Liveness requires checking
session, slot, generation, owner state and resource state, not merely `IsEmpty`.

Use slot tables of boxed records (`unique_ptr<Record>`) so callbacks growing tables
cannot move records. Queued events hold subscription/document tokens, DOM revision,
owner token and a copied `UiEvent`; queued operations own strings, references and
values by copy. Never capture raw record pointers, sol values, a Lua state, or host
widget pointers in deferred commands. All access and retirement is on the engine
thread; asynchronous work may only post owned results plus tokens for revalidation.
There is no public thread-safe mutation API.

Close/disconnect/owner release logically invalidate immediately and cancel pending
deliveries; physical Rml destruction waits until traversal and callback depth are
zero. Keep model storage alive until dependent live, closing and reload-candidate
DOMs are destroyed. Tombstones must retain enough identity to make repeated Close
and Disconnect from the same owner succeed, including after slot reuse; another
owner must still fail. A revoked owner can perform idempotent cleanup, not create
new resources. Session-expired mutation fails safely. `IsVisible` is false for an
invalid/closing handle. Handle destruction has no resource side effects.

## Lua bridge contract

Implement `UiCallbackDispatcher` inside `LuaRuntime.cpp`, with access to its private
Impl. Keep sol registry entries in that Impl, declared after `state`, keyed by
`UiCallback`, recording the owning `UiOwner`. The bridge must have a revocable weak
lifetime link to Impl, not an unchecked raw runtime pointer; UI holds only a weak
bridge. It must be safe to lock a bridge after runtime revocation and get Dropped.
Runtime bindings likewise pair their borrowed UiSubsystem pointer with
GetLifetimeToken and check it before access; shutdown invalidates that token before
physical retirement. A reinitialized subsystem requires a new session/attachment.
No new public state getter or Rml Lua plugin is needed.

1. On engine-backed Initialize, optionally find UiSubsystem and begin a script UI
   session. Install the UI facade even without UI, returning a useful unavailable
   error for creation. Non-engine/headless LuaRuntime remains supported.
2. Create an owner mapping per InstanceId; owners start dormant. StartInstance already
   sets `started` before OnStart: enable there. A scoped active-owner stack is needed
   for OnStart/Update/FixedUpdate, physics, persistence, CallScript and UI callbacks.
   Required modules inherit the active caller; CallScript switches to its receiver.
   Global ExecuteString/CallFunction without an instance cannot create UI resources.
3. The binding reserves a callback token, inserts its protected function, then
   subscribes. Roll back both on failure. Discarding Lua subscription userdata does
   not disconnect it. All resource operations use the active owner, preventing a
   leaked handle from becoming another script's mutation capability.
4. Dispatch validates callback and owner against the *current* runtime mapping,
   started/destroying/faulted/active/session flags. Establish Impl::Scope (including
   the entity's scene), active-owner scope, and Instance::active RAII guard, then use
   the existing protected-call/error policy. A stale callback drops. A reentrant
   receiver drops with diagnostics rather than recursively invoking. An exception
   or protected-call failure faults the owner and requests UI retirement. Include
   script/entity/autoload, source document, target/event, and Lua traceback.
5. Fault cleanup also applies to failures in ordinary lifecycle/method callbacks,
   including failed OnStart. Mark owner retiring, cancel events, release callback
   refs and remove its modal at the next safe Rml boundary. Do not leave input locked.
6. DestroyInstance calls BeginOwnerTeardown before OnDestroy, and ReleaseOwner via
   unconditional scope cleanup afterward, even on exceptions. Existing policy skips
   OnDestroy on faulted instances; automatic UI cleanup still runs. OnDestroy can
   close/hide/update existing resources but cannot load/register new ones.
7. **ScriptSubsystem::EndPlay must SuspendSession before ClearSceneScripts**, not
   only at runtime Shutdown: entity teardown currently happens before Shutdown.
   Suspend forbids creation and delivery for the entire session while allowing
   OnDestroy cleanup. Shutdown also suspends on direct use/failed Initialize.
8. EndSession must run after instance teardown but **before `state.reset()`** and
   before destroying the callback registry/bridge. It synchronously releases every
   reserved and subscribed callback reference. Clear registry as a final backstop,
   revoke bridge, then destroy Lua state. Deferred Rml retirement contains no sol.
   Handle catch paths that currently remove an instance without normal teardown.
   Cover BeginPlay failures, prepared startup, LoadGame replacement, direct Shutdown,
   EndPlay and engine shutdown. ResetSimulation only clears transient UI input;
   it must not close autoload documents on level changes.

The dispatcher is a trusted private protocol: only UiSubsystem's Update loop calls
Dispatch. ReleaseCallback is synchronous, noexcept and never calls user Lua. Copy/
pin the protected function locally while invoking so self-disconnect can erase its
registry entry; that local copy must die before a state reset. State reset requested
inside callbacks must defer until callback depth is zero (as scene/game requests do).
Never allow teardown to destroy an executing Lua state. Native callbacks receive the
same owner validation and scene external-callback guard; exceptions retire the owner.

## Frame protocol

Register Input -> Rendering -> UI -> Script; standalone input host precedes Input.
UI Update runs even while paused. First query host surface metrics, drain safe
retirements, process prior-frame Rml events, and process the ordered input stream.
For each host event, run its Rml
traversal to completion, copy listeners' payloads, exit traversal, then dispatch the
resulting callbacks and apply safe operations before processing the next host event.
This lets a click-opened modal capture later events in the same frame. Use a bounded
dispatch wave: events generated by callback-driven Rml mutations queue for the next
Update, preventing recursive synthetic-event loops. Retest subscription/owner/DOM
revision immediately before each delivery. Successful reload cancels old-DOM events.

UI never flushes runtime scene commands inside traversal/dispatch. Existing
ScriptSubsystem Update flushes before its pause early-return, and Engine has a final
post-render flush; preserve those deferred boundaries. Native callbacks must use
Engine's deferred scene/game request paths and guard Scene direct mutations too.
LateUpdate publishes dirty data then calls context Update using real time; events
from there/render wait for the next Update. Render never calls Lua. Immediate native
mutations outside traversal may update Rml state but still only enqueue events.

## Model contract and stable reflected storage

Public values reuse StateValue with a restricted domain: bool, finite number, UTF-8
string, exact-shape object, homogeneous dense array. Null, cycles, sparse/mixed Lua
tables, unsupported userdata/functions, nonfinite numbers, and integers that cannot
round-trip exactly through double are rejected. Schema validation also rejects
extraneous members/elementType entries, invalid Rml variable/member identifiers and
excessive nesting (use the existing StateValue conversion limits consistently).
Treat every Number as double internally. An object may be empty; its shape remains
empty. Root schema is Object. Object replacement must contain exactly declared keys;
array element schemas are recursive and fixed, while length may change freely.

For Lua, finalize the facade as `UI.CreateModel(name, initialValues, schema?)` with
schema scalars `"boolean"`, `"number"`, `"string"`, objects
`{ type = "object", fields = { ... } }`, and arrays
`{ type = "array", element = ... }`. Example empty inventory:

```lua
UI.CreateModel("inventory", {items = {}}, {
    type = "object", fields = {
        items = {type = "array", element = {
            type = "object", fields = {name = "string", count = "number"}
        }}
    }
})
```

Without schema infer recursively; reject empty arrays as ambiguous. In Lua an empty
table defaults to an empty object during inference. With declared Array, `{}` means
empty Array, including Set on an existing variable. Dense Lua indexing is 1..N;
Rml expressions and UiModelPath indexing are 0..N-1. Get returns deep copies; Set
validates and copies transactionally and dirties the containing top-level variable.
Rml form writes use the same validation/storage and dirty tracking, visible to Get
immediately; setters never execute Lua inline.

Implement private UiModelStorage with frozen schema and owned StateValue tree.
Rml DataVariable `void*` must refer to an arena-owned stable path node containing a
weak storage reference + path + schema type, never to a vector element or StateValue
child. Re-resolve on every get/set/size/child operation. Cache path nodes per path,
retain them until Rml model unregister; a removed index returns failure, never a
dangling reference. Index paths refer to positions, not item identity. Keep the
schema definitions alive through unregister too. Invalidate/update length and mark
the top-level variable dirty on replacement. This handles reallocating and nested
arrays without keeping entire stale value trees alive.

Context-wide model names are unique. Native callers supply UiDocumentDesc.models;
the Lua load facade builds it from that owner's registered models. Validate all
markup/template data-model references against these dependencies and owner before
commit. Cross-owner model bindings fail. Closing a document does not remove models.
Explicit RemoveModel fails while dependencies exist; owner teardown destroys all
dependent DOMs first. Reload candidates count as dependencies.

## Documents, assets, and reload

Loads resolve AssetReference.id exclusively through the shared AssetCatalog; `path`
is diagnostic only. Logical-path overload resolves FindPath and then loads by UUID.
Copy catalog entries before any subsequent scan. UI owns its caches/textures outside
scene asset tables. Source-relative URLs normalize within game/engine roots using
canonical containment; never resolve from CWD. Register catalog fonts through the
private asset adapter/default font bootstrap before loading documents.

Load is synchronous and hidden; no RML script execution. Hud must disable hit tests
and focus in the actual DOM. Interactive captures hit regions/focused text/dragging,
not an incidental fullscreen body. Modal brings forward, blocks both devices and
establishes focus. Hide/close restore eligible focus and release capture. SetText
escapes markup, SetInnerRml is explicit insertion; both invalidate handles to removed
descendants. Elements are separate slots with document+DOM revision checks, not raw
pointers disguised as tokens. Re-resolve ID-based subscriptions after DOM mutation.

RequestReload acknowledges queueing, with completion diagnostics in engine logs.
At a safe boundary load a hidden candidate, collect parser/resource diagnostics
(non-null document alone is insufficient), validate dependencies, then atomically
swap. Preserve document token, models, mode, visibility, and subscriptions; increment
DOM revision and invalidate all old element slots. On failure destroy candidate and
retain old DOM. Missing subscribed IDs are diagnosed and subscription remains dormant
for a later reload. Suppress candidate events. Focus/scroll restoration by ID is best
effort. Invalidate stylesheet/template caches deliberately, accounting for other docs.

## InputSubsystem contract

`InputUiEvent.hpp` is the public event shape. InputSubsystem provides these
declarations/semantics:

```cpp
void OnUiEvent(InputUiEvent event);
std::span<const InputUiEvent> GetUiEvents() const noexcept;
void ConsumeUiEvent(std::uint64_t sequence);
void SetUiCapture(bool keyboard, bool mouse);
```

Maintain pending/latched owned vectors. Update swaps a complete batch; the span is
valid until next Input Update/Reset, with nonzero increasing sequence IDs. OnUiEvent
does not update gameplay state: hosts feed existing OnKey/OnPhysicalKey/OnMouse*
once and enqueue the corresponding UI event once. Do not synthesize a second event
from existing setters. UI handles logical keys/repeats, gameplay remains physical.
Navigation supplement covers Home/End/Page/Insert absent from current Key enum.

ConsumeUiEvent records consumed physical/logical key/button transitions and wheel/
motion for all gameplay readers, including legacy raw queries. Block pressed and
released edges for the rest of the frame even if capture toggles off. Held consumed
keys/buttons stay suppressed until their real release; masking must not clear raw
physical truth needed to detect release. SetUiCapture is an engine-reserved highest
priority device mask, effective for already-latched state; preserve per-frame sticky
consumption across modal open/close. Newly gained capture suppresses already-held
keys. Input Reset/focus loss must clear UI queues, repeats, suppression, composition
and capture coherently, while ensuring UI receives a focus-loss signal to cancel Rml
state. Do not manufacture pointer events at zero coordinates.

Win32 WndProc must cover sent IME/focus messages and queued input exactly once;
coordinate pump translation, including UTF-16 surrogate handling. IME Commit is the
sole insertion for that result; suppress duplicate WM_CHAR/InputUiText. Qt retains
UI repeats/Tab while gameplay deduplicates physical presses. Forward paused UI input
and preserve editor release/Stop policy. Convert coordinates to physical pixels
once, and use dpRatio independently of dimensions. Host reports metrics on DPI-only
changes too. Composition selection/replacement/clause indices use Unicode scalars;
Win32/Qt UTF-16 offsets require conversion and bounds validation. Map Rml's 6.3 text
input context and composition ranges, not just ProcessTextInput of preedit text.

`UiHost::GetSurfaceMetrics()` returns an optional snapshot of physical dimensions
and DPI. UI queries it during initialization and at the start of Update, after the
host has processed window events. Return zero dimensions for an unavailable or
minimized surface. Return `std::nullopt` for custom/offscreen hosts that use configured
metrics or `UiSubsystem::SetSurfaceMetrics()` explicitly. For DX11, nonzero host
snapshots supply DPI while the rendering surface determines dimensions, keeping UI
and the render target synchronized. Native host lifetimes must be checked before
querying the window; an expired host reports an unavailable surface.

UiHost receives copied surrounding text/selection/caret snapshots for IME query and
candidate placement. Convert caret pixels back to native/logical coordinates. Cancel
composition on focus loss, owner teardown or session end. Host clipboard and cursor
remain Qt-free. `SetCapture` supplies UI requirements to the host's cursor policy;
it does not change the gameplay request stored in `InputSubsystem`. UI pointer
capture, a visible-pointer requirement, or text entry overrides gameplay hiding and
locking. A UI drag keeps its mouse capture independently of gameplay mode. When UI
releases its requirements, the host reapplies the requested mode if it owns input.
Focus loss or explicit editor input release frees capture without clearing that
request. The recapture click is consumed by the editor. A null host uses an in-memory
clipboard and no-op presentation requests.

## Renderer hook

Add an owned/detachable overlay registration to RenderingSubsystem/RenderingSystem,
invoked by RenderingSystem **after composition + debug lines, before its sole Present**.
The hook calls UiRuntimeAccess::RenderOverlay. Use a weak lifetime or registration
lease, detached before UI shutdown; never capture an unguarded UiSubsystem pointer.
Call BeforeSurfaceResize before releasing/recreating the swap-chain target, including
failed retry paths. Overlay executes only when RenderingSubsystem's surface is valid
and resize succeeded. Borrow current RTV/context only during the call; retain no
back-buffer COM refs across frames. Host metrics communicate physical dimensions
and dp ratio; the overlay does not independently present or resize the swap chain.

Retain upstream 6.3 DX11 clip/transform/layer support. Restore touched D3D state AND
invalidate/resynchronize RenderingContext caches (including geometry shader and
SRV/RTV hazards). Compose after tone mapping; define premultiplied-alpha and target
color-space treatment consistently for fonts, vertex colors, and decoded PNGs.
Do not reuse forced-sRGB scene texture output without conversion/audit. Null backend
requires neither RenderingSubsystem nor a native surface and still runs layout/input.

## SDK consumption

Installed SDK consumers use `find_package(Engine CONFIG REQUIRED)` and link
`Engine::Engine`. In-tree standalone applications can link `Engine::EngineStatic`.
Public UI headers live under `engine/ui/`; private `UiRuntimeAccess` is not installed
API. Custom hosts implement `UiHost` and supply it through `UiSubsystemConfig::host`.
