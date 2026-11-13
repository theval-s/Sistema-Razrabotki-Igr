# Navigation

1. Open a scene with loaded mesh assets. Navigation uses the triangles of all
   Mesh Renderer components in world space.
2. Choose **Navigation → Build Navigation Mesh** and set the voxel size and
   agent dimensions. The editor shows the resulting polygon edges in turquoise.
3. Enter Play. Scripts can query the baked mesh with the API below.
4. Rebuild after changing scene geometry. Opening another scene clears the mesh.
   Stopping Play restores the mesh that was present when Play started.

The bake lives in memory. In standalone games, call `Navigation.Build()` once
the level meshes are loaded. It returns false if geometry is unavailable or
cannot be baked. A failed rebuild retains the previous mesh.

```lua
local goal = Vector3.new(8, 0, 5)

function OnStart()
    local points = Navigation.FindPath(entity:GetPosition(), goal)
    for _, point in ipairs(points) do
        Log.info(string.format("%.2f %.2f %.2f", point.x, point.y, point.z))
    end
end
```

`Navigation.FindPath(start, goal)` returns world-space waypoints including the
projected endpoints. An empty table means there is no complete route.
`Navigation.Build(options)` accepts an optional table with `cellSize`,
`cellHeight`, `agentHeight`, `agentRadius`, `agentClimb`, and `agentSlope`.
Distances are in world units. Slope is in degrees. A Lua rebuild during Play
applies to that session. The caller moves the entity along the waypoints.
