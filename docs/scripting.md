# Scripting sessions and instances

## Play current scene

Use **Play Current Scene** on the viewport toolbar (F5), **Pause** to suspend simulation,
and **Stop** (Shift+F5) to return to editing. Play uses the current in-memory scene,
including unsaved edits; it does not compile C++ or write the scene to disk.
Autoloads start before entity scripts and native controllers. Stop restores the original scene,
selection and unsaved-change status, even after visiting other levels. Authoring
panels and scene/project file actions are disabled during Play. Closing the editor
stops the game before offering to save the restored editing scene.

During Play the scene camera drives rendering. When a level has no camera the
renderer retains the last viewpoint (the editor viewpoint on initial Play), with
a warning on initial startup. The editor viewpoint is restored on Stop. Escape
releases game input; click the Play viewport to reacquire it. That click is consumed
by the editor. Right-click no longer captures the editor navigation cursor during
gameplay. Pause leaves game UI interactive; resuming does not reacquire explicitly
released input. Play controls remain available while the gameplay cursor is locked.

Lua can request a level change:

```lua
local ok, message = Scene.OpenLevel("Next")
-- Also accepts "scenes/Next.scene.json" or "game/scenes/Next.scene.json".
-- Short names resolve under game/scenes/; names are case-sensitive catalog keys.
if not ok then Log.warning(message) end
```

The request validates a catalogued project Scene and its JSON before queueing it.
It is applied after gameplay callbacks at the end of the next/current Engine tick;
the calling script must not assume the new scene exists immediately. If multiple
requests arrive in one tick, the last valid request wins. Invalid requests return
false plus a diagnostic and keep the current scene. OpenLevel errors are already
logged by the engine, so displaying the returned message is optional.

Scene changes destroy entity-owned Lua instances before removing old entities,
then load the new scene. Autoload instances and require cache survive across levels
until Stop. New entity scripts start after scene population. Unknown/unserializable authored components
cause Play to fail before scene mutation rather than silently dropping them.

Snapshots are authored data, not SaveGame files. In editor mode C++ controller
definitions remain dormant until Play. Game time starts at zero on each Play;
real editor time and rendering continue while paused. An ordinary standalone
Engine keeps its existing lifecycle unless its host starts a game session. A C++
standalone host uses `Engine::StartGame(contentRoot, autoloads, error)` to start
autoloads and scripts in the current scene. StartGame requires an initialized
standalone Engine, a registered ScriptSubsystem, a loaded **catalog-backed scene**,
and no active session or callback/transition. `StopGame()` ends that session and
returns to normal standalone operation. Editor hosts use the Play/Stop lifecycle
and its editing-scene snapshot.

## Editor logging and autoload settings

### Lua completion in VS Code

Open the folder containing your `.sriproject` in VS Code, with the Lua Language
Server extension installed. On project opening and after saving Autoload settings,
SRI Editor writes `.sri/lua/engine.d.lua` and `autoloads.d.lua`, and merges their
library path into `.luarc.json`. **Project → Refresh Lua Definitions** regenerates
them on demand. These are tooling files outside `content/`; never require them.
Generated files are replaced on refresh, so keep your own annotations elsewhere.

The engine definitions describe Log, math values, Entity and component snapshots,
Scene, Input contexts and key constants, Physics, Time, script communication,
SaveGame requests, and lifecycle/persistence hooks. LuaLS recognizes Properties
and can infer field suggestions from your literal defaults. New scripts include a
commented example. After updating the editor, use Refresh Lua Definitions to update
existing projects; existing script source files are left unchanged.
Existing LuaLS settings and library paths are retained. A
malformed `.luarc.json` is left intact and reported; if you use `.luarc.jsonc`, add
`.sri/lua` to `workspace.library` yourself (the editor leaves comments untouched).
The automatic setup assumes VS Code opens the project root, not just `content/`
or an individual Lua file. After updating definitions, restart the Lua language
server if completion has not refreshed.

### Accessing autoloads

The configured name addresses the actual running instance:

```lua
-- GameState.lua, registered as GameState in Project → Autoload
player_score = 0
local bonus = 5 -- private, not exposed through Autoloads

function AddScore(amount)
    player_score = player_score + amount + bonus
end

-- In a GameObject script:
function OnStart()
    Autoloads.GameState.AddScore(10)
    Log.info(tostring(Autoloads.GameState.player_score))
end
```

Top-level script functions use dot calls; legacy table methods that accept self
use colon calls. `Autoloads.Name` is not a second execution of the source and does
not require a module. Public fields/functions stay live across scene changes.
Only configured names get completion: each entry is an open table, so the engine
does not infer or validate its fields or function signatures. Maintain those
contracts in your scripts. Disabled names get optional types and are nil at runtime.
Lua keywords used as names need brackets, e.g. `Autoloads["end"]`.

