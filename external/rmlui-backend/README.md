# RmlUi DX11 renderer

These files originate from [RmlUi](https://github.com/mikke89/RmlUi)
tag **6.3**, commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413`;
the renderer has local engine adaptations described below:

- `Backends/RmlUi_Renderer_DX11.cpp`
- `Backends/RmlUi_Renderer_DX11.h`
- `Backends/RmlUi_Include_Windows.h`
- `LICENSE.txt`

The renderer embeds its HLSL shader sources in the `.cpp` file. It is compiled
against the separately installed RmlUi Core, not a second copy of the core.
RmlUi is distributed under the MIT license; see `LICENSE.txt`.
## Engine adaptations

The engine subclasses the renderer in `engine/src/ui/UiDx11Renderer.cpp` to resolve
assets and decode premultiplied PNGs with WIC. It isolates the entire D3D11 context
state using a direct D3D11.1 context-state swap and borrows the final scene target
only during the overlay call. No backend
window, swap chain, message loop, or presentation is used. The vendored constructor
has guarded D3D11.1/shader failure paths and an `IsValid()` readiness query; the
upstream clip-mask, transform, layer, filter, and shader implementation is retained.
