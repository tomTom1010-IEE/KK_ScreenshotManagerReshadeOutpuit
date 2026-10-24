# KK Depth Hotfix 21.1.3.4

Date: 2026-10-25. KK x64 / D3D11 only. Native bridge API: 4.
Source branch: `hotfix/kk-depth-write-evidence`.

## 中文说明

### 安装

1. 完全退出游戏，备份旧文件。
2. 将本包两个 DLL 一起放入 `BepInEx\plugins\KK_BepisPlugins\`，替换同名文件：
   `Screencap.dll`、`OfflineDepthD3D11Bridge.dll`。
3. `D3D11 bridge DLL path` 推荐留空，避免仍加载其他位置的旧 DLL。
4. 正常使用关闭 `D3D11 bridge candidate diagnostics`、`D3D11 bridge capture stage diagnostics`
   和 `KK environment fingerprint log`。旧配置会保留，默认值更改不会覆盖已有设置。
5. 按 `LeftCtrl + F10`，检查 color、depth、metadata，再移动镜头测试；确认正确后再使用 app 或 VE。

配套 VideoExport 不需要更新。KKS 不安装本包，继续使用
[上一正式版](https://github.com/tomTom1010-IEE/KK_ScreenshotManagerReshadeOutpuit/releases/tag/Release0704_main)
的 KKS 包。旧 Stable/Compatibility 资产和发布分支不改动。

### 修复内容

- 用渲染线程事件界定本次离屏相机栈捕获窗口，保留独立 capture ID、路径和在途读回状态。
- 补强 D3D11 vtable 与实际实现入口的 hook，防止运行时优化改写入口后漏记 Draw/Clear。
- 增加开启深度写入时的 GPU 样本证据，以及按提交时间记录的完整 clear/复制来源链。
  不因纹理非零就接受旧深度；局部旧图更新、未来源写入、未验证 command list 不会取得虚假资格。
- 保留精确尺寸、非 MSAA、可读性、小样本及完整 float 内容验证。
- 保存并恢复桥接绘制修改的状态；复用转换/staging 纹理，限制读回池预算，并记录内存压力与具体失败原因。
- 阶段诊断和环境指纹默认关闭，但本帧写入验证始终保留。
- `.rfloat`、metadata、颜色超采样与配套 VE 接口不变。KKS 代码与发布包不变。

### 验证与限制

测试版同一捕获实现共通过 64 次实际游戏捕获：开启/关闭阶段诊断各 12 次 Ctrl+F10 和 20 帧 VE。
所有完整深度均通过尺寸、数值检查，并在独立 D3D11 设备上重采样后匹配各自 capture 的采样哈希；
移动镜头叠加检查未发现系统性错帧。正式包仅调整版本/日志标识、文案和默认诊断配置，捕获算法保持一致。

开启/关闭阶段诊断的短序列 VE 平均耗时分别约 480/499 ms，native 捕获窗口约 58/34 ms。
两轮镜头路线不同，不能据此承诺整体加速或固定性能损失。纹理分配热身后分别稳定在 18/4 次。
本轮可用 RAM 仍约 27 GiB，未验证真实低内存或所有其他用户设备的可靠性。

仍不支持 MSAA depth 读回。缺深度时关闭 MSAA，尝试 Studio 镜头视角，并在测试时关闭后台优化。
证据不足、全清空或读回失败时仍拒绝输出假深度。MinHook 1.3.4 已静态链接，许可证随包提供。

## English

Install both DLLs together into `BepInEx\plugins\KK_BepisPlugins\` with the game closed.
Do not mix the old Stable/Compatibility bridge with this Screencap build. Leave the bridge
path empty unless it points to the matching DLL. Check existing diagnostic settings after
upgrading; candidate/stage diagnostics and environment fingerprint logging should be off
for normal use. Verify a moving color/depth pair before app processing or VideoExport.

This hotfix addresses intermittent missing depth caused by render-thread capture timing
and incomplete D3D11 write observation. Persistent entry hooks supplement vtable hooks;
GPU samples with depth writes enabled and time-ordered copy provenance establish current
capture evidence. Nonzero texture contents or CPU Draw counts alone do not qualify a
candidate. Unverified command-list execution invalidates prior evidence. Exact-size,
non-MSAA, readable-resource and full-content validation remain in place, together with
pipeline-state restoration and bounded reusable readback resources.

The paired VideoExport interface, `.rfloat` schema and color resolution/supersampling are
unchanged. This release contains no KKS or VideoExport binaries. Their previous releases
remain valid. Testing covered 64 successful in-game captures with stage diagnostics on/off
and separate WARP/hardware D3D11 regression tests. This does not guarantee all plugin,
driver, free-view or genuinely low-memory environments. MSAA depth remains unsupported;
missing evidence must still fail closed instead of exporting historical or empty depth.

For native builds, use an x64 C/C++17 toolchain, CMake and the Windows D3D11 libraries.
MinHook source is pinned and vendored in `Native/OfflineDepthD3D11Bridge/third_party`.

```powershell
cmake -S Native/OfflineDepthD3D11Bridge -B Native/OfflineDepthD3D11Bridge/build-hotfix -G Ninja -DCMAKE_BUILD_TYPE=Release -DORS_BUILD_TESTS=ON
cmake --build Native/OfflineDepthD3D11Bridge/build-hotfix
ctest --test-dir Native/OfflineDepthD3D11Bridge/build-hotfix --output-on-failure
dotnet build src/KK_Screencap/KK_Screencap.csproj -c Release
```

## Checksums

The release ZIP includes `BUILD-INFO.json` with source commit, build verification and
file SHA256 values. The release also supplies a SHA256 checksum asset for the ZIP.
