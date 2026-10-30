# Scripting sessions and instances

`ScriptSubsystem::Initialize()` prepares the subsystem without creating a Lua state.
After initialization, `BeginPlay(contentRoot, error)` starts a fresh Lua runtime;
the root must be an existing directory. Calling BeginPlay during Play fails and
preserves the active session. The root is retained for future asset integration.

`EndPlay()` destroys all instances before releasing the state. It is safe to call
repeatedly; subsystem Shutdown also calls it. A subsequent BeginPlay creates a new
state. Pause only suppresses updates: it preserves the state, tables, and IDs.
Button wiring and scene/asset integration are separate steps.

The persisted asset identity is `AssetReference::id`. The future scene integration
must resolve it with `GetCatalog().Find(reference.id, CatalogAssetKind::Script)`
and pass the resulting `CatalogEntry::source` to the runtime. The reference's path
is a diagnostic hint, never a fallback source. The runtime does not resolve assets.

`CreateInstance(desc, id, error)` accepts an absolute source path, the asset UUID,
and an entity ID for diagnostics. It executes the file in a fresh environment and
retains its returned table. An instance ID is opaque, transient, and never reused
across runtimes or Play sessions. Failed creation leaves the output ID unchanged.

Scripts return one table with optional callbacks:

```lua
return {
    Start = function(self)
        self.elapsed = 0
    end,
    Update = function(self, dt)
        self.elapsed = self.elapsed + dt
    end,
    Destroy = function(self)
        -- Release gameplay resources here.
    end
}
```

Callbacks are captured at creation. `StartInstance` starts once; repeating it is a
successful no-op. `UpdateInstance` requires a started, non-faulted instance.
`ScriptSubsystem::Update` updates started instances only during Play and with an
unpaused, positive simulation time step. It never starts or ends a session and
does not call the legacy global Update function.

A callback failure faults only that instance. Automatic updates skip faulted
instances and continue updating others. Errors include the source, asset UUID,
and entity ID. `DestroyInstance` calls Destroy for started instances, even when
faulted, and releases the record even if Destroy fails. Unstarted instances are
released without a Destroy callback. Invalid/stale IDs fail. EndPlay performs the
same cleanup for all remaining instances and logs callback failures.

Instance environments isolate script globals and tables, but inherit the runtime's
global bindings; this is not a separate security sandbox. Instance data and Lua
references stay in `LuaRuntime::Impl` and are never serialized into a scene.
`ExecuteString`, `ExecuteFile`, `CallFunction`, and `CallUpdate` remain available
for compatibility; gameplay callbacks use the instance API.
