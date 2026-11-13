# UI performance measurements

## Reproduce

`ui_benchmark` is an explicit developer target in `tests/CMakeLists.txt`, excluded
from the default build and CTest. Its implementation is `tools/ui_benchmark.cpp`.
Run from the repository root with the normal Windows build dependencies available:

```powershell
cmake --preset windows-example-game -DSRI_BUILD_TESTS=ON
cmake --build build/windows-example-game --config RelWithDebInfo --target ui_benchmark -j 6
& ./build/windows-example-game/bin/RelWithDebInfo/ui_benchmark.exe
& ./build/windows-example-game/bin/RelWithDebInfo/ui_benchmark.exe --no-ui
```

Both executable modes completed with exit code 0 in the measured run. Only the
benchmark target and its dependencies were built. Existing warnings from engine
headers/sources remain in the build output.

## Machine and build

Observed on **2026-11-13, 19:17:40 local time** (the separate `--no-ui` run
started at 19:17:57):

| Item | Observed value |
| --- | --- |
| Computer | DESKTOP-6HN0RUK |
| Windows native version | 10.0.26200 |
| CPU | AMD Ryzen 9 9950X3D 16-Core Processor |
| Logical processors | 32 |
| GPU selected by the engine | NVIDIA GeForce RTX 5070 Ti |
| DXGI dedicated video memory | 16,771,973,120 bytes |
| Compiler | MSVC `_MSC_FULL_VER=193833145` (19.38.33145) |
| Generator | Visual Studio 17 2022, x64 |
| Configuration | RelWithDebInfo (`/O2 /Ob1 /DNDEBUG`, symbols enabled) |
| Engine version | 0.3.1 |
| RmlUi | vcpkg 6.3, FreeType enabled |
| Surface | Hidden HWND, 1280 × 720 physical pixels |
| Worker configuration | One task worker plus the main thread |

The production `RenderingContext` requests a hardware D3D11 11.1 device with
`D3D11_CREATE_DEVICE_DEBUG`, including in this optimized configuration. These are
**optimized-build, debug-layer-enabled measurements**, not debug-layer-free
shipping performance.

## Workload and timing boundaries

The tool owns an Engine, InputSubsystem, RenderingSubsystem, ScriptSubsystem,
and a late-registered UiSubsystem. It uses the existing private
`UiRuntimeAccess::RenderOverlay` entry point and public subsystem methods; it
introduces no runtime API. The normal UI overlay callback registration is retained.

The actual catalog-backed `game/scripts/ui/example_ui.lua` autoload creates its
three documents, three models, and four subscriptions. The HUD uses the shipped
font/styles and bindings. Physical **I** opens the real inventory with its twelve
authored supply rows; physical **P** hides inventory and opens the modal pause
menu. The tool verifies inventory pointer capture and the paused state. Stages
run in order **HUD → inventory → pause** in one process.
The measured assets include the current block display defaults, dp headings,
range chrome, and hover tooltip styles; the default player name is Cyrillic.
The current UI subsystem requests a visible cursor while interactive.

For each stage, discard **60 warmup frames**, then take the arithmetic mean of
**300 measured frames**:

* **UI update CPU:** sum of UiSubsystem `Update` and `LateUpdate`, including the
  production event/model/layout work.
* **Whole update CPU:** Input `Update`, UI `Update`, Script `Update`, UI
  `LateUpdate`, frame bookkeeping, and the nested timing calls. Lua therefore
  participates in normal HUD/inventory binding publication. Script update is
  skipped by the normal ScriptSubsystem paused-frame behavior in the pause stage.
* **Render submission CPU:** only `RenderOverlay`, including production D3D11
  renderer state setup/restoration and command submission.
* **GPU:** D3D11 timestamp queries immediately around the overlay, validated with
  a timestamp-disjoint query. Target binding/clearing occur before the start
  timestamp. Query polling/Flush/wait happen outside the CPU timing windows.
  Disjoint timestamps or query failures cause the tool to fail.

No Engine `Tick`, scene rendering, or `Present` is called. This avoids the
production Present/v-sync path. The swapchain backbuffer's RTV is borrowed as the
overlay target. A synthetic 1/60-second frame advances the direct subsystem
calls; Engine-global `GetFrame()`/Lua Time remains at its initial snapshot because
there is no engine tick. Thus this is a stable document/binding workload, not an
advancing gameplay-clock benchmark. GPU completion is synchronized every frame,
so this measures serialized overlay costs rather than naturally pipelined game
throughput. The scene background is a constant clear color.

## Observed CPU/GPU means

All numbers are **milliseconds per frame**, from the successful run above.

| Visible UI | UI update CPU | Whole update CPU | Render submission CPU | GPU overlay |
| --- | ---: | ---: | ---: | ---: |
| HUD | 0.017818 | 0.026589 | 0.027897 | 0.011287 |
| HUD + inventory (12 rows) | 0.037958 | 0.055605 | 0.116957 | 0.018237 |
| HUD + modal pause menu | 0.030403 | 0.031125 | 0.067348 | 0.016299 |

These are single-run means on this machine, with no background-load or clock
locking controls. They are not worst-case bounds, percentile measurements,
whole-game frame times, or a before/after UI integration speed comparison.