Startup follows list order: later autoloads can use earlier ones; object scripts
start after all enabled autoloads. An autoload is published after its file loads
and before its OnStart. During reverse teardown, an instance remains available
through its own OnDestroy, then its name is removed. Stop/failed startup drops the
whole registry with the session. A callback fault keeps the table available but
disables that instance's remaining lifecycle callbacks, following normal policy.

`Log.info("Hello world")`, `Log.warning(...)`, and `Log.error(...)` are registered
by LuaRuntime's RegisterLog during state initialization. They write to the shared
spdlog default logger. MainWindow::buildLogDock installs a callback sink, copies
the message onto Qt's GUI thread, and appendLog displays it in the existing Log
panel. The editor and Engine continue linking the same shared spdlog; the engine
does not depend on Qt. If a message is missing, check subsystem registration,
session/OnStart execution, the logger level, and the sink connection.

Project → Autoload edits an ordered list of `{name, script: {id, path}, enabled}`
entries in the `.sriproject` file. Names are ASCII identifiers, and names and
script UUIDs must be unique, including disabled entries. Missing script files do
not prevent opening a project. Files added through the dialog must be catalogued
`.lua` assets inside the project's content directory; declaration `.d.lua` files
and files without valid metadata are rejected. Script paths are logical hints;
UUID remains the identity.

The dialog edits a copy; Save writes the project atomically before replacing the
in-memory settings, and Cancel discards edits. It is unavailable during an active
scripting session. The editor registers one ScriptSubsystem after input and before
render extraction. Engine initialization prepares it without creating a Lua state;
Engine::Tick is the only update loop. Play/Stop controls the session.

`BeginPlay(contentRoot, project.autoloads, error)` validates the definitions and
resolves enabled entries by UUID using the owning Engine's shared asset catalog.
It copies each source/UUID before execution, checks canonical source containment
inside contentRoot (including symlinks/junctions), and never uses the path hint as
a fallback. Disabled entries do not resolve or execute their assets.

Once preflight succeeds, the subsystem creates one fresh state, then calls the
existing CreateInstance and StartInstance in list order. These instances have no
entity or ScriptComponent; Log and require are already available. Their IDs remain
in runtime startup order and EndPlay destroys them in reverse order before dropping
the state. Entity-owned instances finish before those services, so their OnDestroy
callbacks can still use session services. A startup failure cleans up the partial session and returns false; an
attempt to start while already playing preserves the active session. The original
BeginPlay(contentRoot, error) delegates with an empty autoload list and remains
usable without an owning Engine for the existing standalone runtime tests.

Autoload instances fill the optional `ScriptInstanceDesc::autoloadName`
field and leave entityId empty. Runtime diagnostics show `autoload=Name` in place
of `entity=...`, together with the source, script UUID, callback and Lua file:line.
Existing entity callers can keep using the three-field descriptor unchanged.

Logging has one owner per failure: LuaRuntime logs Lua load/callback failures;
ScriptSubsystem logs validation/catalog/root failures that never reached Lua.
BeginPlay returns that same error for the host to display in a status or dialog;
the host must not log it again. Preflight failures have no Lua source line.
Cleanup errors are separate diagnostics and do not overwrite the original startup
error. A failed startup leaves no state or instances; a later OnUpdate error faults
only that instance while the session and other scripts continue.

## Runtime lifecycle

### GameObject scripts

#### Exposed properties

Declare one top-level literal table (without `local`) to expose fields:

```lua
Properties = {
    horseName = "Horse",
    speed = 2,
    canRace = true
}

function OnStart()
    Log.info(Properties.horseName)
end

function OnUpdate(dt)
    if not Properties.canRace then return end
    local position = entity:GetPosition()
    position.x = position.x + Properties.speed * dt
    entity:SetPosition(position)
end
```

Select each horse and edit its Script → Properties fields. The scene stores only
that object's overrides. Reset removes an override and uses the current script
default. Strings, finite numbers and booleans are supported; fields use identifier
names. Nested tables, computed defaults and returned-table property declarations
are not exposed in this first version. Ordinary legacy scripts remain supported.

Defaults are read without running gameplay code in Edit mode. Saving the Lua file
refreshes the displayed schema within about half a second when the editor is active.
Text/number edits in progress are left alone until focus leaves the field.
Unknown or type-incompatible overrides remain saved and are shown for cleanup;
the runtime ignores them with a warning and uses the current default when present.
Selecting a different script clears overrides from the old script.

Every runtime instance receives a fresh Properties table. Overrides apply after
the file executes and before OnStart: read configurable values inside callbacks,
not into top-level locals. Changes made during Play are runtime-only; Stop restores
authored values. Editing is unavailable during Play, and source changes require a
new Play session. This does not implement hot reload or autoload property overrides.

Select a GameObject, choose **Add Component → Script**, and choose an imported Lua
asset in the Script picker (or drag one from the Content Browser onto the picker).
Each object has at most one Script component. Use **Open script** to open the file
in its associated external editor. Enabled and the asset reference are saved with
the scene, including empty slots and references to missing assets.

