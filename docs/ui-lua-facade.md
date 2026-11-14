# Lua UI facade

The engine-owned facade runs in the existing script state. Register UI in `OnStart`
or a later live instance callback. Required modules inherit the caller; `Entity.CallScript`
uses the receiver's owner, as do methods invoked through `Autoloads.Name`.
Top-level script chunks and unowned global runtime calls cannot register UI.
Every resource mutation checks the active owner, so passing a handle to another script does
not grant access. No UI pointers or handle fields are exposed to Lua.

## Exact API

- `UI.CreateModel(name, initialValues, schema?)` → `model | nil, error`
- `UI.LoadDocument(logicalPath | {id = uuid, path = diagnosticHint?})` → `document | nil, error`
- `document:Show(mode?)`, `:Hide()`, `:Close()`, `:Reload()` → `boolean, error`
  (`mode` is `"hud"`, `"interactive"` (default), or `"modal"`).
- `document:IsValid()`, `:IsVisible()` → boolean
- `document:GetElement(id)` → `element | nil, error`
- `document:On(elementId, eventName, function(event) ... end)` → `subscription | nil, error`
- `model:Get(variable)` → deep-copied value or `nil, error`
- `model:Set(variable, value)` → `boolean, error`
- `element:IsValid()` → boolean
- `element:SetText(text)`, `:SetInnerRml(rml)`, `:SetValue(string)`,
  `:SetClass(name, enabled)`, `:SetProperty(name, value)`, `:Focus()` → `boolean, error`
- `element:GetValue()` → form string or `nil, error`
- `subscription:Disconnect()` → `boolean, error`
- `Time.SetPaused(boolean)` → `boolean, error`

Errors are empty strings on success. Creation without an initialized UI subsystem returns
`nil` and a useful unavailable error. The usertypes have no public constructors. The fixed
native contract has no Lua debugger operation. The host enables the initially hidden
RmlUi debugger in Debug builds; F8 toggles it outside text entry. Non-Debug builds
disable it.

Documents load hidden. Models created by the same owner become explicit document dependencies.
An AssetReference loads by UUID exclusively; its path is diagnostic. `Reload` acknowledges a
queued request; completion diagnostics go to engine logs. Success preserves the document and
subscriptions but invalidates old elements; default parser warnings or errors roll the
replacement back. `SetText` escapes markup; `SetInnerRml` explicitly
inserts markup. Prefer model binding for frequently changing values.

`event` contains `document`, `sourcePath`, `targetId`, `currentTargetId`, `type`,
`inputSequence`, and a copied `parameters` table. Callbacks receive this single argument,
without an implicit script self. Discarding a subscription handle does not disconnect it.
Callbacks run in UI Update before gameplay, including while paused. Pause changes affect
subsequent simulation scheduling; they do not undo work already performed.

## Schema and arrays

Values are copied, not observed by reference. Root values and schema must be objects.
Schema scalars are `"boolean"`, `"number"`, `"string"`; object schema is
`{type="object", fields={...}}`; array schema is `{type="array", element=...}`.

```lua
local inventory = assert(UI.CreateModel("inventory", {items = {}}, {
    type = "object", fields = {
        items = {type = "array", element = {
            type = "object", fields = {name = "string", count = "number"}
        }}
    }
}))
assert(inventory:Set("items", {{name="Potion", count=2}}))
assert(inventory:Set("items", {})) -- retains Array, including nested declared arrays
```

Without an explicit schema, shapes infer recursively and `{}` means an empty object.
Arrays must be dense and homogeneous. Lua indexing is 1..N; Rml expressions use 0..N-1.
Objects retain exactly their declared fields; array lengths may change freely. Nil/null,
cycles, sparse/mixed tables, functions/userdata, nonfinite numbers, and integers that cannot
round-trip exactly through double are rejected. Conversion uses StateValue's depth limit (64).
Get returns a deep copy and Set validates before updating native storage.

## Lifetime

Owners begin dormant and become enabled immediately before `OnStart`. Callback failures
retire their UI and release callback references, including failed `OnStart`. `OnDestroy`
may mutate/close existing UI but cannot create models/documents/subscriptions. Remaining UI
is released unconditionally after it. Autoload UI survives level changes.

EndPlay suspends the whole UI session before entity teardown. Shutdown ends the session
and clears all callback references before destroying the Lua state, including failed startup,
prepared startup rollback, and LoadGame replacement through the same EndPlay/Shutdown path.
Direct Shutdown requested inside a callback suspends immediately and completes at the next
safe runtime command-flush boundary. No executing Lua state is reset.
