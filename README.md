[中文使用说明](README.zh-CN.md) | English

# KK/KKS ScreenshotManager ReShade Output

This fork publishes the Screencap/ScreenshotManager changes needed to export
Offline ReShade inputs from Koikatu and Koikatsu Sunshine. It is intentionally
scoped to the Screencap plugin even though the working tree is based on
BepisPlugins.

## What This Adds

- `Ctrl+F10` Offline ReShade export in ScreenshotManager.
- `coloroutput.png`, depth sidecar, and `metadata.json` in
  `UserData\cap\OfflineReShade`.
- Optional timestamped archive files in the normal screenshot directory.
- KKS hardware/device depth export through Unity 2019.4.
- KK device-depth export through the native D3D11 bridge in
  `Native\OfflineDepthD3D11Bridge`.

## Install

1. Install BepInEx 5 and the usual BepisPlugins dependencies for your game.
2. Build this repository, or use the built Screencap DLL from your local build.
3. Replace the original Screencap DLL inside the game's BepisPlugins folder:
   - KK: `BepInEx\plugins\KK_BepisPlugins\Screencap.dll`
   - KKS: `BepInEx\plugins\KKS_BepisPlugins\KKS_Screencap.dll`
4. For KK only, build `Native\OfflineDepthD3D11Bridge` and put
   `OfflineDepthD3D11Bridge.dll` next to
   `BepInEx\plugins\KK_BepisPlugins\Screencap.dll`, or set
   `Offline ReShade Export > D3D11 bridge DLL path` to the bridge DLL's
   absolute path.
5. Start the game and verify the game-side export before opening the Offline
   ReShade app. Press `LeftCtrl + F10` and confirm that
   `UserData\cap\OfflineReShade` contains a valid `coloroutput.png`,
   `depthoutput.rfloat`, and `metadata.json`.

Do not continue to the app-side ReShade workflow until this check passes. The
app consumes these files; it cannot fix a missing or empty game-side depth
export.

Native bridge build:

```powershell
cmake -S Native\OfflineDepthD3D11Bridge -B Native\OfflineDepthD3D11Bridge\build -G Ninja
cmake --build Native\OfflineDepthD3D11Bridge\build --config Release
```

The native DLL is not required for KKS. KK needs it for high quality
off-screen device depth.

## Release Variants

This project is published as three practical packages:

- **KKS Screencap**: `KKS_Screencap.dll` only. KKS uses Unity 2019.4 camera
  depth texture export and does not need the native D3D11 bridge.
- **KK Stable Path**: `Screencap.dll` + `OfflineDepthD3D11Bridge.dll` built
  from the lighter `main` release path. Use this first on KK installs where
  depth output is already stable and aligned.
- **KK Compatibility Bridge**: `Screencap.dll` + `OfflineDepthD3D11Bridge.dll`
  built from the rehook compatibility path. Use this if the stable package
  loses depth after restart, only captures the first frame, or VideoExport
  intermittently misses depth frames.

Both KK packages export the same `.rfloat` format and metadata schema. The
compatibility package repeatedly verifies and repairs D3D11 context hooks across
captures, which improves reliability on troublesome Unity 5.6/plugin stacks.
Do not mix files between the two KK packages: use the `Screencap.dll` and
`OfflineDepthD3D11Bridge.dll` from the same release package.

Recommended KK selection flow:

1. Install **KK Stable Path** first.
2. Press `LeftCtrl + F10` in Studio and inspect the exported color/depth pair.
3. If stable does not create `depthoutput.rfloat`, creates it only once after a
   restart, misses depth intermittently, or produces VideoExport frames without
   matching depth sidecars, replace both DLLs with **KK Compatibility Bridge**.
4. If compatibility still fails, disable MSAA, use a Studio camera/lens view
   instead of free view, and capture a support log with
   `D3D11 bridge candidate diagnostics` enabled.

## Performance Notes

Normal capture should keep `D3D11 bridge candidate diagnostics` disabled. The
candidate diagnostics mode is for support logs only and adds measurable overhead.

In same-KK testing, the compatibility bridge is within roughly 10% of the
lighter stable path. This is the expected release tradeoff: if a KK environment
cannot use the stable path reliably, the compatibility package gives stable
color/depth output with a small performance cost. Most remaining frame time is
spent in color readback and PNG encoding rather than the D3D11 depth bridge.

Do not use unrelated KK/KKS scenes to calculate exact performance deltas. Scene
complexity, texture size, ScreenshotManager supersampling, MSAA, and whether the
depth sidecar is written at `3840x2160` or `7680x4320` can dominate the result.
KKS performance numbers are useful as a sanity check, not as a strict comparison
against KK.
## Settings