Press Play to run the script. Each object executes the source separately and owns
its own fields and callbacks, even when several objects use the same asset.
Autoloads have no `entity`. Object scripts receive a checked owner handle:

```lua
speed = 2

function OnStart()
    Log.info("Movement started")
end

function OnUpdate(dt)
    local position = entity:GetPosition()
    position.x = position.x + speed * dt
    entity:SetPosition(position)
end
```

Positions are local to the parent. GetPosition returns a Vector3 copy; changes
take effect only after SetPosition. `Vector3.new(x, y, z)` also constructs a value.
SetPosition rejects nonfinite values. `entity:IsValid()` becomes false after the
attachment is removed/replaced, its object is deleted, the scene changes, or Play
stops. Retained handles cannot address a later object that reuses an entity slot.

Disabled/empty attachments do not execute. A missing asset or a Lua error affects
only that attachment and is logged once; other objects continue. Removing an
attachment destroys its instance while the owner still exists. Native changes to
the asset/enabled flag are reconciled before the next scripting update; re-enabling
creates a fresh instance. OnDestroy follows the existing fault policy below.
Stop restores the original attachments, properties and transforms. The checked
component API below supports a fixed set of component names.

`ScriptSubsystem::Initialize()` prepares the subsystem without creating a Lua state.
After initialization, `BeginPlay(contentRoot, error)` starts a fresh Lua runtime;
the root must be an existing directory. Calling BeginPlay during Play fails and
preserves the active session. It passes the canonical absolute content root to
LuaRuntime::Initialize(root, error), which pins module loading to it. Hosts obtain
the configured content root from AssetPathResolver::GetGameRoot(), after setting
it with SetGameRoot(project.ContentDir()). The compatibility Initialize(error)
overload still works but leaves filesystem require unconfigured.

`EndPlay()` destroys all instances before releasing the state. It is safe to call
repeatedly; subsystem Shutdown also calls it. A subsequent BeginPlay creates a new
state. Pause only suppresses updates: it preserves the state, tables, and IDs.
The editor's Play/Stop actions own this lifecycle.

The persisted asset identity is `AssetReference::id`. Scene integration
resolves it with `GetCatalog().Find(reference.id, CatalogAssetKind::Script)`
and pass the resulting `CatalogEntry::source` to the runtime. The reference's path
is a diagnostic hint, never a fallback source. The runtime does not resolve assets.

`CreateInstance(desc, id, error)` accepts an absolute source path, the asset UUID,
and an entity ID for diagnostics. It executes the file in a fresh environment and
retains its environment or returned table. The chunk runs separately for every instance, so table
literals, including nested defaults, are created anew rather than shallow-copied.
An instance ID is opaque and transient. It combines a local slot and a unique
session token: slots may be reused after Stop, but old handles cannot address the
new session or a different runtime. Failed creation releases its temporary record
and leaves the output ID unchanged; no failed creation is scheduled for gameplay.

New scripts use top-level instance fields and functions, with no return wrapper:

```lua
health = 100
elapsed = 0
local damage = 10 -- private to this execution of the script

function OnStart()
    health = health - damage
    Log.info("Hello world")
end

function OnUpdate(dt)
    elapsed = elapsed + dt
end

function OnDestroy()
    Log.info("Stopped")
end
```

Fields, nested tables, local variables and function closures are private to each
instance. Names such as Log, Scene and require are inherited from the engine API.
Top-level callbacks receive no self argument; OnUpdate receives only dt in seconds.
Declare lifecycle functions without `local` so the engine can find them. A top-level
Properties table declares exposed Inspector fields as described above.
Callback lookup uses only the instance's own fields, so
missing functions never fall back to another script's or the runtime's callbacks.

For backward compatibility, scripts may still return one table:

```lua
return {
    OnStart = function(self)
        self.elapsed = 0
    end,
    OnUpdate = function(self, dt)
        self.elapsed = self.elapsed + dt
    end,
    OnDestroy = function(self)
        -- Release gameplay resources here.
    end
}
```

Callbacks are captured as protected functions at creation. Missing/nil callbacks
are allowed; other non-function values fail creation with the field name in the
error. For the returned-table format only, C++ passes the instance table explicitly
as `self`, followed by simulation delta seconds for OnUpdate. A script must either
return no values or return exactly one table; explicit nil, scalar and multiple
returns are rejected. Shared require modules retain their existing return rules.
The lifecycle names are OnStart, OnUpdate, OnFixedUpdate, and OnDestroy, plus the
physics and persistence hooks listed below; the unprefixed Start/Update/Destroy
fields are not instance callbacks.

`StartInstance` invokes OnStart at most once; repeating a successful start is a
successful no-op. A failed start faults the instance and cannot be retried.
`UpdateInstance` requires a successfully started, non-faulted instance.
`ScriptSubsystem::Update` updates started instances only during Play and with an
unpaused, positive simulation time step. It never starts or ends a session and
does not call the legacy global Update function.

