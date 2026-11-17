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

## Edit a tree in the editor

Double-click a `.bt.lua` asset in **Content** to open it in the **Behavior Tree**
panel. On an entity's **Behavior Tree** component, **Show Tree Graph** opens its tree.
The graph runs from the top down, and each composite numbers its children in run
order. Colors show node kinds. A red border marks a node that would stop the tree
from loading. Hover over the node or select it to see why.

- Right-click empty space to add a node. The first node of an empty tree becomes its root.
- Right-click a node to add a child to it, make it the root, or delete it.
- Drag from a composite's lower port to another node's upper port to run that node
  under it. Each node runs under one parent. Drag a link's lower end away to remove it.
- Children run from left to right. Drag a child sideways to change the order.
  **Arrange** lays the tree out again.
- The side panel edits the selected node's ID, kind, script and parameters.
  Double-click an action or condition to open its script.
- With the graph focused, Ctrl+C, Ctrl+V and Ctrl+D copy, paste and duplicate nodes
  with the links between them. Delete removes them, and Ctrl+Z undoes graph edits.

**Save** (Ctrl+S in the panel) rewrites the whole file in the editor's own layout.
Comments and formatting are lost, so the panel asks first if the file wasn't
written by the editor. Links that the graph can't show, such as a child that names
a missing node, appear as problems and are dropped on save. A tree with problems
can be saved, but it won't load until you fix them.

The panel's title shows `*` while there are unsaved changes. The editor offers to
save them before it closes, before a build, and before you open another tree.
Play runs the saved file. When the file changes on disk, the panel reloads it,
and asks first if you have unsaved changes. A file that isn't a valid tree can't
be edited in the graph. Fix it with **Open Lua Source**.

## Attach and control agents

In the editor, select an entity and choose **Add Component → Behavior Tree**.
Select the `.bt.lua` asset in **Tree**. Use **Enabled** to start or stop the
tree and **Apply Parameters** to apply the entity's JSON overrides. For the
pulse demo, enter `{"pulseEnabled": true, "seconds": 1}`. Save the scene to
keep the attachment and its settings. To create a Lua definition with metadata,
use **New → Behavior Tree** in the content browser. Before Play, open the new
tree and choose the starter action's script.

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
