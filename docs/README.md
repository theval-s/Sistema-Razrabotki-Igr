# SRI Engine documentation

Start here if you are writing a game with SRI Editor, or are an AI agent helping with one.
These guides ship with the editor (`docs/` next to `editor.exe`) and with the engine SDK
(`share/doc/SRI_Engine`).

## Where things are

- **Project layout.** A project is a folder with a `.sriproject` file. Game assets live in its
  `content/` folder and are addressed as `game/...`: `game/scripts/player.lua` is
  `content/scripts/player.lua`. Built-in assets are addressed as `engine/...` and live in the
  `engine/` folder next to the editor.
- **Lua API signatures.** The editor writes `.sri/lua/engine.d.lua` and `autoloads.d.lua` into the
  project (LuaLS annotations; **Project > Refresh Lua Definitions** regenerates them). Read them for
  exact names and types; the guides below explain behavior. Never `require` them.
- **Paths from the engine repository.** Paths such as `example_game/...`, `engine/src/...` and
  `tests/...` refer to the engine's source repository and are not part of an editor install.

## Guides

Writing games in Lua with the editor:

| Guide | Covers |
|---|---|
| [scripting.md](scripting.md) | Play mode, script lifecycle, exposed properties, autoloads, shared modules, the Lua API reference, input, cursor modes, saves |
| [physics.md](physics.md) | Rigid bodies and colliders in the editor, contact and sleep callbacks in Lua (also the C++ fixed-update model) |
| [behavior-trees.md](behavior-trees.md) | `.bt.lua` tree assets, condition and action scripts, attaching trees to agents |
| [navigation.md](navigation.md) | NavMesh baking, pathfinding and crowds from Lua (in Russian) |
| [ui.md](ui.md) | Game UI with RML/RCSS: documents, styles, models, events, preview and reload |
| [ui-lua-facade.md](ui-lua-facade.md) | The exact Lua UI API and its lifetime rules |

Asset formats, with their C++ APIs:

| Guide | Covers |
|---|---|
| [scene-assets.md](scene-assets.md) | `.meta` sidecars and UUIDs, scene JSON, saves, the C++ scene API |
| [material-assets.md](material-assets.md) | Material and material-instance JSON, shader hooks, the C++ material API |

C++ hosts, in `sdk/` (engine SDK only):

| Guide | Covers |
|---|---|
| [sdk/ui-implementation-contract.md](sdk/ui-implementation-contract.md) | Native UI integration for C++ hosts |