A creation or callback failure faults only that instance. Automatic updates skip
faulted instances and continue updating others. All subsequent gameplay calls on a
faulted record return its saved error without running Lua or logging it again.
Authored enabled flags and source files are unaffected. A new Play session has no
fault state from the previous one.

Instance callbacks and compatibility callbacks use the same protected-call helper.
Load errors and call errors preserve the full Lua message and traceback. File chunks
are named with `@` plus the UTF-8 native path, preserving Cyrillic Windows filenames.
Instance errors add the callback, source, script UUID, and entity UUID or autoload name. The runtime's
SetError is the sole place that logs runtime failures; callers should not log the
returned message again. Clickable source locations are deferred to stage 07.

`DestroyInstance` marks the record destroying before calling Lua. OnDestroy runs
only for successfully started, non-faulted instances; faulted and unstarted records
skip it. Cleanup always releases callbacks, table, and environment even when
OnDestroy fails. Repeated destruction of a known ID in the current session succeeds
without invoking Lua again; invalid IDs and stale session tokens fail. EndPlay
performs the same cleanup for every record, clears bookkeeping, and only then
destroys the Lua state. Repeated and reentrant Stop calls are safe; an OnDestroy
failure cannot prevent cleanup of later instances or the state.

Instance environments isolate script globals and tables, but inherit the runtime's
global bindings; this is not a separate security sandbox. Instance data and Lua
references stay in `LuaRuntime::Impl` and are never serialized into a scene;
SaveGame persists only explicitly opted-in hook results as described below.
`ExecuteString`, `ExecuteFile`, `CallFunction`, and `CallUpdate` remain available
for compatibility; gameplay callbacks use the instance API.

## Shared modules

Use `require('shared.settings')` for shared data or helpers, not to instantiate the
main behavior file. It checks `<content>/shared/settings.lua` first, then
`<content>/shared/settings/init.lua`. Each name segment is an ASCII identifier:
letters or underscore first, followed by letters, digits, or underscores. Empty
segments, separators, traversal, drive/UNC paths, and absolute names are rejected.

The C++ loader canonicalizes candidates and compares root path components before
reading. Symlinks and Windows junctions may point inside the canonical content
root, but cannot escape it. Resolution never uses the working directory. The
package library is not opened, so no native searchers, loadlib, or mutable search
paths are available. dofile/loadfile remain disabled. This restricts module
loading; it is not a promise of a complete Lua security sandbox.

Modules execute in their own environments with access to the shared engine API.
Their results and environments are cached separately from gameplay instances.
Repeated require of the same name returns the same result during one session,
including false. A module that returns nil or no value is cached as true. A
module should normally return a table. Cycles report the loading chain; failed
loads leave no loading marker or cached result for that name and can be retried.
File and Lua errors become ordinary Lua errors that can be caught with pcall;
uncaught errors use the runtime's existing protected-call diagnostics.

Destroying an instance does not unload its shared modules. Stop destroys instances
first (so healthy OnDestroy callbacks can still require modules), then releases
the module cache before destroying the Lua state. A new Play loads fresh modules.
Behavior chunks still execute independently per instance, including nested defaults.

New scripts created by the editor declare top-level instance fields and functions
OnStart/OnUpdate/OnDestroy, plus a commented shared-module require example.

## Lua API reference

Global services use dot calls (`Physics.AddImpulse(entity, v)`). Entity and input
context methods use colon calls (`entity:GetPosition()`). Unless a return contract
is listed explicitly, invalid handles, unsupported component names, invalid field
types, and nonfinite transform/component values raise Lua errors; use `pcall` when
you need to handle these locally. An uncaught error follows the instance fault
policy above. Physics and queued service requests instead return diagnostics.

### Math values

* `Vector2.new()` / `Vector2.new(x, y)`; mutable `x`, `y` fields.
* `Vector3.new()` / `Vector3.new(x, y, z)`; mutable `x`, `y`, `z` fields.
* `Quaternion.new()` / `Quaternion.new(x, y, z, w)`; mutable `x`, `y`, `z`, `w`
  fields. The default quaternion is identity; default vectors are zero.

Both vector types implement `a + b`, `a - b`, `-a`, `a * scalar`, `scalar * a`,
`a / scalar`, and exact value equality (`==`). Division requires a finite nonzero
scalar. Methods are `v:Length()`, `v:LengthSquared()`, `v:Normalize()`,
`v:Normalized()`, `v:Dot(other)`, and `v:Lerp(other, t)`; Vector3 also has
`v:Cross(other)`. **Normalize and Normalized both return a new value**; neither
changes the receiver. Normalization rejects zero and nonfinite values. Math
functions can also be called with explicit operands, e.g. `Vector3.Dot(a, b)`.

