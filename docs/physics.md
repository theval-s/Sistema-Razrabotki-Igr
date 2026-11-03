# Fixed update and Jolt physics

The engine runs variable-rate Update/controller callbacks, then applies queued scene
changes, then zero or more fixed steps, followed by LateUpdate and Render.
All subsystem callbacks run on the calling/main thread. Jolt uses its bounded
worker pool internally; no workers access Scene or EnTT.

## FixedUpdate

Override Subsystem::FixedUpdate(const FixedUpdateContext&). The context contains
stepIndex (starting at zero), deltaSeconds, and simulationTimeSeconds (end of the
current step, since the last simulation reset). EngineConfig defaults to 1/60 s
and at most 8 steps per frame. Excess whole-step backlog is discarded with a
warning; the fractional remainder is retained. Fixed time excludes dropped time.

Register gameplay subsystems which issue physics commands before PhysicsSubsystem.
Physics writes final dynamic poses before render extraction in LateUpdate.
Changing scenes/Play/Stop resets the accumulator, fixed clock and runtime bodies.
Pause/Edit mode does not simulate; pausing clears the fractional accumulator.
A scene committed in a frame starts accumulating on the next frame. A level load
requested during FixedUpdate applies after the whole step; no later steps use the
old scene. ResetSimulation is the corresponding subsystem lifecycle hook.

## Setup and components

Add PhysicsSubsystem to a host before Initialize; the editor and ExampleGame
already register it. There is one PhysicsSystem per subsystem, while Jolt type
registration lives until the last subsystem shuts down. Engine and EngineStatic
both link Jolt privately via vcpkg. No Jolt types appear in the public API.

A physics entity needs both RigidBody and Collider. Component data is authored
and serializable; BodyIDs and shapes are owned only by the physics subsystem.
Example components object:

~~~json
{
  "RigidBody": { "motionType": "Dynamic", "mass": 1, "friction": 0.5, "restitution": 0.15 },
  "Collider": { "shape": "Box", "center": [0, 0, 0], "showDebug": true, "halfExtents": [0.5, 0.5, 0.5] }
}
~~~

New RigidBody components default to Static. Motion types are Static, Dynamic
and Kinematic. Mass must be positive,
friction nonnegative, restitution in [0,1]. Collider center is an additional local
translation, not a world position; it is scaled with the entity. One scene unit
is one metre; gravity defaults to -9.81 on Y.

Collider shapes and their JSON fields:

| shape | Dimensions / geometry |
| --- | --- |
| Cube | halfExtents.x is the local half side; Y/Z are ignored |
| Box | positive halfExtents [x,y,z] |
| Sphere | positive radius |
| Capsule | positive radius, nonnegative halfHeight of the cylinder, along local Y; total height = 2 * (halfHeight + radius) |
| ConvexMesh | convex hull of the entity's MeshRenderer mesh positions |
| TriangleMesh | original mesh triangles; Static bodies only |

RigidBody JSON requires motionType, mass, friction and restitution. Collider JSON
requires shape, center, showDebug and the dimension fields listed for that shape;
mesh shapes have no dimension fields. Unknown or missing fields are rejected.
Scenes may contain no physics components; their existing scene/asset formats
remain supported. The physics interface consists of RigidBody and Collider.
Box/Cube dimensions and mesh vertices follow world scale per axis (a Cube under
nonuniform scale becomes a rectangular box). A sphere keeps its shape using the
largest world scale axis. A capsule keeps a circular cross-section using max(X,Z)
for radius and Y for cylinder half height; it never becomes an ellipsoid.

Dynamic bodies must be root entities and have positive local scale. Static and
kinematic bodies may have parents when their world transform can be decomposed
without shear or reflection. Nonuniform positive scale is supported for boxes.
Invalid scene data is rejected before replacing the active scene; invalid runtime
edits are diagnosed and the affected body is removed. Components are reconciled
before each step. Changing body settings, collider geometry or scale rebuilds the
body and resets velocity. Mesh AABBs are not implicitly physical colliders.
Mesh colliders require MeshRenderer. While its async mesh load is pending, the
body is absent; it is created on the first fixed step after publication. Changing
the mesh recreates the body. Removed/unavailable mesh data removes its body.

MeshAsset retains shared immutable MeshGeometry (positions and CCW triangle
indices, after import transforms/normalization). GPU-only normals/UV data is still
released after upload. Jolt constructs convex hulls / triangle acceleration trees
when creating a body, not every fixed step. This cooking currently runs on the
main thread and large meshes can cause a Play-start hitch. Convex hulls fill holes
and concavities; triangle surfaces are one-sided and need outward-facing winding.
Dynamic/Kinematic use convex hulls; TriangleMesh is restricted to Static here.
Jolt supports up to 256 hull vertices (interior source vertices do not count).
An excessively detailed or invalid hull can fail cooking; its diagnostic is logged
and the failed input is cached until collider dimensions/source geometry change.
Use a simpler collision mesh or primitive in that case.

## Editing in the editor

Select an entity and use Add Component > Rigid Body (or Collider). Either
entry adds both components with a default Static body, so the scene remains
saveable. Change Motion type to Dynamic for a falling object, or leave Static
for a floor. Dynamic is unavailable on entities with a parent; move the entity
to the hierarchy root first.

