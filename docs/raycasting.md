# Ray queries and picking

Use physics queries for collider-accurate shooting and occlusion; use scene bounds
queries for approximate picking, including objects without physics components.

## Lua API

| Function | Result |
| --- | --- |
| `Physics.Raycast(origin, direction, maxDistance, options?)` | Closest physical hit |
| `Physics.RaycastAll(origin, direction, maxDistance, options?)` | Physical hits |
| `Physics.Linecast(start, end, options?)` | Closest physical hit along the segment |
| `Scene.RaycastBounds(origin, direction, maxDistance, options?)` | Closest world-AABB hit |
| `Scene.RaycastBoundsAll(origin, direction, maxDistance, options?)` | World-AABB hits |
| `Scene.ScreenPointToRay(point, options?)` | Camera ray |
| `Physics.SyncQueries()` | Boolean success, error |

All return `value, error`: success has an empty error string. A closest-hit miss
returns `nil, ""`; an all-hits miss returns an empty array and `""`. Invalid
arguments or unavailable services return `nil` and a nonempty diagnostic. Check
the error before interpreting nil as a miss. `SyncQueries` instead returns
`false, error` on failure.

Ray origin and direction are world-space Vector3 values; direction must be finite
and nonzero and is normalized automatically. `maxDistance` is positive and finite,
in metres. Linecast takes finite world-space Vector3 endpoints; coincident endpoints
are a successful miss. A hit contains `entity` (checked Entity), `distance` (metres),
`point` and `normal` (world-space Vector3), and `isTrigger` (boolean). `normal` is
nil for a distance-zero hit when the origin is inside or on the surface.
Normals follow the collider's surface orientation; accepting a triangle backface
does not flip its normal.
All-hits queries return the nearest surface per entity, sorted by distance: they
do not enumerate exit surfaces, thickness, or individual triangles.

Both query families accept `ignore`, a dense 1-based array of valid current-scene
Entity handles, and `ignoreDescendants` (boolean, default false). Physics also
accepts `includeTriggers` (default false) and `hitBackFaces` (default false,
for mesh triangles). Unknown options are rejected; physics-only options are not
valid for bounds queries.

Physics queries test actual bodies requiring both RigidBody and Collider. Pending
or invalid mesh colliders are absent under the normal physics policy. Bounds
queries test approximate world AABBs from renderer/posed geometry or explicit
Bounds, include nonphysical objects and do not filter out triggers.

After a physics reset the first valid query lazily prepares bodies. Later queries
read the current physics snapshot, without a full scene sync per ray. A fixed step
or explicit `Physics.SyncQueries()` reconciles new/changed bodies and static
transforms; `SyncQueries` does not advance time. Existing kinematic targets take
effect only after a fixed step; use `Physics.Teleport` for deliberate dynamic-body
pose changes. Check the SyncQueries result if calling it explicitly.

## Mouse picking

`Scene.ScreenPointToRay(point, {camera = camera, viewportSize = size})` uses the
active camera and render surface size when options are omitted. A supplied camera
must be a valid current-scene camera entity; `viewportSize` is a Vector2. The
current authored camera view matches rendering (camera roll is ignored). Point
coordinates are viewport-local physical pixels, top-left origin, +Y down, like
`Input.MousePosition()`. For a custom viewport, subtract its pixel offset first.
Points outside `[0, size)` are errors. The returned `{origin, direction, maxDistance}`
starts at the near plane and reaches the far plane; pass it to either raycast family.

```lua
local ray, err = Scene.ScreenPointToRay(Input.MousePosition())
if not ray then
    Log.warning(err)
    return
end
local hit
hit, err = Scene.RaycastBounds(ray.origin, ray.direction, ray.maxDistance)
if err ~= "" then
    Log.warning(err)
elseif hit then
    Log.info("Picked " .. hit.entity:GetName())
end
```

## Line of sight

This tests obstacles between owner and target while ignoring both hierarchies.
Choose eye/target positions appropriate to your game; walls need physics colliders.
An error is **not** an unobstructed line of sight.

```lua
local function hasLineOfSight(owner, target)
    local hit, err = Physics.Linecast(owner:GetWorldPosition(), target:GetWorldPosition(), {
        ignore = {owner, target},
        ignoreDescendants = true,
    })
    if err ~= "" then
        Log.warning(err)
        return false
    end
    return hit == nil
end
```

## C++ API

`engine/scene/RayQuery.hpp` defines `Ray`, `RayHit` and `QueryFilter`.
`PhysicsSubsystem::Raycast`, `RaycastAll`, `Linecast` and `SyncQueries` take
output parameters and return bool plus `std::string& error`; use
`PhysicsRaycastOptions` for the filter and physics flags. Scene equivalents
`engine::RaycastBounds` and `RaycastBoundsAll` take a Scene, scene AssetSystem,
Ray and QueryFilter. `engine::ScreenPointToRay` takes scene, viewport-local point,
viewport size and camera entity (`entt::null` selects the active camera), plus
output ray and error. A successful query can have no hit. Physics public operations
are main-thread only.
