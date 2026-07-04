# KK/KKS ScreenshotManager ReShade Output 中文使用说明

> 这是使用者说明，只覆盖安装、版本选择、导出、输出格式和排错。开发、编译和实现细节请看英文 README。

这个插件是修改版 Screencap/ScreenshotManager，用于从 Koikatu (KK) 和 Koikatsu Sunshine (KKS) 的离屏渲染截图流程中导出 Offline ReShade 需要的输入文件：

- `coloroutput.png`
- `depthoutput.rfloat`
- `metadata.json`

这些文件生成正确之后，才进入 Offline ReShade app 侧继续处理。

## 安装

1. 为对应游戏安装 BepInEx 5 和常规 BepisPlugins 依赖。
2. 安装对应游戏的 Screencap DLL，替换 BepisPlugins 子目录里的原版文件：
   - KK：`BepInEx\plugins\KK_BepisPlugins\Screencap.dll`
   - KKS：`BepInEx\plugins\KKS_BepisPlugins\KKS_Screencap.dll`
4. KK 还需要安装 `OfflineDepthD3D11Bridge.dll`：
   - 推荐放在 `BepInEx\plugins\KK_BepisPlugins\Screencap.dll` 旁边；
   - 或者在插件设置 `Offline ReShade Export > D3D11 bridge DLL path` 中填写绝对路径。
5. 启动游戏后，先按 `LeftCtrl + F10` 做一次测试导出。
6. 确认 `UserData\cap\OfflineReShade` 中生成了：

```text
coloroutput.png
depthoutput.rfloat
metadata.json
```

不要在这一步通过之前进入 Offline ReShade app。app 只读取这些文件，无法修复游戏侧没有导出或导出为空的问题。

## 发布版本选择

发布包分为三类：

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

推荐 KK 选择流程：

1. 先安装 **KK Stable Path**。
2. 在 Studio 中按 `LeftCtrl + F10`，检查 color/depth 是否都能生成。
3. 如果 stable 不生成 `depthoutput.rfloat`、重启后只有第一张有深度、偶发缺深度，或 VideoExport 帧序列偶发缺 depth，请替换为 **KK Compatibility Bridge**。
4. 如果 compatibility 仍失败，关闭 MSAA，使用 Studio 镜头/相机视角而不是自由视角，并开启 `D3D11 bridge candidate diagnostics` 生成支持日志。

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
- 如果 stable path 不稳定，换用 compatibility bridge。
- 需要排查时再开启 `D3D11 bridge candidate diagnostics`，正常使用请关闭。

## 性能口径

正常截图和录制时应关闭 candidate diagnostics。它会额外采样候选深度纹理，只用于支持日志。

在同一 KK 环境中，compatibility bridge 相比 stable path 通常有 10% 以内的性能损失。这个损失是为了换取更稳定的多次截图和 VideoExport 深度输出。实际耗时还会受到场景复杂度、贴图尺寸、ScreenshotManager 超采样、MSAA、PNG 编码和输出分辨率影响。

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
