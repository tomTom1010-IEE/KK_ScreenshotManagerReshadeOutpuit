# KK 深度稳定性测试版

分支：`test/kk-depth-pressure`。Screencap 版本：`21.1.3.1`。
Native bridge 标识：`kk-depth-pressure-v1`，API 版本：`2`。

这是从 KK Compatibility 分支建立的独立测试版，用于排查复杂场景及内存压力下的深度缺失。
本包仅用于 KK x64 / D3D11。原发布分支保持不变，KKS 使用原来的发布包。

## 手动安装

1. 完全退出游戏，备份现有的 `Screencap.dll` 和 `OfflineDepthD3D11Bridge.dll`。
2. 将测试包内的两个 DLL 一起放到游戏目录下的
   `BepInEx\plugins\KK_BepisPlugins\`，替换同名文件。
3. 如果配置中填写过 `D3D11 bridge DLL path`，将其清空以使用插件同目录的 DLL，
   或确认该路径指向本次测试版。不要混用旧版和测试版的两个 DLL。
4. 启动后检查游戏日志出现 `KK depth-pressure test v1 (API 2)`，
   native 日志出现 `initialize build=kk-depth-pressure-v1 api=2`。

本次开发未自动安装或替换任何游戏目录中的插件。

## 修改内容

- 用渲染线程事件开始及结束捕获窗口；主线程提前返回不会关闭 DSV 记录。
- 每次截图独立 capture ID、输出路径和读回状态，避免后续帧覆盖在途任务的信息。
- 保存并恢复桥接绘制修改的 D3D11 状态，并明确设置 blend、depth、rasterizer、
  viewport、输入布局、shader stage 和 predication。
- ClearDepthStencilView 不再改变记录中的 OM 绑定；不同 context 分别统计 Draw。
- 候选必须匹配本次渲染尺寸、非 MSAA、可读取，并有本次 Draw 或深度 clear 记录。
  小尺寸内容验证及完整读回均检查有效值及非清空深度。
- 创建纹理、采样或 Map 失败时记录具体 HRESULT；不再回退到未验证的静态评分结果。
- 缓存转换 shader 和绘制状态，复用 RFloat 转换目标及 staging/query 槽。
- 使用 GPU event query 判断拷贝完成，再进行非阻塞 Map；忙碌时保留任务。
- 队列达到预算时等待旧任务回收；内存压力下限制为单槽。最终 flush 保存末尾任务，
  并在压力状态下释放空闲的大尺寸缓存。
- 写入临时文件并检查完整内容、写入量后提交最终文件，失败不生成假深度。

## 测试设置

在 Screenshot Manager 的 `Offline ReShade Export` 中：

| 设置 | 建议 |
| --- | --- |
| D3D11 bridge memory pressure log | 开启，默认开启 |
| D3D11 bridge readback budget MiB | 先保持默认 512 |
| D3D11 bridge candidate diagnostics | 先关闭，复现失败后再按需开启 |
| Log export timings | 测试性能时开启 |
| KK environment fingerprint log | 第一组可开启记录环境；测性能时关闭 |

读回预算约束桥接层转换目标和 staging 池的像素数据量，最多四个在途槽。
它不是整个游戏的内存/显存上限，驱动分配也可能含额外开销。
以 `7680×4320` 为例，一张 RFloat 约 126.6 MiB，默认预算可容纳一个转换目标及三个 staging 槽。
压力判据包括可用 RAM 或系统提交余量低于 512 MiB，或进程本地显存使用量超过预算的 90%。
压力状态只缩减并发数量，不降低深度分辨率、精度或更改采样值。

## 建议测试顺序

1. 使用此前容易失败的同一场景、同一镜头、同一 MSAA 和截图设置，连续 Ctrl+F10 至少 20 次。
2. 同时查看固定输出及 cap 中的留档；每次应有对应颜色、深度和 metadata。
3. 用配套 VE 的 `Normal + Depth` 录制 50–100 帧，检查颜色与深度数量，抽查首帧、中间帧、
   末帧的内容及对齐关系。现有 VE 的反射接口保持兼容，无须为本次测试重新安装 VE。
4. 再在更复杂的场景或用户自然遇到的内存紧张环境中重复。不需要刻意耗尽系统内存。
5. 比较旧版和测试版性能时，保持场景、分辨率、超采样、MSAA、日志设置及存储位置一致。

默认输出格式保持原样：`depthoutput.rfloat` / `N.depth.rfloat`，小端 IEEE 754 float32 device depth。
KK 深度尺寸仍为 `depthWidth×depthHeight`，包含超采样倍率，不会自动降低到颜色最终输出尺寸。
metadata 字段和原客户端读取方式保持兼容。

## 日志检查

默认 native 日志：`UserData\cap\OfflineReShade\d3d11_depth_probe.log`。

- `capture begin/end id=...`：开始和结束必须对应相同编号，并由渲染线程执行。
- `readback queued ... captureId=...` → `rfloat saved ... captureId=...`：可延迟完成，但编号和路径必须一致。
- `memory ...`：可用 RAM、提交余量、进程私有内存、本地/共享显存预算及使用量、池大小、在途字节和纹理累计分配次数。
- `readback backpressure ... waitMs=...`：等待旧帧释放槽，不代表丢帧。
- `capture failed ... reason=...`：无有效候选、仅 MSAA、分辨率不符、全清空、分配失败、Map/query 错误等。
- `Depth readback flush finished ... remaining=0`：录制结束后不应遗留待保存帧。

`textureAllocations` 统计桥接层成功建立的目标/staging 纹理；固定尺寸持续录制时应在热身后趋于稳定。
分辨率变化、内存压力后释放缓存、失败后的重建可使它再次增加。
DXGI 预算接口不受环境支持时显示 `gpuValid=0`，RAM 信息仍可使用。

发生失败时，保留 native 日志、游戏日志及相同编号的颜色/metadata，注明失败来自 Ctrl+F10 还是 VE。
同一日志可能包含旧版本记录，应从最近的 `initialize build=kk-depth-pressure-v1` 开始分析。

## 验证范围

本地已使用真实 D3D11 WARP 设备验证逐像素 float 深度、行方向、状态恢复、连续帧配对、
资源复用、模拟压力下的回收、分配失败拒绝路径、MSAA/错误尺寸拒绝以及最终 flush。
这些测试没有直接运行 KK；复杂游戏场景下的实际稳定性和性能仍需以上对照测试确认。

## 开发者验证

```powershell
cmake -S Native\OfflineDepthD3D11Bridge -B Native\OfflineDepthD3D11Bridge\build-pressure-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DORS_BUILD_TESTS=ON
cmake --build Native\OfflineDepthD3D11Bridge\build-pressure-test
ctest --test-dir Native\OfflineDepthD3D11Bridge\build-pressure-test --output-on-failure
dotnet build src\KK_Screencap\KK_Screencap.csproj -c Release -p:Version=21.1.3.1 -p:OutputPath=../../bin/KKDepthPressureTest/BepInEx/plugins/KK_BepisPlugins/
```

测试使用系统临时目录保存日志和微型深度样本，不访问游戏安装目录。
