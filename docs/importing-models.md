# Importing models

Static models come into a project through **Import...** in the Content Browser, or by dragging
model files from Explorer onto it. The editor reads `.glb`, `.gltf`, `.fbx`, `.obj`, `.dae`,
`.blend` and `.ply`. For skinned, animated models see [animation.md](animation.md).

## The Import Model dialog

- **Name**: the folder the model goes into, in the Content Browser's current folder.
- **Center on load**: moves the model so its bounds are centred on its origin.
- The slot list: one row per material slot, with the base color texture it samples and the UV
  set that texture reads. A warning icon marks a texture that can't be used, or a texture
  transform that isn't applied; hover it for the reason.
- **Extract textures**: writes the textures the model embeds or names into `textures/`.
  Embedded images are written as stored, never re-encoded.
- **Create a material instance per slot**: an instance of **Material** for each slot, using the
  slot's texture as its albedo. The next field takes an image for slots without a texture of their
  own. For a model with no textures at all, the dialog suggests the image beside it.

Importing `token.glb` as `Token` gives:

~~~text
content/props/Token/
  model.glb                    the model, renamed so the folder can stand for it
  model.glb.meta               names each slot's material instance
  textures/Gold.png
  materials/Gold.material-instance.json
  materials/Gem.material-instance.json
~~~

Files the model loads by name come along, keeping their relative layout: a `.gltf` file's buffers
and images, an `.obj` file's `.mtl`. Images a `.gltf` brings along are used as textures where they
land, without another copy in `textures/`. The import refuses a model that loads a buffer from
outside its own folder. Anything left out, such as a missing texture, is listed in the Log panel.

## Material slots and mesh renderers

A material slot is a source material that the model's triangles use, named as in the model.
A mesh renderer whose **Material** is **(Mesh materials)** draws each slot with the instance the
mesh's `.meta` names for it, and slots without one with the engine default. Choosing a material
instance instead draws every slot with that one. Models added to a scene from the Content Browser
start with **(Mesh materials)** when their mesh has materials of its own.

The generated instances are ordinary material instances: edit them in the Content Browser, see
[material-assets.md](material-assets.md). In Lua, a renderer's `materialInstance` is `""` while it
uses the mesh materials ([scripting.md](scripting.md)). The `.meta` format is in
[scene-assets.md](scene-assets.md#metadata).

## Models already in the project: Extract Materials

A model copied into `content/` outside the editor gets a `.meta` but no materials. Right-click it in
the Content Browser and choose **Extract Materials...**, which has the same options as the import.
Its output goes into the model's folder when the model is that folder's `model.<ext>`, otherwise
into a folder named after the model beside it: `statues/Lion.glb` gets `statues/Lion/textures` and
`statues/Lion/materials`.

Textures already in the project are used where they are. Files already in place are kept, so
running it again only adds what's missing. Loaded meshes switch to the new materials straight
away. A mesh renderer with a material of its own keeps it; set its **Material** to
**(Mesh materials)** to use the extracted ones.

## UV sets

Each slot samples the UV set its base color texture uses, such as glTF's `texCoord: 1` (TEXCOORD_1).
When the mesh lacks that set, the first one is used. A mesh without any UVs loads with zero UVs and
a warning, and its textures won't map.

## Limits

- Only the base color texture is used. Normal, metallic-roughness, emissive and occlusion maps are
  ignored.
- Texture transforms (glTF `KHR_texture_transform`: offset, scale, rotation) aren't applied. The
  slot list and the Log panel point them out.
- Embedded images can be PNG, JPEG, BMP, TIFF or DDS; WebP and KTX2 images are left out. External
  textures must be one of those formats too, so convert TGA files to PNG.
- Models import in the units they were exported in; there is no import scale. Scale the entity's
  Transform, or re-export in metres.

## Troubleshooting

- **The texture is upside down or scrambled.** Use the texture the model carries (import the model
  or use Extract Materials) rather than one pulled out by another tool: some flip images
  vertically. Check that the slot list shows the UV set you expect.
- **A slot draws untextured.** The Log panel says why its texture was left out. Point the slot's
  material instance at another texture, or import again with an image for the other slots.
- **The model draws in one material.** Its mesh renderer has a material of its own; choose
  **(Mesh materials)**.
