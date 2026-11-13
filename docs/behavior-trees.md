# Behavior trees in Lua

The sample assets and their metadata are in `example_game/ai`. They are copied
to `game/ai` alongside the ExampleGame executable. To use them in another
project, copy the directory into its game content root.
The demo scene attaches the same tree to Enchanted cube and Editor cube.
Their pulse messages appear in the game log at different intervals.

Create a cataloged `.bt.lua` asset that returns a tree definition. Set its metadata
kind to `behaviorTree`. Conditions and actions are cataloged as ordinary
`script` assets. The suffixes `.condition.lua` and `.action.lua` describe their
purpose. References can use catalog UUIDs or logical paths.

## Define a tree

```lua
-- pulse.bt.lua
return {
    root = "main",
    nodes = {
        { id = "main", kind = "Selector", children = {"pulse", "idle"} },
        { id = "pulse", kind = "Sequence", children = {"enabled", "wait"} },
        { id = "enabled", kind = "Condition", script = "game/ai/enabled.condition.lua" },
        {
            id = "wait", kind = "Action", script = "game/ai/pulse.action.lua",
            parameters = { seconds = 1 },
        },
        { id = "idle", kind = "Action", script = "game/ai/idle.action.lua" },
    },
}
```

```lua
-- enabled.condition.lua
return {
    Check = function(ctx)
        return ctx.parameters.pulseEnabled == true
    end,
}
```

```lua
-- pulse.action.lua
local elapsed
return {
    OnEnter = function(ctx)
        elapsed = 0
    end,
    Tick = function(ctx, dt)
        elapsed = elapsed + dt
        if elapsed < ctx.parameters.seconds then
            return BT.Running
        end
        ctx.blackboard.pulses = (ctx.blackboard.pulses or 0) + 1
        Log.info(ctx.entity:GetName() .. " pulse " .. ctx.blackboard.pulses)
        return BT.Success
    end,
    OnExit = function(ctx, reason)
        if reason == "Cancelled" then Log.info(ctx.entity:GetName() .. " stopped") end
    end,
}
```

```lua
-- idle.action.lua
return { Tick = function() return BT.Success end }
```

Each leaf receives the owning entity, a parameter table, and its agent's shared
blackboard. Parameters accept strings, booleans, and finite numbers. Per-agent
overrides take precedence over node defaults. Leaf-local variables belong to
one node of one agent. Lua modules imported with `require` can share state across
agents through the session cache.

Sequences advance on success. Selectors advance on failure. Both remember a
running child, so earlier conditions run again when the branch restarts. A
terminal root result remains visible until the next scheduled update, when the
tree starts from its root. `OnExit` runs once for each entered action with
`Success`, `Failure`, `Cancelled`, or `Faulted`.

## Attach and control agents

In the editor, select an entity and choose **Add Component → Behavior Tree**.
Select the `.bt.lua` asset in **Tree**. Use **Enabled** to start or stop the
tree and **Apply Parameters** to apply the entity's JSON overrides. For the
pulse demo, enter `{"pulseEnabled": true, "seconds": 1}`. Save the scene to
keep the attachment and its settings. To create a Lua definition with metadata,
use **New → Behavior Tree** in the content browser and update the starter
action's script path before Play.

From a gameplay script, attach the same tree to two scene entities.

```lua
function OnStart()
    local first = Scene.FindByName("First")
    local second = Scene.FindByName("Second")
    BT.Attach(first, "game/ai/pulse.bt.lua", {pulseEnabled = true, seconds = 1})
    BT.Attach(second, "game/ai/pulse.bt.lua", {pulseEnabled = true, seconds = 2})
end
```

- `BT.Attach(entity, asset, parameters?)` attaches or replaces a tree.
- `BT.SetParameters(entity, values)` replaces the override table. Running
  actions receive the new values at their next tick.
- `BT.SetEnabled(entity, false)` stops the tree. Enabling it starts a fresh instance.
- `BT.Detach(entity)` removes the attachment and cancels the active action.
- `BT.GetStatus(entity)` returns the latest result and the active action ID
  (an empty string when idle). It can also return `Pending`, `Stopped`, or `Faulted`.

Mutations are deferred until the current callback batch finishes. An authored
scene component stores the tree reference, enabled state, and overrides. A Play
session starts fresh runtime state, including after loading a saved game.

## Check responsiveness

During Play, change one agent's `seconds` while it is running, disable or remove
the other agent, and press Stop while an action is active. Sample
`BT.GetTiming()` before stopping to record the update count, mean milliseconds,
and maximum milliseconds for the Play session. Record FPS alongside those
figures for the demonstration in §4.1.8. Actions
should return `Running` between steps. Each callback should do a small amount of
work and return control to the engine.

## Roadmap

1. **Add BT executor** — validate definitions and execute independent,
   resumable C++ instances.
2. **Integrate Lua BT** — add asset and scene support, the Lua leaf adapter,
   attachment controls, and Play-session scheduling.
3. **Verify BT behavior** — cover native execution and Lua lifecycle with
   regression tests and document the two-agent demonstration.

For GOAP, reuse the agent context and action lifecycle. Keep world snapshots,
action preconditions and effects, and minimum-cost search in a separate planner.
