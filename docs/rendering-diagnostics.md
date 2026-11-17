# Rendering diagnostics and debug views

The viewport status area reports renderer startup and runtime problems, scene asset issues and useful loading information. Use **Details** for the original operation and error, and **Show Log** to inspect the existing Log dock. Startup errors appear even when the engine tick never starts.

## Reading status

- **Renderer not initialized**: no active renderer. **Ready** means required rendering resources and passes are usable.
- **Rendering suspended**: the surface is unavailable, for example while minimized or zero-sized. This is not a renderer failure. A previously initialized surface resumes when restored. If the initial surface has zero extent, initialization returns without starting the GPU; the editor retries full engine initialization once it has a drawable extent. Other hosts must provide the same retry boundary.
- **Renderer failed**: inspect the operation, pass, path and original native or shader compiler details in Details and Log. Device removal information, when available, supplements the original error. Failure is retained through shutdown; recovery requires an explicit shutdown and new initialization attempt, not a resize or per-frame retry. There is no automatic device recovery.
- **Asset catalog error**, preparing references, or failed/unavailable scene renderer assets identify separate asset problems, not necessarily a failed renderer. Details can include paths, UUIDs and the original request error; if the event was missed, consult the Log. Scene replacement, reference removal and repair update the scene-scoped status.
- **No drawable scene items submitted** is informational. Submitted items are not a count of visible pixels; a skinned mesh may submit several sections. In Edit, the editor camera is valid without a scene camera. In Play, no active scene camera leaves the current RenderWorld camera in use (last camera or default).

## Viewport selector

Choose a mode in the viewport toolbar. Selection is transient, defaults to **Final** on initialization and does not change the scene or Play state. The selector is disabled when the renderer is unavailable.

| Mode | Display |
| --- | --- |
| Final | Lit scene with the existing Reinhard tone mapping. |
| Albedo | Stored surface color without lighting or Reinhard tone mapping. |
| Normals | Already-encoded world-space normal RGB (`normal * 0.5 + 0.5`). |
| Depth | Linear positive camera-space depth visualized using the reverse-Z projection: near is bright, far is dark, and cleared background is black. Depth at the far plane shares the clear value and is black. |

Selection outlines appear only in Final. Gizmos, host diagnostic labels and engine UI remain overlays, so they can cover diagnostic pixels. They are composited in the established order before a single Present; disable or exclude overlays when inspecting raw diagnostic pixel values.

## Session log

**Open Log File** and **Open Log Folder** use the active sink path. The preferred location is `logs/editor.log` beside the editor executable; if that cannot be opened, the editor tries `logs/editor.log` under the writable application-local data location and reports the fallback. Paths support non-ASCII characters. Logs rotate with bounded retention, and each session records a boundary, engine version, executable and selected project. Warnings and errors are flushed promptly.

If neither location opens, the status reports the problem and the editor remains usable. If a previously opened file sink later fails during writing, rotation or flushing, it disables itself and reports the error to stderr; other sinks continue receiving messages.

## Developer reference and verification

See the [renderer API contract](rendering-diagnostics-contract.md) for ownership, lifecycle and `ShaderContext` migration, and the [asset event contract](asset-event-correlation-contract.md) for attempt correlation and lifetime.

With the Windows VS 2022 preset and editor/test dependencies installed, the focused targets can be configured, built and run from the repository root:

```powershell
cmake --preset windows-vs2022 -DSRI_BUILD_TESTS=ON
cmake --build build/windows-vs2022 --config Debug --target editor rendering_diagnostics_tests rendering_resource_tests render_debug_view_tests render_debug_native_tests asset_event_tests asset_catalog_tests editor_rendering_status_tests editor_rendering_integration_tests session_log_tests viewport_overlay_tests viewport_input_tests ui_render_tests rendering_skinning_tests log_model_tests --parallel 4
ctest --test-dir build/windows-vs2022 -C Debug -R '^(rendering_diagnostics_tests|rendering_resource_tests|render_debug_view_tests|render_debug_native_tests|asset_event_tests|asset_catalog_tests|editor_rendering_status_tests|editor_rendering_integration_tests|session_log_tests|viewport_overlay_tests|viewport_input_tests|ui_render_tests|rendering_skinning_tests|log_model_tests)$' --output-on-failure
```

For the optional real-project native viewport check, set `SRI_TEST_PROJECT` to a project file before running `viewport_overlay_tests`. Automated checks do not replace a visual editor inspection of mode switching, resize/minimize/restore, status and overlays.

For native editor screenshots, set `SRI_RENDERING_SCREENSHOT_DIR` to a temporary output directory before running `editor_rendering_integration_tests`. This optional path shows the real editor, waits for scene assets, captures Final / Albedo / Normals / Depth / restored Final and the startup-error UI, and requires an unobscured interactive desktop. The ordinary test run writes no screenshots. Inspect the captures separately from the automated assertions.

### Milestone verification

The configured Windows VS 2022 Debug build completed for the editor and all 14 focused test targets above; all 14 CTest targets passed. WARP pixel readback and native production-pipeline checks include static/skinned geometry, reverse-Z depth, repeated switching, resize/suspend/restore, overlay ordering, failure cleanup and restart. The optional `viewport_overlay_tests` run with `SRI_TEST_PROJECT=C:\Work\Gamedev\Projects\Test1\Test1\Test1.sriproject` also passed.

Six screenshots of the native editor were inspected: the four views, returning to Final, and the fatal-startup banner with the existing Log dock open. This was a screenshot-based visual pass on the deterministic fixture, not a full interactive editing session. Opening the log through the operating system's associated application remains a manual check; Unicode paths, active-path selection, fallback and sink behavior have automated coverage.