ConfigurationManager section: `Offline ReShade Export`

- `Export color and depth`: default hotkey is `LeftCtrl + F10`.
- `Export directory`: default is `UserData\cap\OfflineReShade`.
- `Archive color/depth in screenshot directory`: also writes timestamped
  `...-Color.png` and `...-Depth.*` files to `UserData\cap`.
- `Log export timings`: writes detailed timing diagnostics to the game log.
- KKS `Depth output format`: defaults to `RawRFloat`.
- KK `D3D11 bridge DLL path`: optional explicit path to the native bridge.
- KK `D3D11 bridge log path`: native bridge diagnostic log path.
- KK `D3D11 bridge candidate diagnostics`: slow candidate sampling log for
  debugging wrong/empty depth frames. Keep it disabled for normal capture.

Screenshot resolution is still controlled by ScreenshotManager's normal render
screenshot settings. For Offline ReShade, color and depth must describe the same
off-screen render:

- Set the ScreenshotManager render resolution to the final image size you want
  the app to process.
- Keep the ScreenshotManager downscaling/supersampling settings intentional.
  High supersampling increases both render cost and depth file size.
- After changing resolution or downscaling, take one `LeftCtrl + F10` test shot
  and confirm that `metadata.json` reports the expected `width`, `height`,
  `depthWidth`, and `depthHeight`.
- Do not judge app-side depth effects until this game-side color/depth pair is
  confirmed to be the expected size and visually aligned.

## Output Files

Default realtime handoff directory:

```text
UserData\cap\OfflineReShade\
  coloroutput.png
  depthoutput.rfloat
  metadata.json
```

When archive output is enabled, the normal screenshot folder also receives
timestamped copies:

```text
UserData\cap\CharaStudio-YYYY-MM-DD-HH-MM-SS-Color.png
UserData\cap\CharaStudio-YYYY-MM-DD-HH-MM-SS-Depth.rfloat
```

VideoExport `Normal + Depth` writes one pair per frame:

```text
UserData\VideoExport\Frames\<timestamp>\
  0.png
  0.depth.rfloat
  1.png
  1.depth.rfloat
  metadata.json
```

## Depth Format

Current main format:

- extension: `.rfloat`
- encoding: `rfloat32_device_depth_little_endian`
- element type: little-endian IEEE 754 `float32`
- dimensions: `metadata.depthWidth * metadata.depthHeight`
- row order: `bottom_to_top`
- value: D3D/Unity device depth in `0..1`
- reversed-Z flag: `metadata.reshadeDepthReversed`

`metadata.json` fields:

- `width`, `height`: color output size
- `depthWidth`, `depthHeight`: depth sidecar size
- `nearClip`, `farClip`, `fov`, `aspect`: camera parameters
- `downscalingRate`: ScreenshotManager supersampling rate
- `colorResolve`: currently `screenshotmanager_lanczos`
- `depthEncoding`, `depthFileExtension`, `depthByteOrder`, `depthRows`
- `depthCaptureSource`: Unity hardware depth on KKS or KK D3D11 bridge depth
- `reshadeDepthReversed`, `reshadeDepthUpsideDown`, `reshadeFarPlane`

## Algorithm Notes

KKS uses Unity 2019.4's camera depth texture path. ScreenshotManager renders
the off-screen camera at the requested supersampled size, copies/reads device
depth, and exports it as raw float depth.

KK uses Unity 5.6.2, where `BuiltinRenderTextureType.Depth` is not reliable for
this off-screen render path. The native bridge hooks the Unity D3D11 immediate
context during the `Camera.Render()` window, records depth-stencil views, checks
readable non-MSAA candidates for nonzero content, copies the selected depth to
an `R32_FLOAT` texture, then writes delayed staging readbacks to `.rfloat`.
Empty candidates are not written; the plugin logs a warning instead of saving
fake depth.

KK reliability notes:

- Use a Studio camera/lens view for stable depth capture. Free-view screenshots
  can miss the D3D11 depth pass in Unity 5.6.
- Disable MSAA if depth is missing or the log reports MSAA/non-readable DSVs.
- Disable Optimize in Background while testing user reports.
- Candidate diagnostics are for troubleshooting only and should stay off for
  normal recording.

This bridge is archived here rather than in a separate repository because it is
tightly coupled to ScreenshotManager's off-screen export timing and metadata.

---