## Process memory

`GetProcessMemoryInfo` with `PROCESS_MEMORY_COUNTERS_EX` supplies private committed
bytes (`PrivateUsage`) and working-set bytes. Baseline is taken after Engine and
Rendering initialization, backbuffer acquisition, and query creation, immediately
before adding UI. Runtime-ready is immediately after UI initialization, before
starting Lua. Loaded-stage snapshots follow their warmup and measured frames.

| Stage | Private bytes | Working-set bytes | Private-byte delta vs before UI |
| --- | ---: | ---: | ---: |
| Process start, before Engine/runtime initialization | 2,940,928 | 10,186,752 | — |
| Before UI | 161,071,104 | 52,568,064 | 0 |
| Empty UI runtime ready | 162,816,000 | 56,766,464 | +1,744,896 (1.66 MiB) |
| HUD loaded/rendered | 192,319,488 | 67,301,376 | +31,248,384 (29.80 MiB) |
| Inventory loaded/rendered | 194,457,600 | 69,296,128 | +33,386,496 (31.84 MiB) |
| Pause menu loaded/rendered | 194,723,840 | 69,505,024 | +33,652,736 (32.09 MiB) |

The autoload loads all three document DOMs and models up front; “HUD” means HUD
visible, not HUD as the only loaded document. Later stages retain previous
allocations/caches. Loaded deltas include Lua session startup, font atlases,
documents/models, UI rendering resources, allocator retention, and any process/
driver allocations made during these stages. They do not isolate RmlUi heap use
and do not measure dedicated GPU resource consumption.

The separate `--no-ui` run observed process-start private/working-set bytes of
2,945,024 / 10,194,944, and before-UI bytes of **162,279,424 / 52,973,568**. It
initializes the same base engine/graphics fixture and exits before UI registration.
Its difference from the first process illustrates process/driver variability;
the deltas in the table deliberately use the same-process baseline.

Both modes are the same executable: the OS loader has already loaded its linked
UI DLL dependencies before the first snapshot. These snapshots measure runtime
initialization/content growth, not the total process cost of linking UI into a
previously UI-free engine.

## Optimized binary sizes

On 2026-11-13, a detached worktree at pre-UI tracked HEAD
`b67fcdf2d03013a56d71a50945546443723bfcb8` was built without source changes.
The current measurements are the rebuilt integrated working-tree binaries in
`build/windows-vs2022/bin/RelWithDebInfo`. File sizes are on-disk bytes:

| Binary | Pre-UI bytes | Current bytes | Delta bytes |
| --- | ---: | ---: | ---: |
| ExampleGame.exe | 7,951,360 | 9,363,456 | +1,412,096 |
| Engine.dll | 8,102,912 | 9,523,200 | +1,420,288 |

Both builds use Visual Studio 17 2022, x64, MSVC 19.38.33145
(`14.38.33130` tools), Windows SDK 10.0.26100.0, and RelWithDebInfo:
`/Zi /O2 /Ob1 /DNDEBUG` and `/debug /INCREMENTAL` for executable and shared
library links. The baseline used `SRI_BUILD_EDITOR=OFF`, `SRI_BUILD_TESTS=OFF`,
and `SRI_BUILD_EXAMPLE_GAME=ON`; the existing current build has editor and tests
enabled. The baseline used the repository vcpkg toolchain with
`VCPKG_MANIFEST_INSTALL=OFF` and the existing
`build/windows-example-game/vcpkg_installed` directory. The current build uses
its own `build/windows-vs2022/vcpkg_installed`; shared installed package version
fields match, though identical dependency binaries were not established. These
are therefore measured file-size comparisons with matching compiler and primary
configuration, not a controlled attribution solely to UI. Incremental linking,
other working-tree changes, and dependency builds can affect sizes.

ExampleGame links the static engine, so its executable and the separately built
shared Engine.dll are alternative engine payloads; their deltas should not be
added as one game payload. PDBs, external DLLs, and content are excluded.

### UI dependency payload

The tool reports actual `std::filesystem::file_size` values for DLLs in its output
directory and the shipped font. The UI dependency payload observed in
`build/windows-example-game/bin/RelWithDebInfo` was:

| File | Bytes |
| --- | ---: |
| rmlui.dll | 1,736,192 |
| freetype.dll | 707,072 |
| z.dll | 91,648 |
| bz2.dll | 75,776 |
| libpng16.dll | 197,632 |
| brotlidec.dll | 52,224 |
| brotlicommon.dll | 137,728 |
| engine/ui/fonts/DejaVuSans.ttf | 756,072 |
| **Listed dependency payload + font** | **3,754,344** |

This is an on-disk dependency payload, separate from the binary sizes above.
Some transitive dependencies may also be shared with existing
engine dependencies. PDBs, runtime system DLLs, authored UI documents/styles,
and changes to engine code size are not included in this total. The tool also
prints the other DLL sizes in the directory; directory enumeration does not
prove which DLLs are newly required by UI.

The isolated benchmark output originally contained neither `ExampleGame.exe`
nor the shared engine DLL because only the benchmark target was built there.