Quaternion factories are `Quaternion.Identity()`,
`Quaternion.Euler(pitch, yaw, roll)`, `Quaternion.YawPitchRoll(yaw, pitch, roll)`,
and `Quaternion.AxisAngle(axis, radians)`. All angles are radians; AxisAngle
normalizes a finite nonzero axis. Methods are `q:Length()`, `q:Normalize()`,
`q:Normalized()`, `q:Slerp(other, t)`, `q:Rotate(vector)`, and `q:Compose(other)`.
Quaternion normalization returns a copy. `q * otherQuaternion` composes rotations;
`q * vector3` rotates a vector. Rotate, Compose, multiplication and Slerp normalize
their quaternion operands; Slerp also requires finite `t` and normalizes its result.

### Entity identity, transforms, tags, and hierarchy

Entity handles have no Lua constructor. Obtain them from `entity`, scene lookup,
spawning, hierarchy methods, or physics callbacks. `a == b` is true only for valid
handles to the same scene entity. A failed single-entity scene lookup or a missing
parent returns an **invalid handle**, not nil: check `handle:IsValid()`.

| Methods | Contract |
| --- | --- |
| `IsValid()` | Safe lifetime/identity check. Owner handles also expire when their script attachment is destroyed. |
| `GetId()` | Persistent entity UUID string; distinct from a script asset UUID. |
| `GetName()`, `SetName(name)` | Read/write the object name. |
| `GetPosition()`, `SetPosition(v)` | Parent-local Vector3 position. |
| `GetRotation()`, `SetRotation(q)` | Parent-local Quaternion; setter normalizes finite nonzero input. |
| `GetEulerAngles()`, `SetEulerAngles(v)` | Parent-local rotation as a Vector3 of X/Y/Z degrees, matching the inspector. |
| `GetScale()`, `SetScale(v)` | Parent-local Vector3 scale. |
| `GetWorldPosition()`, `SetWorldPosition(v)` | World-space position. |
| `GetWorldRotation()`, `SetWorldRotation(q)` | World-space rotation; setter normalizes finite nonzero input. |
| `GetWorldScale()`, `SetWorldScale(v)` | World-space scale. |
| `Forward()`, `Right()`, `Up()` | World-rotation directions; forward is local -Z, right +X, up +Y. |
| `GetTags()` | Copy of the string tag list. |
| `HasTag(tag)`, `AddTag(tag)`, `RemoveTag(tag)` | Query/edit tags. |
| `GetParent()`, `GetChildren()` | Parent handle or array of child handles. |
| `SetParent(parentOrNil)` | Preserve local transform; nil unparents; returns boolean. |
| `SetParentKeepingWorld(parentOrNil)` | Preserve world transform; nil unparents; returns boolean. |

Transform getters return copies. World operations require a decomposable TRS
transform: unsupported shear and singular parent transforms raise errors.
Reparenting rejects invalid/cross-scene parents and cycles.

### Scene lookup and mutation

* `Scene.FindByName(name)` returns the first match as a checked handle.
* `Scene.FindAllByName(name)` and `Scene.FindByTag(tag)` return arrays of handles.
* `Scene.FindById(uuid)` and `Scene.FindByUuid(uuid)` find a persistent entity UUID.
* `Scene.Spawn(name, options?)` creates and returns an entity immediately. Options
  are `position` (Vector3, zero), `rotation` (Quaternion, identity), `scale`
  (Vector3, one), `parent` (Entity), and `components` (table keyed by supported
  component name). Transforms are parent-local. Component values are validated
  before creation; script attachment changes are deferred.
* `Scene.Destroy(target)` requests destruction of the entity and its descendants.
* `Scene.AttachController(target, type, data?)` queues a native C++ controller
  definition with version 1 and enabled true; `type` must be registered. Data must
  be serializable (string-keyed objects or contiguous 1-based arrays, finite
  scalar values, no unsupported userdata/functions or mixed/sparse keys).
* `Scene.OpenLevel(name)` returns `success, error`; level resolution and deferred
  application are described at the start of this document.

```lua
local lamp = Scene.Spawn("Runtime lamp", {
    position = Vector3.new(0, 2, 0),
    components = {
        PointLight = { color = Vector3.new(1, 0.8, 0.5), intensity = 3, range = 8 }
    }
})
lamp:AddTag("runtime")
```

### Component snapshots

`target:HasComponent(type)` returns a boolean. `target:GetComponent(type)` returns
a detached table or nil if absent. Editing that table does not edit the scene:
commit with `target:SetComponent(type, data)`. Set patches an existing component;
omitted/nil fields retain their values. `target:AddComponent(type, data?)` requires
the component to be absent and uses defaults for omitted fields. Script and
MeshRenderer additions require asset data. `target:RemoveComponent(type)` returns
false if absent, otherwise true for a queued removal. Unsupported types and unknown
fields are errors. There remains **one Script component per object**.

