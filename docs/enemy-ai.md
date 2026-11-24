# Basic enemy AI

This guide builds a melee enemy that waits at its starting point, chases the player when they
come into sight, hits them when they are in reach, and walks back when they escape. You can
build it in two ways:

- **One script** (option A). This is the quickest way to get one enemy working.
- **A behavior tree** (option B). The logic lives in small reusable files, and each enemy can be
  tuned from the Inspector without code.

Both versions follow a NavMesh path when one is baked and walk in a straight line otherwise.

## 1. Prepare the player

Select the player object and enter `Player` in **Tags**. Enemies find the player by this tag.

Enemies deal damage by calling `TakeDamage` on the player's script with
`CallScript` (see [scripting.md](scripting.md#calling-another-objects-script)). Add the function
to your player script:

```lua
health = 100

function TakeDamage(amount)
    health = health - amount
    Log.info("Player hit, health " .. health)
end
```

## 2. Option A: one script

In the Content Browser choose **New → Script**, name it `enemy`, and replace its contents with:

```lua
-- Waits at home, chases the player in sight, attacks in reach.
Properties = {
    sightRange = 10,
    attackRange = 1.5,
    speed = 3,
    damage = 10,
    attackCooldown = 1,
    heightOffset = 0.5, -- from this object's origin down to its feet
}

local player, home, state
local cooldown = 0
local path, nextPoint, repathIn = {}, 1, 0

local function setState(next)
    if state ~= next then
        state, repathIn = next, 0
        -- One-time reactions to entering a state go here.
    end
end

-- Turns the object's forward (-Z) toward point.
local function face(point)
    local d = point - entity:GetWorldPosition()
    if d.x ~= 0 or d.z ~= 0 then
        entity:SetWorldRotation(Quaternion.YawPitchRoll(math.atan(-d.x, -d.z), 0, 0))
    end
end

-- One step toward goal: along a NavMesh path if one is baked, else straight.
local function stepTowards(goal, dt)
    repathIn = repathIn - dt
    if repathIn <= 0 then
        repathIn = 0.5
        local feet = entity:GetWorldPosition()
        feet.y = feet.y - Properties.heightOffset
        path, nextPoint = Navigation.FindPath(feet, goal), 1
    end
    local position = entity:GetWorldPosition()
    local target = Vector3.new(goal.x, position.y, goal.z)
    if nextPoint <= #path then
        local point = path[nextPoint]
        target = Vector3.new(point.x, point.y + Properties.heightOffset, point.z)
    end
    local delta = target - position
    local distance = delta:Length()
    local step = Properties.speed * dt
    if distance <= step then
        step, nextPoint = distance, nextPoint + 1
    end
    if distance > 0 then
        face(target)
        entity:SetWorldPosition(position + delta * (step / distance))
    end
end

function OnStart()
    home = entity:GetWorldPosition()
end

function OnUpdate(dt)
    cooldown = math.max(0, cooldown - dt)
    if not (player and player:IsValid()) then
        player = Scene.FindByTag("Player")[1]
        if not player then return end
    end
    local target = player:GetWorldPosition()
    local offset = target - entity:GetWorldPosition()
    offset.y = 0
    local distance = offset:Length()

    if distance <= Properties.attackRange then
        setState("attack")
        face(target)
        if cooldown == 0 then
            cooldown = Properties.attackCooldown
            local ok, err = player:CallScript("TakeDamage", Properties.damage)
            if not ok then Log.warning(err) end
        end
    elseif distance <= Properties.sightRange then
        setState("chase")
        stepTowards(target, dt)
    else
        setState("home")
        stepTowards(home, dt)
    end
end
```

Select the enemy object, choose **Add Component → Script**, and pick `enemy.lua`. Tune each enemy
in **Script → Properties**. Then press Play.

- The script moves the object by setting its position, so don't give it a **Dynamic** rigid body.
- The model's front should face -Z, the engine's forward direction.
- Each frame the script measures the flat (XZ) distance to the player and picks a state.
  `setState` runs once per change. `stepTowards` plans a new path every half second.

## 3. Option B: a behavior tree

The same enemy as a tree. Behavior trees are explained in [behavior-trees.md](behavior-trees.md).

```text
root (Selector)
├── engage (Sequence)
│   ├── seesPlayer        condition: player within sightRange
│   └── fight (Selector)
│       ├── melee (Sequence)
│       │   ├── inReach   condition: player within attackRange
│       │   └── attack    action: hit the player, at most once per cooldown
│       └── chase         action: one step toward the player
└── goHome                action: one step toward the starting point
```

In the Content Browser create a folder named `ai` and open it. Create these files and replace
each one's generated contents with the code below:

- `enemy.bt.lua`: choose **New → Behavior Tree** and name it `enemy`.
- The four leaf scripts: choose **New → Script** and name them `sees_player.condition`,
  `in_reach.condition`, `attack.action` and `move_to.action`.

A leaf script must `return` a table, so replace the whole New Script template. If you use a
different folder, update the `script` paths in `enemy.bt.lua` to match.

```lua
-- enemy.bt.lua
return {
    root = "root",
    nodes = {
        { id = "root", kind = "Selector", children = {"engage", "goHome"} },
        { id = "engage", kind = "Sequence", children = {"seesPlayer", "fight"} },
        { id = "seesPlayer", kind = "Condition", script = "game/ai/sees_player.condition.lua",
          parameters = { sightRange = 10 } },
        { id = "fight", kind = "Selector", children = {"melee", "chase"} },
        { id = "melee", kind = "Sequence", children = {"inReach", "attack"} },
        { id = "inReach", kind = "Condition", script = "game/ai/in_reach.condition.lua",
          parameters = { attackRange = 1.5 } },
        { id = "attack", kind = "Action", script = "game/ai/attack.action.lua",
          parameters = { damage = 10, attackCooldown = 1 } },
        { id = "chase", kind = "Action", script = "game/ai/move_to.action.lua",
          parameters = { target = "player", speed = 3, heightOffset = 0.5 } },
        { id = "goHome", kind = "Action", script = "game/ai/move_to.action.lua",
          parameters = { target = "home", speed = 3, heightOffset = 0.5 } },
    },
}
```

```lua
-- sees_player.condition.lua
-- Runs first every frame: finds the player and stores it and its distance for later nodes.
return {
    Check = function(ctx)
        local bb = ctx.blackboard
        bb.home = bb.home or ctx.entity:GetWorldPosition()
        if not (bb.player and bb.player:IsValid()) then
            bb.player = Scene.FindByTag("Player")[1]
            if not bb.player then return false end
        end
        local offset = bb.player:GetWorldPosition() - ctx.entity:GetWorldPosition()
        offset.y = 0
        bb.distance = offset:Length()
        return bb.distance <= ctx.parameters.sightRange
    end,
}
```

```lua
-- in_reach.condition.lua
return {
    Check = function(ctx)
        return ctx.blackboard.distance <= ctx.parameters.attackRange
    end,
}
```

```lua
-- attack.action.lua
-- Hits the player at most once per cooldown, and always finishes this frame.
return {
    Tick = function(ctx)
        local bb = ctx.blackboard
        local now = Time.SimulationTime()
        if now >= (bb.nextAttack or 0) then
            bb.nextAttack = now + ctx.parameters.attackCooldown
            local ok, err = bb.player:CallScript("TakeDamage", ctx.parameters.damage)
            if not ok then Log.warning(err) end
        end
        return BT.Success
    end,
}
```

```lua
-- move_to.action.lua
-- One step toward the player or home: along a NavMesh path if one is baked, else straight.
local path, nextPoint, repathAt = {}, 1, 0

return {
    Tick = function(ctx, dt)
        local p, me = ctx.parameters, ctx.entity
        local goal = p.target == "home" and ctx.blackboard.home or ctx.blackboard.player:GetWorldPosition()
        local now = Time.SimulationTime()
        if now >= repathAt then
            repathAt = now + 0.5
            local feet = me:GetWorldPosition()
            feet.y = feet.y - p.heightOffset
            path, nextPoint = Navigation.FindPath(feet, goal), 1
        end
        local position = me:GetWorldPosition()
        local target = Vector3.new(goal.x, position.y, goal.z)
        if nextPoint <= #path then
            local point = path[nextPoint]
            target = Vector3.new(point.x, point.y + p.heightOffset, point.z)
        end
        local delta = target - position
        local distance = delta:Length()
        local step = p.speed * dt
        if distance <= step then
            step, nextPoint = distance, nextPoint + 1
        end
        if distance > 0 then
            if delta.x ~= 0 or delta.z ~= 0 then
                me:SetWorldRotation(Quaternion.YawPitchRoll(math.atan(-delta.x, -delta.z), 0, 0))
            end
            me:SetWorldPosition(position + delta * (step / distance))
        end
        return BT.Success
    end,
}
```

Select the enemy object, choose **Add Component → Behavior Tree**, and pick `enemy.bt.lua` in
**Tree**. To tune one enemy, enter overrides in **Parameters (JSON)**, for example
`{"speed": 5, "sightRange": 15}`, and press **Apply Parameters**. An override reaches every node
that reads that name, so `speed` changes both `chase` and `goHome`. You can also attach the tree
from a script:

```lua
BT.Attach(Scene.FindByName("Grunt"), "game/ai/enemy.bt.lua", {speed = 5})
```

### Why every action finishes in one frame

A Sequence or Selector does not re-check earlier conditions while an action returns `Running`.
If `chase` kept running until it reached the player, the enemy would not notice the player
running out of sight. These actions do one frame of work and return `Success` instead. Next
frame the tree starts again from `root` and checks everything again. Return `Running` only for
work that must not be interrupted, and end it from the action itself.

### Where state lives

- `ctx.blackboard` is shared by all nodes of one enemy. Here it holds the player handle, the
  distance, the home position and the next attack time.
- Top-level `local` variables in a leaf file belong to one node of one enemy. `chase` and `goHome`
  each keep their own path in `move_to.action.lua`, and so does every enemy.
- `ctx.parameters` holds the node's defaults, overridden by the enemy's parameters.

## 4. Walk around walls

Both versions call `Navigation.FindPath` without a NavMesh argument. It uses the best route
across all baked NavMeshes and returns no points when none is baked. To bake one, follow
steps 1–2 of [navigation.md](navigation.md). In short: give the floor and walls a **Static** rigid
body, a collider and **Navigation Source**, add a NavMesh with **Add → NavMesh**, and press
**Bake Navigation Mesh**.

Set `heightOffset` to the distance from the enemy's origin to its feet. For a unit cube standing on
the floor that is `0.5`. Path points lie on the floor, and the enemy is placed `heightOffset`
above them.

The bake is saved to a `.navmesh` file next to the scene, so save the scene after baking. The
NavMesh then loads with the scene: in the editor, in Play, after `Scene.OpenLevel`, from a save
and in the packaged game. Bake again after you move the floor or walls.

For many enemies that should avoid each other, use DetourCrowd agents instead of `FindPath`
(see "Движение толпы" in [navigation.md](navigation.md)).

## Limits and troubleshooting

- **Sight is distance only in these examples.** Enemies notice the player through walls.
  Add a collider-based line-of-sight check using the example in
  [raycasting.md](raycasting.md#line-of-sight); treat query errors as blocked, not clear.
- **A script error stops the tree.** The Log panel names the tree, the entity and the node, and
  `BT.GetStatus(entity)` returns `Faulted`. During Play, the Behavior Tree panel shows the error
  and marks the node (see [behavior-trees.md](behavior-trees.md#watch-a-tree-during-play)). The tree stays stopped until it is disabled and
  enabled again, or for the rest of the Play session. Fix the script and press Play again.
- **Keep leaf callbacks small.** Each `Check`, `OnEnter`, `Tick` and `OnExit` call may run at most
  20,000 Lua instructions. A call that runs longer faults the tree.
- **Frame budget.** All trees together run at most 256 node steps per frame, and one tree at most
  32. This tree takes up to 7 steps, so about 35 enemies update every frame. Any more take turns.
  They still move at the right speed, in larger steps.
- **No path?** `FindPath` needs the start and goal within about 1 unit horizontally and 2 units
  vertically of the baked mesh. Check `heightOffset`, and check that the mesh covers both points.
  If the NavMesh's **Baked data** row in the inspector says "bake again", bake it.
- **No damage?** The player needs the `Player` tag and a script with a `TakeDamage` function.
  Otherwise the enemy logs the `CallScript` error as a warning.