# BepisPlugins
A collection of essential [BepInEx](https://github.com/BepInEx/BepInEx) plugins for Koikatu / Koikatsu Party, EmotionCreators, AI-Shoujo / AI-Girl, HoneySelect2, HoneyCome, SamabakeScramble / Summer Vacation Scramble, and other games by Illusion/Illgames. Check plugin descriptions below for a full list of included plugins. 

If you wish to contribute or need help, check the #help channel on the [Koikatsu discord server](https://discord.gg/hevygx6).

### How to install
1. Install the latest version of [BepInEx](https://github.com/BepInEx/BepInEx). Make sure it is installed and working before installing BepisPlugins.
   - For HoneySelect2 and games older than it, get BepInEx 5.
   - For RoomGirl/HoneyCome and games newer than it, get BepInEx 6 (nightly build 668 or later).
2. Install the latest version of the [ConfigurationManager](https://github.com/BepInEx/BepInEx.ConfigurationManager) plugin.
3. Download the latest release archive for your game (specified by the two letter prefix, e.g. AI for AI-Girl) from the releases page (not the "Clone or download" button).
4. Extract the archive into your game directory (where the game exe and BepInEx folder are located). Replace old files if asked.

## Plugin descriptions
You can see more information about some of the plugins by checking their config files in `BepInEx\config` (or by using the in-game [ConfigurationManager plugin](https://github.com/BepInEx/BepInEx.ConfigurationManager)).

Note: Not all plugins might be available for a given game (not yet ported by anyone, or technically infeasible).

### BGMLoader
Loads custom BGMs and clips played on game startup. Stock audio is replaced during runtime by custom clips from BepInEx\BGM and BepInEx\IntroClips directories.

[Tutorial on how to replace sound clips and background music using BGMLoader.](https://gitgoon.dev/IllusionMods/BepisPlugins/wiki/BGM-Loader)

### ColorCorrector
Allows configuration of some post-processing filters. (change of bloom amount, disable saturation filter)

### ExtensibleSaveFormat
Allows additional data to be saved to character, coordinate and scene cards. The cards are fully compatible with non-modded game, the additional data is lost in that case. This is used by sideloader to store used mod information.

### InputUnlocker
Allows user to input longer than normal values to InputFields. This allows longer names and other properties stored as text.

### Screencap
Creates screenshots based on settings. Can create screenshots of much higher resolution than what the game is running at. It can make screen (F9 key) or character (F11 key) screenshots.

### Sideloader
Loads mods packaged in .zip archives from the Mods directory without modifying the game files at all. You don't unzip them, just drag and drop to Mods folder in the game root.

It prevents mods from colliding with each other (i.e. 2 mods have same item IDs and can't coexist; sideloader automatically assigns correct IDs). It also makes it easy to disable/remove mods with no lasting effects on your game install (just remove the .zip, no game files are changed at any point).

> Note: Sideloader is not available for games by Illgames because of technical reasons (IL2CPP). You will have to use [SardineTail](https://github.com/MaybeSamigroup/SVS-SardineTail/wiki) for them instead.

[More information and tutorial on sideloader-compatible mod creation.](https://gitgoon.dev/IllusionMods/BepisPlugins/wiki/1-Introduction-to-zipmod-format)

[Step-by-step guide for creating a simple texture mod.](https://gitgoon.dev/IllusionMods/BepisPlugins/wiki/2-How-to-create-a-simple-zipmod)

[Tool for automatically converting old list mods to sideloader-compatible form.](https://gitgoon.dev/IllusionMods/ZipStudio/releases)

### SliderUnlocker
Allows user to set values outside of the standard 0-100 range on all sliders in the editor.

### IMGUIModule.Il2Cpp.CoreCLR.Patcher
Fixes issues with IMGUI caused by the game being IL2CPP that prevent other plugins like ConfigurationManager from being displayed correctly.

## Removed plugins

### Configuration Manager
Moved to https://github.com/BepInEx/BepInEx.ConfigurationManager

### DeveloperConsole
Moved to https://github.com/BepInEx/DeveloperConsole

### IPALoader
Moved to https://github.com/BepInEx/IPALoaderX

### MessageCenter
Moved to https://github.com/BepInEx/MessageCenter

### ScriptEngine
Moved to https://github.com/BepInEx/BepInEx.Debug

## Obsolete plugins
### DynamicTranslationLoader
Replaced by [XUnity.AutoTranslator](https://github.com/bbepis/XUnity.AutoTranslator)

### ResourceRedirector
Replaced by [XUnity.ResourceRedirector](https://github.com/bbepis/XUnity.AutoTranslator)