| Type string | Snapshot/patch fields |
| --- | --- |
| `Camera` | `fovY`, `nearPlane`, `farPlane` (numbers); `0 < fovY < pi`, `0 < nearPlane < farPlane`. |
| `DirectionalLight` | `color` (Vector3 RGB), `intensity` (number), all nonnegative. |
| `PointLight` | DirectionalLight fields plus positive `range`. |
| `SpotLight` | PointLight fields plus radian `innerHalfAngle`, `outerHalfAngle`; `0 <= inner <= outer < pi/2`, outer positive. |
| `MeshRenderer` | `mesh`, `materialInstance` (asset UUID strings on read; catalogued UUID or logical path on write). |
| `RigidBody` | `motionType` (`"Static"`, `"Dynamic"`, `"Kinematic"`), positive `mass`, nonnegative `friction`, `restitution` in [0, 1]. |
| `Collider` | `shape` (`"Cube"`, `"Box"`, `"Sphere"`, `"Capsule"`, `"ConvexMesh"`, `"TriangleMesh"`), `center` (Vector3), positive `halfExtents` (Vector3), positive `radius`, nonnegative `halfHeight`, `showDebug`, `isTrigger` (booleans). |
| `Script` | `script` (asset UUID/logical path), `enabled` (boolean), `properties` (string-keyed string/finite-number/boolean overrides). A supplied properties table replaces the override map. |
| `GamePersistence` | `transform`, `controllerState`, `scriptState` (booleans). `scriptState` defaults to false; see persistence opt-in below. |
| `Bounds` | `min`, `max` (Vector3), with min <= max per axis. |

Component fields are validated as a complete resulting component before commit.
Asset writes resolve against the owning Engine's catalog with kind checking.
Numbers must be finite and representable; vector fields require actual math values,
not `{x=..., y=..., z=...}` tables. Physics may impose further restrictions on
otherwise valid authored component combinations; see [physics.md](physics.md).

`target:SetActiveCamera()` requires a Camera component;
`target:IsActiveCamera()` reports whether the entity is the active camera.
`target:GetMaterialParameters()` returns `{specularColor = Vector3, shininess = number}`.
`target:SetMaterialParameters({specularColor = ..., shininess = ...})` patches an
entity-local material override, using nonnegative values. It does not modify shared
assets. Get requires a MeshRenderer and ready rendering/material resources.

### Game UI

See the [UI guide](ui.md) and [exact Lua facade reference](ui-lua-facade.md) for
documents, models, event callbacks, ownership and paused menus.

### Input contexts and named bindings

`Input.GetContext(name)` and its alias `Input.DefineContext(name)` get/define a
nonempty named context and activate it nonblockingly. They return nil if the input
subsystem is unavailable. Retain the returned context while using it. Calls for
the same live name share the context wrapper and its activation.

```lua
local controls
function OnStart()
    controls = Input.GetContext("Gameplay")
    if not controls then return end
    controls:BindAction("Jump", PhysicalKey.Space)
    controls:BindMouseAction("Fire", MouseButton.Left)
    controls:BindAxis1D("Move", PhysicalKey.A, -1)
    controls:BindAxis1D("Move", PhysicalKey.D, 1)
    controls:BindAxis2D("Move2D", PhysicalKey.W, Vector2.new(0, 1))
end

function OnUpdate(dt)
    if Input.Pressed("Jump") then Log.info("Jump") end
    local movement = Input.Axis1D("Move")
end
```

Context methods:

* `IsValid()` checks activation lifetime.
* `BindAction(name, key)`, `BindMouseAction(name, button)`,
  `BindAxis1D(name, key, value)`, `BindAxis2D(name, key, vector2)` add bindings;
  axis contributions must be finite.
* `Pressed(name)`, `Released(name)`, `Down(name)` poll named actions.
* `Axis1D(name)` returns a number; `Axis2D(name)` returns Vector2.
* `MouseDelta()` returns Vector2; `MouseWheel()` returns a number.
* `MousePosition()` returns Vector2 in render-surface pixel coordinates, with
  origin at the top-left and +y down. Position persists across frames without
  movement. Blocked, released, or expired contexts return zero. Focus loss clears
  position on the next input latch.
* `Capture(priority?, keyboard?, mouse?)` replaces the activation and returns
  success. Defaults are priority 0, keyboard false, mouse false. Blocking is
  explicit and participates in host input-context arbitration.
* `Release()` and its alias `Reset()` release activation, preserving definitions;
  `Capture()` can reacquire it. Sharing a context also shares Release/Capture.

The global `Input.Pressed/Released/Down/Axis1D/Axis2D/MouseDelta/MousePosition/MouseWheel` functions
poll the `Gameplay` context. Without input they return false/zero/zero-vector.
Polling uses **binding names**, not physical key constants.

