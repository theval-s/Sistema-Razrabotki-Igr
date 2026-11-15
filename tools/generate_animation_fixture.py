#!/usr/bin/env python3
"""Generate the original, self-contained TestBot GLB using only the stdlib."""
import hashlib
import json
import math
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "example_game/models/TestBot/model.glb"
blob = bytearray()
views = []
accessors = []


def accessor(values, fmt, kind, component, *, target=None, bounds=False):
    while len(blob) % 4:
        blob.append(0)
    offset = len(blob)
    for value in values:
        blob.extend(struct.pack('<' + fmt, *value))
    view = {'buffer': 0, 'byteOffset': offset, 'byteLength': len(blob) - offset}
    if target is not None:
        view['target'] = target
    views.append(view)
    result = {'bufferView': len(views) - 1, 'componentType': component,
              'count': len(values), 'type': kind}
    if bounds:
        result['min'] = [min(v[i] for v in values) for i in range(len(values[0]))]
        result['max'] = [max(v[i] for v in values) for i in range(len(values[0]))]
    accessors.append(result)
    return len(accessors) - 1


def section(boxes):
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    # Each face has its own vertices for crisp normals and complete UVs.
    faces = [((1, 0, 0), [(1,-1,-1),(1,1,-1),(1,1,1),(1,-1,1)]),
             ((-1, 0, 0), [(-1,-1,1),(-1,1,1),(-1,1,-1),(-1,-1,-1)]),
             ((0, 1, 0), [(-1,1,-1),(-1,1,1),(1,1,1),(1,1,-1)]),
             ((0,-1, 0), [(-1,-1,1),(-1,-1,-1),(1,-1,-1),(1,-1,1)]),
             ((0, 0, 1), [(1,-1,1),(1,1,1),(-1,1,1),(-1,-1,1)]),
             ((0, 0,-1), [(-1,-1,-1),(-1,1,-1),(1,1,-1),(1,-1,-1)])]
    for center, half, joint in boxes:
        for normal, corners in faces:
            start = len(positions)
            for corner, uv in zip(corners, [(0,0),(0,1),(1,1),(1,0)]):
                # Geometry is mesh-local; mesh node translates +0.25 in X.
                positions.append(tuple(center[i] + half[i] * corner[i] - (0.25 if i == 0 else 0) for i in range(3)))
                normals.append(normal)
                uvs.append(uv)
                # Blend limb/head vertices at their upper end into the torso.
                blend = joint != 0 and corner[1] == 1
                joints.append((joint, 0, 0, 0))
                weights.append((0.75, 0.25, 0, 0) if blend else (1, 0, 0, 0))
            indices.extend((start, start+1, start+2, start, start+2, start+3))
    attrs = {'POSITION': accessor(positions, '3f', 'VEC3', 5126, target=34962, bounds=True),
             'NORMAL': accessor(normals, '3f', 'VEC3', 5126, target=34962),
             'TEXCOORD_0': accessor(uvs, '2f', 'VEC2', 5126, target=34962),
             'JOINTS_0': accessor(joints, '4B', 'VEC4', 5121, target=34962),
             'WEIGHTS_0': accessor(weights, '4f', 'VEC4', 5126, target=34962)}
    return {'attributes': attrs, 'indices': accessor([(i,) for i in indices], 'H', 'SCALAR', 5123, target=34963), 'material': 0}


primitives = [section([((0,1.25,0),(.4,.5,.22),0), ((0,1.95,0),(.27,.2,.2),1)]),
              section([((-.58,1.25,0),(.16,.48,.16),2), ((.58,1.25,0),(.16,.48,.16),3),
                       ((-.22,.38,0),(.17,.38,.18),4), ((.22,.38,0),(.17,.38,.18),5)])]
# All joint positions are in source-root space; IBM = inverse(joint world) * mesh world.
joint_positions = [(0,0,0), (0,1.75,0), (-.58,1.7,0), (.58,1.7,0), (-.22,.76,0), (.22,.76,0)]
matrices = []
for x, y, z in joint_positions:
    matrices.append((1,0,0,0, 0,1,0,0, 0,0,1,0, .25-x,-y,-z,1))
ibm = accessor(matrices, '16f', 'MAT4', 5126)
times = accessor([(0,),(.5,),(1,)], 'f', 'SCALAR', 5126, bounds=True)


def clip(name, channels):
    samplers, targets = [], []
    for node, path, values in channels:
        size = 4 if path == 'rotation' else 3
        output = accessor(values, str(size) + 'f', 'VEC' + str(size), 5126)
        samplers.append({'input': times, 'output': output, 'interpolation': 'LINEAR'})
        targets.append({'sampler': len(samplers)-1, 'target': {'node': node, 'path': path}})
    return {'name': name, 'samplers': samplers, 'channels': targets}


def rotation(angle):
    return (math.sin(angle/2), 0, 0, math.cos(angle/2))


identity = rotation(0)
animations = [clip('idle', [(1, 'translation', [(0,0,0),(0,.025,0),(0,0,0)]),
                            (2, 'scale', [(1,1,1),(1,1.03,1),(1,1,1)])]),
              clip('walk', [(1, 'translation', [(0,0,0),(0,.04,0),(0,0,0)]),
                            (3, 'rotation', [rotation(.45),rotation(-.45),rotation(.45)]),
                            (4, 'rotation', [rotation(-.45),rotation(.45),rotation(-.45)]),
                            (5, 'rotation', [rotation(-.45),rotation(.45),rotation(-.45)]),
                            (6, 'rotation', [rotation(.45),rotation(-.45),rotation(.45)]),
                            (2, 'scale', [(1,1,1),(1,1,1),(1,1,1)])])]
q = math.sqrt(.5)
document = {'asset': {'version': '2.0', 'generator': 'Original TestBot stdlib generator'},
            'scene': 0, 'scenes': [{'nodes': [0]}],
            'nodes': [{'name': 'SourceRoot', 'translation': [1,2,3], 'rotation': [0,0,q,q], 'children': [1,7]},
                      {'name': 'Torso', 'children': [2,3,4,5,6]},
                      *[{'name': name, 'translation': list(pos)} for name, pos in zip(
                          ['Head','LeftArm','RightArm','LeftLeg','RightLeg'], joint_positions[1:])],
                      {'name': 'RobotMesh', 'translation': [.25,0,0], 'mesh': 0, 'skin': 0}],
            'skins': [{'name': 'TestBotSkin', 'inverseBindMatrices': ibm, 'skeleton': 1, 'joints': [1,2,3,4,5,6]}],
            'meshes': [{'name': 'TestBot', 'primitives': primitives}],
            'materials': [{'name': 'Robot', 'pbrMetallicRoughness': {'baseColorFactor': [.5,.7,.9,1], 'metallicFactor': 0, 'roughnessFactor': .8}}],
            'animations': animations, 'bufferViews': views, 'accessors': accessors,
            'buffers': [{'byteLength': len(blob)}]}
encoded = json.dumps(document, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('ascii')
encoded += b' ' * (-len(encoded) % 4)
blob.extend(b'\0' * (-len(blob) % 4))
glb = (struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(encoded) + 8 + len(blob)) +
       struct.pack('<I4s', len(encoded), b'JSON') + encoded +
       struct.pack('<I4s', len(blob), b'BIN\0') + blob)
OUTPUT.write_bytes(glb)
print(f'{OUTPUT}: {len(glb)} bytes SHA-256 {hashlib.sha256(glb).hexdigest()}')
