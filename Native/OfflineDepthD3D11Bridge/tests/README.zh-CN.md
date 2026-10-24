# KK 本帧深度写入证据测试版 v3

> 本文保留 v1-v3 测试版记录。正式 hotfix 21.1.3.4 的安装与设置见[中文使用说明](../../../README.zh-CN.md)。正式版阶段诊断默认关闭，日志标识为 `kk-depth-hotfix-21.1.3.4`，不再使用下文的测试版标识。

分支：`test/kk-depth-pressure`。Screencap 版本：`21.1.3.3`。
Native bridge 标识：`kk-depth-write-evidence-v3`，API 版本：`4`。

这是从 KK Compatibility 分支建立的独立测试版，用于排查复杂场景及内存压力下的深度缺失。
本包仅用于 KK x64 / D3D11。原发布分支保持不变，KKS 使用原来的发布包。

v3 修复本次写入证据与 hook 覆盖，保留 v2 阶段诊断；不改变颜色/深度输出格式，不用诊断样本补齐缺失深度。
本次没有更改 VE；VE 失败后重复捕获和计数的问题仍保留，分析时以每次 capture ID 为准。

## 手动安装

1. 完全退出游戏，备份现有的 `Screencap.dll` 和 `OfflineDepthD3D11Bridge.dll`。
2. 将测试包内的两个 DLL 一起放到游戏目录下的
   `BepInEx\plugins\KK_BepisPlugins\`，替换同名文件。
3. 如果配置中填写过 `D3D11 bridge DLL path`，将其清空以使用插件同目录的 DLL，
   或确认该路径指向本次测试版。不要混用旧版和测试版的两个 DLL。
4. 启动后检查游戏日志出现 `KK depth write evidence test v3 (API 4)`，
   native 日志出现 `initialize build=kk-depth-write-evidence-v3 api=4`。

本次开发未自动安装或替换任何游戏目录中的插件。

## 修改内容

### v3 本帧写入证据

- 不再仅凭 Draw/Clear 的 CPU 计数判断新鲜度。候选必须有本次捕获的完整 clear、
  开启深度写入期间实际通过 GPU 深度/模板测试的样本，或可信本帧资源的完整复制证据。
- GPU 证据按实际 DSV 绑定和深度写入状态分段，通过 occlusion query 验证；
  诊断自身的绘制会暂停这些查询，不给自己制造“游戏绘制过”的证据。
- 增加 `OMSetDepthStencilState`、CopyResource/CopySubresourceRegion、ResolveSubresource、
  ClearState 与 ExecuteCommandList 跟踪。未经验证的 depth resolve 不作为可信输出来源。
- 为每个资源保存本次捕获内的版本链：复制引用提交时的源版本，后续源纹理写入不能反向证明
  先前的旧图复制是新鲜的。局部复制也不能替未更新的历史像素取得完整新鲜度资格。
- 使用静态链接的 MinHook 1.3.4 对已发现的 D3D11 实现入口建立持久拦截，
  补充易被运行时优化改写的 vtable；每个入口保存独立原始 trampoline，避免跨 context 错调或重复计数。
  不适合方法专属拦截的共享 no-op 入口保留接口层观察，无法确认的内容仍拒绝输出。
- 原函数按实际 vtable 缓存；被修改的 context/device 保持有效引用到卸载 hook，避免释放后访问 vtable。
- deferred context 的录制不等于本次执行。当前未验证的 command list 执行会使已有证据失效，
  日志记录 `unverifiedCommandLists`；此情况不会静默接受旧深度。
- GPU 查询缓存最多 512 个，资源版本记录有上限，查询/分配失败保留错误原因及 HRESULT。
  这一版新增 hook 和查询开销，必须用实际游戏同场景测试，不能承诺与旧版本耗时相同。

依据：[D3D11 query 定义](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)、
[immediate/deferred rendering](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render)、
[MinHook](https://github.com/TsudaKageyu/minhook)。MinHook 已编入 bridge，无须额外安装 DLL；许可证随包附带。

### v2 新增诊断

- 新选项 `D3D11 bridge capture stage diagnostics`，本测试版默认开启，与旧的 candidate diagnostics 独立。
- 每次显式 `Camera.Render()` 前后提交带 capture ID、相机序号及路径的不可变渲染线程标记。
- 每台相机结束、同尺寸可读 DSV 清除之前、整个相机栈结束时，保存小尺寸 GPU 采样。
  典型采样尺寸为 128×72；最多 64 份，缓存可复用，并为正式输出预留原有读回预算。
- 正式候选选择失败时，额外采样被 `draws=0 && clears=0` 排除的同尺寸可读 DSV。
  即使样本非零，也不会让该候选获得输出资格。
- 清除 hook 内只提交采样和拷贝，不 Map、不等待 GPU。捕获结束后统一读取诊断样本，
  所有诊断读回共用 2 秒等待期限，失败只记日志，不将诊断样本写入 depth 文件。
- 诊断自身的 Draw/OM 调用不计入游戏的绘制/绑定统计，且保存、恢复所修改的管线状态。
- 这些采样会增加命令及日志开销，也可能影响驱动的状态优化，不能用本轮结果作为正式性能基准。
  如需对照，关闭新选项后重复同样的测试。

### v1 保留内容

- 用渲染线程事件开始及结束捕获窗口；主线程提前返回不会关闭 DSV 记录。
- 每次截图独立 capture ID、输出路径和读回状态，避免后续帧覆盖在途任务的信息。
- 保存并恢复桥接绘制修改的 D3D11 状态，并明确设置 blend、depth、rasterizer、
  viewport、输入布局、shader stage 和 predication。
- ClearDepthStencilView 不再改变记录中的 OM 绑定；不同 context 分别统计 Draw。
- 候选必须匹配本次渲染尺寸、非 MSAA、可读取；v3 使用以上真实写入证据替代旧 CPU 次数门槛。
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
| D3D11 bridge capture stage diagnostics | 开启，本次重点；默认开启 |
| D3D11 bridge candidate diagnostics | 关闭，减少其他诊断干扰 |
| Log export timings | 开启记录，但不将阶段诊断当作性能测试 |
| KK environment fingerprint log | 第一组可开启记录环境；测性能时关闭 |

读回预算约束桥接层转换目标和 staging 池的像素数据量，最多四个在途槽。
它不是整个游戏的内存/显存上限，驱动分配也可能含额外开销。
以 `7680×4320` 为例，一张 RFloat 约 126.6 MiB，默认预算可容纳一个转换目标及三个 staging 槽。
压力判据包括可用 RAM 或系统提交余量低于 512 MiB，或进程本地显存使用量超过预算的 90%。
压力状态只缩减并发数量，不降低深度分辨率、精度或更改采样值。

## 建议测试顺序

1. 先关闭 MSAA，使用此前容易失败的同一场景、同一镜头和截图设置，连续 Ctrl+F10 至少 20 次。
2. 同时查看固定输出及 cap 中的留档；每次应有对应颜色、深度和 metadata。
3. 用配套 VE 的 `Normal + Depth` 录制 50–100 帧，检查颜色与深度数量，抽查首帧、中间帧、
   末帧的内容及对齐关系。现有 VE 的反射接口保持兼容，无须为本次测试重新安装 VE。
4. 再在更复杂的场景或用户自然遇到的内存紧张环境中重复。不需要刻意耗尽系统内存。
5. 比较旧版和测试版性能时，保持场景、分辨率、超采样、MSAA、日志设置及存储位置一致。

### 本次重点：动态验证

1. 先静止镜头连续 Ctrl+F10 20 次，观察先前偶发无输出的问题是否消失。
2. 每次明显改变相机位置或角度后再截图，至少 10 张。变化应足够大，不能只依靠微小呼吸或眨眼。
3. VE 再录制 30–60 帧明显移动镜头或物体的序列，至少抽查首帧、中间、末帧以及此前失败附近帧。
4. 使用每帧 metadata 的 `depthWidth/depthHeight` 读取 `.rfloat`，查看颜色与深度轮廓是否同步变化。
   不仅检查文件数量，也要排除复制上一帧的现象。
5. 保留 native、environment 和游戏日志。成功候选的 `evidenceCaptureId` 应与本次 capture ID 一致；
   `probeHash` 应随明显几何变化而变化，但静止时相同是正常现象。哈希变化本身不是准入依据。
6. 最后关闭阶段诊断重复一组，检查稳定性是否依赖诊断，并单独观察新增 GPU 查询的性能开销。

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
同一日志可能包含旧版本记录，应从最近的 `initialize build=kk-depth-write-evidence-v3` 开始分析。

- `entry_hook installed ... method/variant/address`：已发现并拦截的实现入口，通常热身后不再增长。
- `write_evidence id=... queries=... fullCopies=... unverifiedCommandLists=... error=...`：本次写入验证汇总。
- `candidate ... evidenceCaptureId=... writeReason=... gpuPassedSamples=... probeHash=...`：候选使用的证据。
  `writeReason` 可为 `current_capture_clear`、`gpu_depth_samples` 或 `copy_from_current_capture`。
- `noFreshWrite`：没有可证明的本次写入；旧字段 `noDraw` 保留为该数量的别名，不再仅指 CPU Draw 次数。

### 阶段日志的读法

- `camera_stage ... cameraIndex=... stage=camera_begin/camera_end`：相机阶段边界，附带累计 hook Draw/clear 次数。
- `stage_scan ...`：该阶段观察到的 DSV 数量、同尺寸可读候选数量、实际提交的诊断样本数。
- `stage_sample ... stage=camera_end`：对应相机结束时的内容。
- `stage_sample ... stage=before_clear`：清除前的内容；`nextClear` 是即将写入的清除值。
- `stage_sample ... stage=stack_end`：整个相机栈结束时的内容。
- `stage_sample ... stage=rejected_no_draw`：正式输出拒绝的无本次 Draw/clear 证据候选。
- 样本日志在捕获结束后统一写入，比较顺序应使用 `id + seq + observedTick`，
  同时匹配 `dsv + resource`。`cameraIndex` 表示观察发生在哪台相机阶段，不代表该 DSV 由它独占。
- `freshEvidence` 保留 v2 的 CPU Draw/clear 计数含义；v3 的 `gpuFreshEvidence/gpuPassedSamples`
  对应该样本时点冻结的资源版本。仅非零不能据此直接放宽输出验证。
- 若同一 DSV 在主相机结束时非零，后续清除前非零，而相机栈结束时全零，说明内容在后续阶段丢失。
  若被过滤候选在栈结束时仍非零，则需要进一步区分本次绘制漏记与历史残留。
- 某些驱动的优化入口可能绕过 Clear hook，导致缺少 `before_clear` 行；相机结束时的前后对照
  仍可显示内容何时变空。小尺寸点采样也可能漏掉极小几何，不应单凭一次小样本证明整张图为空。
- `stage_diag skipped ...` 表示达到 64 样本上限、预算不足、查询/Map 失败等诊断限制，
  不代表正式深度一定失败。所有样本标记 `diagnosticOnly=true`。

## 验证范围

本地已使用真实 D3D11 WARP 设备验证逐像素 float 深度、行方向、状态恢复、连续帧配对、
资源复用、模拟压力下的回收、分配失败拒绝路径、MSAA/错误尺寸拒绝以及最终 flush。
这些测试没有直接运行 KK；复杂游戏场景下的实际稳定性和性能仍需以上对照测试确认。

v2 增加阶段前后内容变化、清除 hook 状态隔离、被过滤候选保持拒绝、优化入口绕过 clear、
最多 64 样本、连续帧缓存复用、关闭新诊断时不提交采样等测试。
清除 hook 的状态隔离用例显式调用 hook 入口，以避免 WARP 在修改管线状态时改写其 vtable；
另一个用例刻意绕过 clear hook，验证相机阶段对照仍能发现内容消失。

v3 验证绕过 CPU hook 的 24 帧变化深度、硬件 D3D11 小场景、本帧完整复制、复制后清空源、
旧帧复制、源的未来写入不能追溯生效、局部更新的旧背景、关闭深度写入、GPU 证据失败、
deferred 仅录制未执行、未验证 command list 执行使先前证据失效、状态重置后禁用深度写入，
以及应用自身 occlusion query 的样本数不被本次证据查询改变。
这些是独立 D3D11 测试，不等于已经通过 KK 游戏实际复杂场景或低内存验证。

## 开发者验证

```powershell
cmake -S Native\OfflineDepthD3D11Bridge -B Native\OfflineDepthD3D11Bridge\build-pressure-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DORS_BUILD_TESTS=ON
cmake --build Native\OfflineDepthD3D11Bridge\build-pressure-test
ctest --test-dir Native\OfflineDepthD3D11Bridge\build-pressure-test --output-on-failure
dotnet build src\KK_Screencap\KK_Screencap.csproj -c Release -p:Version=21.1.3.3 -p:OutputPath=../../bin/KKDepthWriteEvidenceTest/BepInEx/plugins/KK_BepisPlugins/
```

测试使用系统临时目录保存日志和微型深度样本，不访问游戏安装目录。
