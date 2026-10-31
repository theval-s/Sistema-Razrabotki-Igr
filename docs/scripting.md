# Scripting sessions and instances

## Play current scene

Use **Play Current Scene** on the toolbar (F5), **Pause** to suspend simulation,
and **Stop** (Shift+F5) to return to editing. Play uses the current in-memory scene,
including unsaved edits; it does not compile C++ or write the scene to disk.
Autoloads start before entity scripts and native controllers. Stop restores the original scene,
selection and unsaved-change status, even after visiting other levels. Authoring
panels and scene/project file actions are disabled during Play. Closing the editor
stops the game before offering to save the restored editing scene.

During Play the scene camera drives rendering. When a level has no camera the
renderer retains the last viewpoint (the editor viewpoint on initial Play), with
a warning on initial startup. The editor viewpoint is restored on Stop. Escape
releases viewport focus; right-click no longer captures the editor navigation
cursor during gameplay.

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
Engine keeps its existing lifecycle unless its host opts into SetEditorMode().

## Editor logging and autoload settings

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
Stop restores the original attachments and transforms. Script Properties in the
Inspector and general ECS get/add/has bindings remain separate work.

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
Properties table can describe future Inspector fields; Inspector integration is
still a separate feature. Callback lookup uses only the instance's own fields, so
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
The names are OnStart, OnUpdate, and OnDestroy; the
unprefixed Start/Update/Destroy fields are not instance callbacks.

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
references stay in `LuaRuntime::Impl` and are never serialized into a scene.
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

Stage 06 will use Scene::AddLifecycleObserver for ComponentRemoving and
EntityDestroying, while the component/entity data is still accessible. Store the
subscription token and remove it with RemoveLifecycleObserver before the captured
subsystem state dies. No scripting observer is registered yet. Lua-driven scene
mutations will need a safe-operation queue: Scene's current queue protects C++
Controller callbacks only, not Lua callbacks.
