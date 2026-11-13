# Navigation

1. Choose **Add → NavMesh** in the scene hierarchy.
   Move the entity to the center of the play area and set **Half extents** so
   the box encloses the floor and the obstacles. The box uses world axes and
   follows the entity position. Rotation and scale do not affect it.
2. Add **Navigation Source** to each floor, wall or obstacle that should affect
   the bake. Leave **Use Collider** enabled for objects with a static Rigid Body
   and Collider. Turn it off to use the object's Mesh Renderer, for example for
   an imported level. Unmarked objects, triggers and moving rigid bodies are
   excluded.
3. Select the NavMesh object and click **Bake Navigation Mesh**. The viewport shows the
   region box and the turquoise edges of the resulting navigation polygons.
   If baking fails, check that the region encloses the floor, source meshes have
   loaded, and the agent fits through the intended passages.
4. In the Content Browser choose **New → Navigation Follower Script** and attach
   the script to a visible object without a dynamic rigid body. Place it over
   the baked mesh. In **Script → Properties**, set the destination, speed and
   pivot height above the floor. Set **navMeshName** to the name of a specific
   NavMesh, or leave it empty for automatic selection. Press **Play**.

Source assignments and NavMesh settings are saved with the scene. Create and
rename as many NavMesh objects as needed, then bake each one separately. For
example, two overlapping objects can use different agent radii. The bakes live
in memory. Bake again after reopening the scene or editing its geometry. Play
uses the current bakes and Stop restores the editor bakes.

In Lua, pass world-space positions. To select a specific NavMesh, pass its entity as the third argument:

```lua
local start = entity:GetWorldPosition()
local goal = Vector3.new(8, 0, 5)
local mesh = Scene.FindByName("Large Agent NavMesh")
local points = Navigation.FindPath(start, goal, mesh)
if #points == 0 then
    Log.warning("No route")
end
```

The returned table contains projected endpoints and intermediate waypoints. An
empty table means there is no complete path on the requested mesh. Omit the
third argument to select the shortest complete route on a single baked mesh.
Routes do not cross from one NavMesh object to another. The
generated follower script demonstrates moving an object along these points.

## Crowd agents in Lua

Use a unique name for the NavMesh and bake it with a positive agent radius.
The crowd uses the height and radius of that bake. Each NavMesh has its own
crowd with up to 128 agents. Agents avoid other agents on the same NavMesh.
The position returned by Detour is at floor level.

```lua
Properties = {
    navMeshName = "NavMesh",
    speed = 3,
    heightOffset = 0.5,
    goalX = 8,
    goalY = 0,
    goalZ = 5,
}

local mesh
local registered = false

function OnStart()
    mesh = Scene.FindByName(Properties.navMeshName)
    local feet = entity:GetWorldPosition()
    feet.y = feet.y - Properties.heightOffset
    registered = Navigation.RegisterAgent(mesh, entity, feet, Properties.speed)
    if not registered then
        Log.warning("Could not register on the baked NavMesh")
        return
    end
    local goal = Vector3.new(Properties.goalX, Properties.goalY, Properties.goalZ)
    if not Navigation.SetAgentGoal(mesh, entity, goal) then
        Log.warning("Could not set a crowd goal")
    end
end

function OnUpdate()
    if not registered then return end
    local feet = Navigation.GetAgentPosition(mesh, entity)
    if feet then
        feet.y = feet.y + Properties.heightOffset
        entity:SetWorldPosition(feet)
    end
end

function OnDestroy()
    if registered then Navigation.RemoveAgent(mesh, entity) end
end
```

Attach the script to an object whose transform can be controlled by Lua.
The goal call submits a request and does not guarantee arrival. Destroyed
entities are removed automatically. Stopping Play clears its agents.