`PhysicalKey` (also `Input.PhysicalKey`) exposes A–Z, Num0–Num9, F1–F12, Enter,
Escape, Backspace, Tab, Space, Delete, Right, Left, Down, Up, NumpadEnter,
LeftControl, LeftShift, LeftAlt, LeftMeta, RightControl, RightShift, RightAlt,
RightMeta. `MouseButton` (also `Input.MouseButton`) exposes Left, Right, Middle.
There is currently no Lua gamepad or raw text-input polling API; text entry is
handled through UI controls.

### Cursor modes

`Input.SetCursorMode(mode)` requests a cursor mode for the game in both standalone
and editor Play. `CursorMode` and `Input.CursorMode` refer to the same constants:

| Mode | Behavior while gameplay owns the pointer |
| --- | --- |
| `Normal` | Visible pointer with absolute position and movement. |
| `Hidden` | Invisible pointer that still moves freely. |
| `Locked` | Invisible pointer held in place for mouse-look; use `Input.MouseDelta()`. |

```lua
function OnStart()
    Input.SetCursorMode(CursorMode.Locked)
end
```

`Input.GetCursorMode()` returns the **requested** mode, even while UI or focus
temporarily overrides it. An interactive menu, UI pointer capture, or text entry
suspends gameplay hiding/locking; closing the UI restores the request automatically.
A passive HUD does not override it. Releasing editor game input or losing window
focus releases the cursor without forgetting the request.

The request is shared by gameplay scripts and lasts until changed or the session
ends. It survives `Scene.OpenLevel` and input-state resets. Stop and failed startup
reset it to `Normal`. Loading a saved game also resets it before the replacement
session's restore/start callbacks, which can request a new mode. The mode itself is
not saved; request it from `OnStart` or derive it from restored game state.

`SetCursorMode` returns no value and rejects an invalid mode with a Lua error.
Without an input subsystem, setting a valid mode has no effect and the getter
returns `CursorMode.Normal`.

### Time and physics

`Time.Delta()` and `Time.SimulationTime()` return simulation frame delta and
elapsed simulation seconds. `Time.RealDelta()` and `Time.RealTime()` return real
frame delta and elapsed real seconds; `Time.IsPaused()` reports Engine pause.
Without an owning Engine these return zero/false. `OnUpdate(dt)` receives the
simulation frame delta; `OnFixedUpdate(dt)` receives each fixed-step delta. Use
the fixed callback's argument for fixed-step work: Time.Delta is the frame delta.
Paused simulation does not run ordinary updates or fixed steps.

All physics commands return `success, error` (empty string on success):

| Function | Contract |
| --- | --- |
| `Physics.SetLinearVelocity(target, velocity)` | Finite Vector3 world velocity; requires a dynamic body. |
| `Physics.AddImpulse(target, impulse)` | Finite Vector3 world impulse; requires a dynamic body. |
| `Physics.MoveKinematic(target, position, rotation)` | Kinematic body's parent-local target pose, consumed by physics stepping. |
| `Physics.Teleport(target, position, rotation)` | Immediately changes an existing body's parent-local pose and invalidates prior contacts. |

Targets must be valid entities in the current scene with usable physics bodies.
Pose commands require a finite Vector3 and a finite, already-normalized Quaternion
(squared length within 1e-4 of one). Missing subsystems, unsuitable bodies, and
invalid arguments return false plus a diagnostic.

Optional object-script physics callbacks are `OnCollisionEnter(other, contact)`,
`OnCollisionExit(other, contact)`, `OnCollisionStay(other, contact)`,
`OnTriggerEnter(other, contact)`, `OnTriggerExit(other, contact)`,
`OnTriggerStay(other, contact)`, `OnSleep()`, and `OnWake()`. Autoloads do not
receive entity contact/activity callbacks. `other` is an Entity or nil if already
removed. `contact` contains `point` (Vector3), receiver-oriented `normal` (Vector3),
`stepIndex` (integer), and `otherId` (transient numeric EnTT ID, **not a UUID**).
Returned-table scripts receive self before these arguments. Physics callback faults
follow the normal per-instance fault policy.

### Calling another object's script

`target:HasScript()` checks for a live, started, non-faulted receiver.
`target:HasScriptMethod(name)` checks for an available public function.
`target:CallScript(name, ...)` invokes it synchronously and returns **true followed
by all of the function's results**, or **false followed by an error string**.
Top-level functions receive the supplied arguments; returned-table methods receive
their instance table as self first.

```lua
local receiver = Scene.FindByName("Door")
if receiver:IsValid() and receiver:HasScriptMethod("Open") then
    local ok, result = receiver:CallScript("Open", "bronze-key")
    if not ok then Log.warning(result) end
end
```

Empty names, lifecycle/physics/persistence hook names, and SaveVersion are reserved.
Missing, unstarted, destroying, faulted, or stale receivers fail safely.
Attachment enable/asset changes take effect when the subsystem reconciles them;
disabling an attachment is not an immediate communication barrier within a callback.
Calling an already-active receiver fails (including A → B → A recursion).
HasScriptMethod returns false for an active receiver. A user-method exception faults
the receiving instance; lookup/reserved-name/reentry rejection does not invoke it.
Use `Autoloads.Name` for direct communication with named session services.

