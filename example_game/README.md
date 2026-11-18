# ExampleGame

Build with `cmake --preset windows-example-game` and
`cmake --build --preset windows-example-game-debug`. Run ExampleGame.exe from the
configured output folder; the executable resolves its adjacent `engine/` and
`game/` assets. `deploy_ExampleGame` installs a portable folder with the same layout.

## Controls and audio tour

The default scene also contains editable smoke and fire ParticleEmitter entities.
Their direction is local up; rotating either entity changes its emission direction.
The standalone engine scene assets `engine/scenes/smoke.scene.json` and
`engine/scenes/fire.scene.json` can also be parsed and instantiated as entity sets.
Appearance IDs 0 and 1 select smoke and fire; IDs 2 and above are custom.

- WASD: move; mouse: look; Q/E: down/up; Shift: sprint.
- P: pause/settings; I: inventory; F5/F9: save/load; R: raycast.
- T: Lua-triggered mono one-shot at the enchanted cube, independent of its entity.
- The orange light carries a child AudioSource. Its quiet mono loop follows the
  animated parent's world pose. Approach it, back away, and turn the camera to
  hear distance attenuation and left/right panning. The explicit listener follows
  the host's existing gameplay camera position and rotation.
- A quiet stereo ambience loops as a streamed 2D session voice on Music.
- The pause menu has independent Master, Music, SFX and UI sliders. Music and SFX
  freeze while paused; **Test UI sound** remains audible and tests UI gain.

Audio failure does not prevent gameplay. Check engine diagnostics if no device is
available. Saves contain authored components, not playback cursors; session startup
restarts the demonstration. LoadGame ends the previous audio session, including
session music, and the replacement Lua `OnStart` starts a new ambience voice from
the beginning. Ordinary level changes preserve session music. Settings are
demonstration values, not persisted options.

## Generated assets and licensing

`audio/effect.wav` (1 s mono), `audio/orbit-mono.wav` (2 s mono loop), and
`audio/ambience-stereo.wav` (8 s stereo loop) are 48 kHz signed 16-bit PCM WAV.
The stereo clip deliberately uses streaming despite its small demonstration size.
Regenerate byte-identical files using Python 3:

```text
python example_game/audio/generate.py
python example_game/audio/generate.py --check
```

The integer-only generator, audio and fixed-UUID sidecars are CC0 1.0; see
`audio/LICENSE.txt`. They use no third-party recordings. Keep each `.meta` beside
its WAV. CMake copies/installs the whole audio directory, including its license.
WAV files follow this example's Git LFS rules; fetch LFS content on clone, or
regenerate these three files with the command above.
This dedication applies to these generated audio assets and their generator;
other example assets retain their respective repository/adjacent notices.
Deployed dependency notices are under `licenses/`; miniaudio's vcpkg copyright
notice is included by the shared deployment helper.
