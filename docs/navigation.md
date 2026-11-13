# Navigation

1. Create an empty entity and add **Navigation Region** in the Inspector.
   Move the entity to the center of the play area and set **Half extents** so
   the box encloses the floor and the obstacles. The box uses world axes and
   follows the entity position. Rotation and scale do not affect it.
2. Add **Navigation Source** to each floor, wall or obstacle that should affect
   the bake. Leave **Use Collider** enabled for objects with a static Rigid Body
   and Collider. Turn it off to use the object's Mesh Renderer, for example for
   an imported level. Unmarked objects, triggers and moving rigid bodies are
   excluded.
3. Select the region and click **Bake Navigation Mesh**. The viewport shows the
   region box and the turquoise edges of the resulting navigation polygons.
   If baking fails, check that the region encloses the floor, source meshes have
   loaded, and the agent fits through the intended passages.
4. In the Content Browser choose **New → Navigation Follower Script** and attach
   the script to a visible object without a dynamic rigid body. Place it over
   the baked mesh. In **Script → Properties**, set the destination, speed and
   pivot height above the floor. Press **Play**.

Source assignments and region settings are saved with the scene. The engine uses
one active bake at a time. Baking another region replaces the active mesh. The
bake is kept in memory, so bake again after opening a scene or editing its
geometry. Play uses the current bake and Stop restores the editor bake.

In a Lua script, query the active bake with world-space positions:

```lua
local start = entity:GetWorldPosition()
local goal = Vector3.new(8, 0, 5)
local points = Navigation.FindPath(start, goal)
if #points == 0 then
    Log.warning("No route")
end
```

The returned table contains projected endpoints and intermediate waypoints. An
empty table means there is no complete path or there is no active bake. The
generated follower script demonstrates moving an object along these points.
