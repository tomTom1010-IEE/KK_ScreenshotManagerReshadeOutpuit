# KK/KKS ScreenshotManager ReShade Output 中文使用说明

> 这是使用者说明，只覆盖安装、版本选择、导出、输出格式和排错。开发、编译和实现细节请看英文 README。

这个插件是修改版 Screencap/ScreenshotManager，用于从 Koikatu (KK) 和 Koikatsu Sunshine (KKS) 的离屏渲染截图流程中导出 Offline ReShade 需要的输入文件：

- `coloroutput.png`
- `depthoutput.rfloat`
- `metadata.json`

这些文件生成正确之后，才进入 Offline ReShade app 侧继续处理。

## KK Hotfix 21.1.3.4

本次 [KK 深度 hotfix](https://github.com/tomTom1010-IEE/KK_ScreenshotManagerReshadeOutpuit/releases/tag/KKHotfix-21.1.3.4) 修复离屏截图偶发缺深度的问题，改进捕获时序、hook 覆盖和本帧写入验证。

- 仅适用于 KK x64 / D3D11。
- 必须一起更新本包的 `Screencap.dll` 和 `OfflineDepthD3D11Bridge.dll`，对应 bridge API 4。
- 不需要额外安装 MinHook DLL 或旧的 replacement shader bundle。
- 配套 VideoExport 不需要重新安装，继续使用 `Normal + Depth`。
- KKS 不使用本包，继续使用[上一正式版的 KKS 发布包](https://github.com/tomTom1010-IEE/KK_ScreenshotManagerReshadeOutpuit/releases/tag/Release0704_main)。旧 KK Stable/Compatibility 发布包也保留不变。

阶段诊断与环境指纹日志在正式 hotfix 中默认关闭。旧配置不会自动重置，从测试版升级时请手动检查这些选项。

## 安装

1. 为对应游戏安装 BepInEx 5 和常规 BepisPlugins 依赖。
2. 安装对应游戏的 Screencap DLL，替换 BepisPlugins 子目录里的原版文件：
   - KK：`BepInEx\plugins\KK_BepisPlugins\Screencap.dll`
   - KKS：`BepInEx\plugins\KKS_BepisPlugins\KKS_Screencap.dll`
3. KK 还需要安装 `OfflineDepthD3D11Bridge.dll`：
   - 推荐放在 `BepInEx\plugins\KK_BepisPlugins\Screencap.dll` 旁边；
   - 或者在插件设置 `Offline ReShade Export > D3D11 bridge DLL path` 中填写绝对路径。
4. 启动游戏后，先按 `LeftCtrl + F10` 做一次测试导出。
5. 确认 `UserData\cap\OfflineReShade` 中生成了：

```text
coloroutput.png
depthoutput.rfloat
metadata.json
```

不要在这一步通过之前进入 Offline ReShade app。app 只读取这些文件，无法修复游戏侧没有导出或导出为空的问题。

## 发布版本选择

旧版提供以下三类包，本次另增加 KK Depth Hotfix：

- **KKS Screencap**
  - 只需要 `KKS_Screencap.dll`。
  - KKS 使用 Unity 2019.4 的 camera depth texture 路径，不需要 native D3D11 bridge。
- **KK Stable Path**
  - 包含 `Screencap.dll` 和 `OfflineDepthD3D11Bridge.dll`。
  - 路径更轻，性能最好。
  - KK 用户请先尝试这个版本。
- **KK Compatibility Bridge**
  - 也包含 `Screencap.dll` 和 `OfflineDepthD3D11Bridge.dll`。
  - 会在多次截图之间重新确认和修复 D3D11 context hook。
  - 如果 stable 版本不能稳定生成深度，请换用这个版本。

KK 两个版本输出同样的 `.rfloat` 深度格式和 metadata。不要混用两个包里的 DLL：`Screencap.dll` 和 `OfflineDepthD3D11Bridge.dll` 必须来自同一个发布包。

**KK Depth Hotfix 21.1.3.4** 在 compatibility 路径上增加持久入口 hook、GPU 本帧写入证据及受预算限制的纹理复用，输出格式保持不变。旧 stable 或 compatibility 偶发缺深度时，请升级到这个 hotfix，而不是混用新旧 DLL。

推荐 KK 选择流程：

1. 新 KK 用户或遇到缺深度的用户，安装 **KK Depth Hotfix**。
2. 在 Studio 中按 `LeftCtrl + F10`，检查 color/depth 是否都能生成。
3. 已经使用旧 **KK Stable Path** 或 **KK Compatibility Bridge** 且始终稳定的用户可以继续保留。若不生成深度、重启后只有第一张成功，或 VE 偶发缺 depth，请成对升级两个 DLL。
4. hotfix 仍失败时，关闭 MSAA，尝试 Studio 镜头/相机视角，测试时关闭 Optimize in Background；需要支持日志时再开启诊断选项。

## 设置

ConfigurationManager 中的设置分组是：

```text
Offline ReShade Export
```

常用设置：

- `Export color and depth`
  - 默认热键：`LeftCtrl + F10`
- `Export directory`
  - 默认：`UserData\cap\OfflineReShade`
- `Archive color/depth in screenshot directory`
  - 同时在 `UserData\cap` 中保存带时间戳的 `Color` / `Depth` 归档文件。
- `Log export timings`
  - 输出性能计时日志，排查性能问题时再打开。
- KKS `Depth output format`
  - 默认 `RawRFloat`。
- KK `D3D11 bridge DLL path`
  - 可选。用于手动指定 native bridge DLL 路径。
- KK `D3D11 bridge log path`
  - native bridge 诊断日志路径。
- KK `D3D11 bridge candidate diagnostics`
  - 用于排查空深度、错深度或错分辨率。正常截图和录制时建议关闭。
- KK `D3D11 bridge capture stage diagnostics`
  - 相机结束、清除前和相机栈结束时的小尺寸诊断采样。正式 hotfix 默认关闭，正常截图和录制也请关闭。
- KK `D3D11 bridge readback budget MiB`
  - 默认 `512`，限制桥接转换与读回纹理池，而不是整个游戏的内存或显存。超大截图可能需要提高；至少需要两张全分辨率 float 纹理和少量临时空间。
- KK `D3D11 bridge memory pressure log`
  - 记录内存、显存和纹理池状态。即使关闭，分配及读回失败仍会记录。
- KK `KK environment fingerprint log`
  - 排查环境差异时使用的详细日志，正式 hotfix 默认关闭。

关闭 candidate/stage diagnostics 不会关闭本帧写入验证，插件仍会拒绝未经验证的旧深度。

## 截图分辨率

Offline ReShade 的截图分辨率仍由 ScreenshotManager 原本的 render screenshot 设置控制。color 和 depth 必须来自同一次离屏渲染，所以它们的尺寸设置需要一起检查。

建议：

- 把 ScreenshotManager render resolution 设置为你希望 app 处理的最终图片尺寸。
- 谨慎设置 downscaling/supersampling。超采样越高，渲染、读回和 depth 文件都会更大。
- 修改分辨率或 downscaling 后，先按一次 `LeftCtrl + F10`。
- 打开 `metadata.json`，确认 `width`、`height`、`depthWidth`、`depthHeight` 是预期值。
- 确认 color/depth 尺寸和内容正确之后，再进入 app 侧调 ReShade。

## 输出文件

默认实时交接目录：

```text
UserData\cap\OfflineReShade\
  coloroutput.png
  depthoutput.rfloat
  metadata.json
```

开启归档输出后，普通截图目录也会生成带时间戳的副本：

```text
UserData\cap\CharaStudio-YYYY-MM-DD-HH-MM-SS-Color.png
UserData\cap\CharaStudio-YYYY-MM-DD-HH-MM-SS-Depth.rfloat
```

VideoExport 的 `Normal + Depth` 会在帧目录生成：

```text
UserData\VideoExport\Frames\<timestamp>\
  0.png
  0.depth.rfloat
  1.png
  1.depth.rfloat
  metadata.json
```

## 深度格式

当前发布版主格式：

- 扩展名：`.rfloat`
- 编码：`rfloat32_device_depth_little_endian`
- 数据类型：little-endian IEEE 754 `float32`
- 尺寸：`metadata.depthWidth * metadata.depthHeight`
- 行顺序：`bottom_to_top`
- 数值：D3D/Unity device depth，范围 `0..1`
- reversed-Z：看 `metadata.reshadeDepthReversed`

常用 metadata 字段：

- `width`, `height`：color 输出尺寸
- `depthWidth`, `depthHeight`：depth 文件尺寸
- `nearClip`, `farClip`, `fov`, `aspect`：相机参数
- `downscalingRate`：ScreenshotManager 超采样/下采样倍率
- `depthEncoding`, `depthFileExtension`, `depthByteOrder`, `depthRows`
- `depthCaptureSource`：KKS Unity hardware depth 或 KK D3D11 bridge depth
- `reshadeDepthReversed`, `reshadeDepthUpsideDown`, `reshadeFarPlane`

## KK 注意事项

KK 使用 Unity 5.6.2，不能可靠通过 Unity API 获取离屏 Camera.Render 的高质量深度。因此 KK 发布版使用 D3D11 bridge 在截图渲染窗口中捕获 depth-stencil。

如果 KK 没有深度：

- 优先使用 Studio 镜头/相机视角截图，自由视角在部分环境中不稳定。
- 关闭 MSAA。
- 测试时关闭 Optimize in Background。
- 如果旧 stable path 或 compatibility 不稳定，成对升级到 KK Depth Hotfix。
- 需要排查时再开启 `D3D11 bridge candidate diagnostics`，正常使用请关闭。
- `D3D11 bridge capture stage diagnostics` 也应保持关闭，只在排查需要时开启。

本轮 hotfix 的移动镜头测试在开启和关闭阶段诊断时都正常，但尚未覆盖所有自由视角、插件、驱动组合及真正低内存环境，不能保证这些情况下永不失败。

## 性能口径

正常截图和录制时应关闭 candidate diagnostics、capture stage diagnostics 和 environment fingerprint log。升级前已有的配置值会保留，请手动确认。

此前旧 compatibility 与 stable 在单一 KK 环境中的对比约有 10% 以内差距，这不是新 hotfix 的性能保证。新版本增加了 GPU 写入证据查询，实际开销需同场景验证。本轮关闭阶段诊断后 native 捕获窗口缩短，但整帧导出没有加速，主要瓶颈仍是颜色读回与 PNG 编码。不能用不同镜头或不同 KK/KKS 场景给出通用的性能损失百分比。

## 验证流程

安装后建议按这个顺序检查：

1. 进入 Studio。
2. 设置好 ScreenshotManager render resolution。
3. 按 `LeftCtrl + F10`。
4. 检查 `UserData\cap\OfflineReShade\coloroutput.png` 是否是正确画面。
5. 检查 `depthoutput.rfloat` 是否存在且大小合理。
6. 检查 `metadata.json` 中尺寸是否符合预期。
7. 用你的 depth 可视化工具或 Offline ReShade app 确认深度不是空图、不是错分辨率图。
8. 通过之后，再进行 app 侧 ReShade 参数调试或 VideoExport 批量录制。
