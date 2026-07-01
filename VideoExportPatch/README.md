# VideoExport Normal + Depth patch

This directory contains the VideoExport-side changes for Offline ReShade Capture.

It is intentionally kept outside the BepisPlugins solution/project files, so it does not affect building `KKS_Screencap.dll`.

## Files changed upstream

Apply `videoexport-normal-depth.patch` to a HSPlugins/VideoExport source tree, or copy the files from `modified-files/` over the matching paths:

- `VideoExport.Core/ScreenshotPlugins/ScreencapPlugin.cs`
- `VideoExport.Core/VideoExport.cs`

## What it does

- Adds Screenshot Manager capture type `Normal + Depth`.
- Forces PNG color frame output for that capture type.
- Calls `Screencap.ScreenshotManager.ExportOfflineReShadeInputs(...)` by reflection.
- Writes frame color to the normal VideoExport frame path.
- Writes matching depth as `<frame>.depth.rfloat` or `<frame>.depth.png`, depending on ScreenshotManager depth output format.
- Writes per-sequence `metadata.json` in the same frame directory.

## Build note

Build VideoExport from the HSPlugins source tree, not from this BepisPlugins repository. The latest local build output was produced under:

`C:\tmp\HSPlugins\bin\build\VideoExport.KKS\VideoExport.dll`

Do not commit `bin/build` or `bin/out` outputs to source control; publish them as release artifacts if needed.
