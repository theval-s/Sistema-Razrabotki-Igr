# Building and shipping a game

**Project > Build Game** in the editor runs `sri-build`, which checks the project, cooks its
assets and writes a folder you can ship. The editor itself never uses cooked data: it always
imports your source files, so nothing in `content/` changes when you build.

The same build from the command line:

~~~text
sri-build --project <file.sriproject> --out <folder> [--runtime <dir>] [--title T]
          [--width N] [--height N] [--fullscreen] [--capture-mouse] [--no-pak]
~~~

## What a build contains

~~~text
MyGame/
  MyGame.exe         starts the game; named after the project
  bin/               the game runtime, the engine and third-party libraries
  licenses/          notices for the third-party libraries
  game.json          window title, size, fullscreen, mouse capture; safe to edit
  game.sripak        everything else
  .sri-build         marks the folder as a build, so the next build may replace it
~~~

`MyGame.exe` is a small launcher that loads the game from `bin/`, so ship the folder as a whole.

`sri-build` only replaces an output folder that is empty or carries `.sri-build`. If the project
has errors it reports them and leaves the previous build as it was.

## The game window

The game opens in a window whose inside is `width` by `height` pixels. With `fullscreen` it covers
the monitor it opens on instead, without a frame; the display mode stays as it is. **Alt+Enter**
switches between the two at any time, and the game doesn't receive that Enter press.

## Cooking

| Content | In a build |
|---|---|
| Meshes (`.obj`, `.fbx`, `.glb`, `.gltf`, `.dae`, `.blend`, `.ply`) | Imported once at build time and stored as `<file>.cooked`; the game loads them without Assimp |
| Skeletons, animation clips, skinned meshes | `<file>.cooked`, with the runtime animation data already built |
| HLSL | Not shipped. Every render pass and every hook of every material asset is compiled with optimizations into `shaders.cooked` |
| `.meta` sidecars | Not shipped. Their UUIDs and settings are gathered into `manifest.json` |
| Textures, audio, Lua scripts and behavior trees, scenes, materials, UI files, NavMesh files | Shipped as they are |

Because shaders are precompiled, a built game cannot compile HLSL. A material shader that no
material asset in the project uses, for example one only C++ code creates, is missing from the
library: loading it fails with `Shader is not in the cooked shader library`.
Give such a shader a material asset in `content/` so the build compiles its hooks.

Cooked files record the engine version that wrote them. A game whose files were cooked by another
engine version refuses them with `cooked by a different engine version; build the game again`:
rebuild after updating the engine.

## The pack

`game.sripak` is an ordinary zip archive with another extension. To look inside, run
`tar -tf game.sripak` or open a renamed copy with any zip tool. Paths inside it match how the
project addresses them: `game/...`, `engine/...`, plus `manifest.json`, `shaders.cooked` and
`game.sriproject` at the top.

Most files are compressed. Audio clips and PNG/JPEG images are stored uncompressed: those images
are compressed already, and streamed audio reads straight from the pack. File names are matched
without regard to case, like Windows paths.

When the game starts it mounts the pack over its own folder. Files in the pack win; anything not in
the pack, such as `game.json`, is read from the folder. A damaged pack or cooked file fails to load
with a message naming the file.

`--no-pak` writes the same cooked files loose next to the executable instead, which is handy for
checking what a build contains. The game runs from either layout.
