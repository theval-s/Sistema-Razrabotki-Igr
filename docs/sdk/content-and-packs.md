# Game content and packs in C++ hosts

A built game keeps its content in `game.sripak` (see [../building-games.md](../building-games.md)).
A host that runs built games has to set up two things before it creates the `Engine`, as
`sri_runtime` does:

~~~cpp
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/Content.hpp>
#include <engine/core/Pak.hpp>

const auto folder = engine::AssetPathResolver::GetRoot();
if (std::filesystem::is_regular_file(folder / engine::PakFileName)) {
    auto pak = engine::PakSource::Load(folder / engine::PakFileName, error);
    if (!pak) return Fail(error);
    engine::content::Mount(folder, std::move(pak));
}
// A cooked build has a manifest; without one the asset system scans sources and .meta files.
if (engine::content::IsFile(folder / L"manifest.json"))
    engine::AssetPathResolver::SetManifest(folder / L"manifest.json");
~~~

A mounted source answers for every path under its folder; whatever it does not have is read from
disk. Mounts are process-wide, thread-safe, and can be replaced with `Mount` or removed with
`Unmount`. Streams and readers already open keep the pack mapped after it is unmounted.

## Reading game files

`ReadTextFile` and `ReadBinaryFile` (`engine/core/FileSystem.hpp`) already read through mounts.
For anything else that touches game content, use `engine::content` instead of `std::filesystem`
or file streams, or the code will not see packed files:

| Instead of | Use |
|---|---|
| `std::filesystem::is_regular_file` / `exists` | `content::IsFile(path)` |
| `std::filesystem::is_directory` | `content::IsDirectory(path)` |
| `std::ifstream`, `fopen`, a library's own file access | `content::Open(path, error)`: a seekable `content::Stream` |
| `std::filesystem::canonical` | `std::filesystem::weakly_canonical`, then the checks above |

Packed files have no directory listing through `engine::content`; enumerate assets through the
`AssetCatalog`, which a cooked build restores from the manifest.

Writing stays on disk: save games, logs and settings go to real folders.

## Tests and tools

`PakWriter` builds a pack from files and byte buffers; `PakSource::Files()` lists one. Any
`content::Source` implementation can be mounted, for example an in-memory source in a test
(`tests/content_fixture.hpp` in the engine repository).