### SaveGame requests and explicit script state

```lua
local pendingSave
function SaveCheckpoint()
    local request, err = SaveGame.RequestSave("checkpoint-1")
    if not request then Log.warning(err); return end
    pendingSave = request
end

function OnUpdate(dt)
    if pendingSave and pendingSave:IsComplete() then
        local ok, err = pendingSave:GetResult()
        if not ok then Log.warning(err) end
        pendingSave = nil
    end
end
```

`SaveGame.RequestSave(slot)` and `SaveGame.RequestLoad(slot)` return `request, error`:
nil plus a diagnostic on rejection, otherwise a queued request handle. Requests
require an active session and cannot be queued from persistence hooks. Slot names
contain **1–64 ASCII letters, digits, underscores or hyphens**; a leading digit is
allowed. They are not paths: separators, traversal and absolute paths are rejected.
The default save directory is `contentRoot.parent_path() / ".sri/saves"`
(the project's `.sri/saves` for the usual project layout); each slot is written as
`<slot>.save.json`. A C++ host can override the directory with
`Engine::SetSaveDirectory(directory)`; passing an empty path restores the default.
Acceptance means queued, not successful disk I/O or restore. Poll
`request:IsComplete()` and then `request:GetResult()` for `success, error`.
Before completion, GetResult returns `false, "Request is pending"`.
Operations run at an Engine safe point after gameplay callbacks. A load replaces
the Lua session: entity scripts and autoloads are freshly created, and shared
modules reset too. Do not depend on either the requesting entity or an autoload's
old request field surviving a load. Preflight validation failures leave the live
scene/session intact. Ending a session cancels its other pending requests.

Lua state is **opt-in**, separate from scene serialization and Inspector Properties.
Entity scripts require `GamePersistence.scriptState = true` in their persistence
component and both hooks. Enable **Save script state** in the Game Persistence
Inspector, or set the Lua component field. Autoloads opt in by defining the hook pair. Defining
only one hook fails instance creation. `SaveVersion` is a positive integer,
defaulting to 1, captured when the instance is created.

```lua
SaveVersion = 2
score = 0
restoredTargetId = nil -- optional persistent entity UUID, never an Entity handle

function OnSaveGame()
    return { score = score, targetId = restoredTargetId } -- exactly one table
end

function OnLoadGame(data, savedVersion)
    restoredTargetId = data.targetId -- retain the saved UUID for lookup after restore
    if savedVersion == 1 then
        score = data.points or 0 -- migrate the older representation
    else
        score = data.score or 0
    end
end
```

Loads accept positive saved versions **less than or equal to current SaveVersion**;
the script performs any older-version migration in `OnLoadGame(data, savedVersion)`.
Future versions are rejected. Returned-table scripts declare these fields on the
returned table and receive self first. Save results support booleans, finite
numbers (including Lua integers), strings, nested string-keyed objects, and
contiguous 1-based arrays. Empty `{}` represents an empty object. Mixed/sparse
keys, cycles, depth beyond 64, functions, threads, userdata (including Entity,
Vector3 and Quaternion), and nonfinite numbers are rejected. Encode math values
as scalar tables and entity references as UUID strings. Nil is not a persistent
table entry; saved nested null values cannot be restored as Lua entries.

Save data records script asset identity and version. Preflight validates saved
autoload names/assets, entity UUID/script asset/opt-in identities, and compatible
versions before replacing the live scene. The new session creates fresh autoload
and entity instances, restores native controller state, invokes initial
`OnLoadGame` hooks, and then starts scripts and controllers. Keep restored values
intact in OnStart rather than resetting them unconditionally; resolve saved UUIDs
with `Scene.FindById`, checking the resulting handle with IsValid.

A Lua load-hook error fails the load request but does not fault the script
instance. The fresh session starts normally so gameplay can continue (subject to
the usual startup-error policy); this is not a rollback to the old session, and
earlier hooks may already have restored their data. Transform, native controller
state, and script state have independent persistence opt-ins.

### Deferred mutation and teardown

The scripting subsystem observes component removal and entity destruction, so
OnDestroy runs before the corresponding owner data is removed. The observer and
physics-event subscriptions are removed on teardown.

Lua callbacks run inside a scene mutation guard. Destruction is queued; all
`RemoveComponent` calls, Script component add/set operations, and native controller
attachments use deferred commands. Ordinary transform changes, non-Script
component add/set operations, and entity creation are immediate. Commands flush
at explicit safe points around startup, update, and fixed-update batches after
Lua callback stacks have unwound, including nested script calls. Do not assume a
queued removal/attachment/destruction is visible in the calling callback. Queued
operations recheck entity identity before use, and pending commands are discarded
on scene/session teardown. Scene.OpenLevel and SaveGame requests have their own
Engine safe-point processing as described above.
