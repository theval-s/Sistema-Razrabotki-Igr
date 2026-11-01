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

A physics entity needs both RigidBody and BoxCollider. Component data is authored
and serializable; BodyIDs and shapes are owned only by the physics subsystem.
Example scene components:

~~~json
"RigidBody": { "motionType": "Dynamic", "mass": 1, "friction": 0.5, "restitution": 0.15 },
"BoxCollider": { "center": [0, 0, 0], "halfExtents": [0.5, 0.5, 0.5] }
~~~

Motion types are Static (default), Dynamic and Kinematic. Mass must be positive,
friction nonnegative, restitution in [0,1]. Collider half extents must be positive;
center is a local offset, not a world position. The offset and half extents are
scaled with the entity. One scene unit is one metre; gravity defaults to -9.81 on Y.

Dynamic bodies must be root entities and have positive local scale. Static and
kinematic bodies may have parents when their world transform can be decomposed
without shear or reflection. Nonuniform positive scale is supported for boxes.
Invalid scene data is rejected before replacing the active scene; invalid runtime
edits are diagnosed and the affected body is removed. Components are reconciled
before each step. Changing body settings, collider geometry or scale rebuilds the
body and resets velocity. Mesh AABBs are not implicitly physical colliders.

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
nonphysical objects. This PR does not add physics-specific queries or contact events.

## Demonstration and checks

ExampleGame's demo scene contains a static floor and three falling boxes tagged
physics-demo. The enchanted cube keeps its previous authored animation. Pause and
Play/Stop use the engine's existing controls; restarting Play reconstructs bodies
from the authored snapshot. Save-game transforms continue using GamePersistence;
velocities/sleep state are not saved, so loading restarts physics from those poses.
Demo boxes do not opt into GamePersistence; they reset to their authored positions
on loading a save, preserving the example scene's existing persistence policy.

Build using the existing presets and run existing GoogleTest executables directly.
No CTest integration or new test suites are required. Check fixed stepping, floor
contacts, removal, repeated initialization, paused frames and scene replacement.

Not included: interpolated rendering, character controllers, mesh/convex cooking,
constraints, ragdolls, Lua physics bindings, inspector controls and contact events.
