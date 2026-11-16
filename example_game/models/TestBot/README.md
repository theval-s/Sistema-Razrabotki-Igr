# TestBot animation fixture

TestBot is an original robot model and animation created for this project; it
does not use third-party models or textures. The model, animations, and generator
are dedicated to the public domain under CC0 1.0 Universal:
https://creativecommons.org/publicdomain/zero/1.0/ .

From the repository root, run `python tools/generate_animation_fixture.py`.
The stdlib-only generator overwrites `model.glb` and prints its SHA-256. Expected
SHA-256: `2d039a8927dd94bb51578495b30e348887f698965b9e6fc0baadccd6a6da9aed`.
The GLB contains one skin, two mesh primitives sharing one material, no external
resources, and 1-second looping `idle` and in-place `walk` clips. Some upper
vertices of each non-root box have weights 0.75 joint / 0.25 torso.

## Bind pose and test values

Coordinates are glTF right-handed, Y-up; matrices below act on column vectors.
The source root has translation (1, 2, 3) and a +90 degree Z rotation,
quaternion (0, 0, sqrt(0.5), sqrt(0.5)). The mesh is its child with translation
(0.25, 0, 0). The torso joint is another child of the source root, and all
other joints are torso children. Joint indices and local translations are:

| Index | Joint | Translation |
|---|---|---|
| 0 | Torso | (0, 0, 0) |
| 1 | Head | (0, 1.75, 0) |
| 2 | LeftArm | (-0.58, 1.7, 0) |
| 3 | RightArm | (0.58, 1.7, 0) |
| 4 | LeftLeg | (-0.22, 0.76, 0) |
| 5 | RightLeg | (0.22, 0.76, 0) |

Each inverse bind is `inverse(jointWorldBind) * meshWorldBind`, so its linear
part is identity and its translation is, in joint order: (0.25, 0, 0),
(0.25, -1.75, 0), (0.83, -1.7, 0), (-0.33, -1.7, 0),
(0.47, -0.76, 0), (0.03, -0.76, 0). Consequently
`inverse(meshWorldBind) * jointWorldBind * inverseBind` is identity for every
joint. The root transform does not cancel if mesh world is omitted. Mesh-local
box coordinates already subtract the mesh translation.

Both clips have keys at 0, 0.5 and 1 seconds, all LINEAR. Torso translation
bobs by 0.025 (idle) or 0.04 (walk) in local Y at 0.5; walk alternates arm and
leg rotations by +/-0.45 radians about local X. Idle scales the head to
(1, 1.03, 1) at 0.5. Neither clip translates horizontally.