Rigid Body exposes mass, friction and restitution. Collider has a Shape selector:
Cube, Box (parallelepiped), Sphere, Capsule, Mesh. Mesh exposes a Convex checkbox
which asks Jolt to generate a convex hull, even when the source mesh is concave.
In JSON, Mesh + Convex is shape ConvexMesh; Mesh without Convex is TriangleMesh.
Only the selected shape's dimension fields are shown. Center is a local offset;
[0.5, 0.5, 0.5] half extents describe a unit box before Transform scale.

Assign the model in Mesh Renderer and wait for loading, then:

- Fit to mesh sizes the selected primitive from the mesh AABB and centers it on
  that AABB. Cube uses the largest half extent; Sphere uses the AABB half-diagonal;
  Capsule uses max(X,Z) half extent for radius and max(0,Y-radius) for cylinder
  half height. Capsule fitting is an approximation; adjust it for the model.
- From mesh resets Center and selects TriangleMesh for Static or ConvexMesh for
  Dynamic/Kinematic (preserving an existing Convex selection). Geometry stays linked to the Mesh Renderer asset rather than
  being copied into scene JSON. Mesh tools are disabled while data is unavailable.
- Convex is optional for Static: checked uses a generated hull, unchecked uses
  original triangles. Changing Motion type to Dynamic/Kinematic automatically
  enables Convex and locks the checkbox; switching back to Static allows turning
  it off. A previously chosen Convex mode is preserved by From mesh.

Numeric fields enforce positive mass/dimensions, nonnegative capsule half height
and friction, and restitution in [0,1]. Save with Ctrl+S and press Play. Stop
restores the authored pose. Removing either physics component removes both; the
remove button tooltip states this explicitly. Shape/settings edits during Play
rebuild the body and reset velocity.

The primary selected entity (the one shown in the inspector) displays a gold
collider wireframe over the viewport, visible through the rendered mesh. The
Collider inspector's Show collider checkbox controls this preview. It defaults
to enabled for new components and persists as the required boolean showDebug field
in Collider JSON, and never rebuilds the physical body or resets its velocity. This
works in Edit and Play for every Collider shape, including the cooked convex
hull rather than the source mesh. Editing Shape, Center, dimensions, scale, Convex
or the source mesh updates the outline; moving/rotating transforms cached lines.
Deselecting, deleting the component/entity, unavailable mesh data or an invalid
collider clears the preview. Preview uses the simulation's Jolt shape builder and
Jolt leaf-shape triangles, without creating bodies or advancing physics in Edit.
Only one preview geometry is cached; unchanged shapes are not cooked each frame.

## Pose ownership and commands

- Dynamic: physics owns position/rotation. Direct Transform edits are overwritten;
  use Teleport to move a simulated body deliberately.
- Static: authored Transform changes are applied at the next fixed step.
- Kinematic: Transform is the target reached over the next step. MoveKinematic
  sets that target; an unchanged target stops the body on the following step.
- Teleport takes local position/rotation, moves the body immediately and preserves
  velocity. SetLinearVelocity supports dynamic bodies; AddImpulse requires Dynamic.
  All commands return bool plus a diagnostic string and must run on the main thread.

Bodies are batch-added using Jolt's broadphase interface. Removal of either
component, destruction of an entity, scene replacement and Shutdown release bodies.
Queries in SceneQueries remain the existing scene/AABB queries; they also cover
nonphysical objects.

## Contact callbacks in Lua

Attach an enabled Script component to either participant to receive
`OnCollisionEnter(other, contact)` and `OnCollisionExit(other, contact)`.
Scripts that return a table receive `self` before these arguments.
Events describe pairs of entities. Enter is emitted on the first contact and
Exit after the last contact ends. Sleeping contacts stay active. When an
entity is destroyed, the surviving script can receive Exit with `other == nil`.

`contact.point` is a representative world-space contact point.
`contact.normal` points away from the other body toward the recipient.
Exit carries the last reported contact sample. `contact.otherId` is an opaque
scene-local integer that stays the same for the pair, including on Exit.
`contact.stepIndex` identifies the fixed step that produced the event.
Events are delivered on the main thread before the next frame's OnUpdate.
Scene changes and Stop discard queued events.

## Demonstration and checks

ExampleGame's demo scene contains a static floor and three falling boxes tagged
physics-demo. The enchanted cube uses authored animation. Pause and
Play/Stop use the engine's existing controls; restarting Play reconstructs bodies
from the authored snapshot. Save-game transforms continue using GamePersistence;
velocities/sleep state are not saved, so loading restarts physics from those poses.
Demo boxes do not opt into GamePersistence; they reset to their authored positions
on loading a save, preserving the example scene's existing persistence policy.

Build using the existing presets and run existing GoogleTest executables directly.
No CTest integration or new test suites are required. Check fixed stepping, floor
contacts, removal, repeated initialization, paused frames and scene replacement.

Not included: interpolated rendering, character controllers, asynchronous shape cooking,
constraints, ragdolls, Lua physics commands.
