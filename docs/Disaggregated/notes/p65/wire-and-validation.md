# P6.5 wire、计量与主机验证记录

基线是 `origin/feat/disaggregated@2ec8e7f4`，实现树是 `410dfa94c3d92241e9ec01ddbba1bb911eb60d4b` 加本轮未提交修改。没有推送。以下数字注明采集边界，不代表整个 P6.5 已收官；设备、完整 integration 与最终阶段审查另记。

## wire 事实与握手

`gen_pipe.py` 从 `PipeFields.def` 生成 82 个 payload/value struct 的成员名、`offsetof`、成员 `sizeof`，以及 81 个 opcode 的有序目录（payload、flags、WaitClass）。摘要还覆盖记录头、handle、render-state chunk 边界、caps codec 版本、caps 尺寸、协议版本、字节序与指针宽度。git stamp 与函数指针表尺寸不再混入 wire 指纹。`DynamicBackendParameters` 的三处 `SizeT` 改为 `Uint64`，两处 `GLenum` 改为 `Uint32`；本波 payload/blob 数值区没有裸 `char` 成员。

Hello/Welcome 新字段采用 append；保留旧 FlatBuffers 字段槽。`wireFingerprint` 必须一致；Fork 必须同 build，Connect 的不同 build 默认警告，`MOBILEGL_IPC_REQUIRE_SAME_BUILD=1` 时拒绝。握手不兼容经 `Refuse` 返回，不调用 abort。LinkTerms 的四个窗口由 server 陈述。SurfaceReply 的 `eventHead` fence 使控制回复不能越过尚未送达的数据面事件。

使用 WSL clang 与 NDK 27.3 arm64 的**实际 CapsCodec 编译参数**分别编译 constexpr probe，结果如下；不是凭 LP64 推测一致。

| 事实 | x86_64 Linux | arm64 Android |
|---|---:|---:|
| 成员布局摘要 | 4304347197452401705 | 4304347197452401705 |
| 有序目录摘要 | 16257371160882448404 | 16257371160882448404 |
| render-state 边界摘要 | 2319517018389825580 | 2319517018389825580 |
| DynamicBackendParameters | 352 B | 352 B |
| MGPCaps | 408 B | 408 B |
| 指针宽度 | 64 bit | 64 bit |
| format 维度 | 76 | 76 |
| target 维度 | 12 | 12 |

证据：WSL `/tmp/p65-crossabi-80fz3t4f/{probe.cpp,x86_64.s,arm64.s}`。

### wf 控制

- 真正复制到临时树后，互换 `MGPRange::Offset`/`Size` 两个同宽 `Uint64` 成员；成员布局摘要由 `4304347197452401705` 变为 `6038841773737773369`，两次使用同一未改动 build header。证据 `/tmp/p65-wire-control-ehave5w9/{before.s,after.s}`。生产树没有被临时改坏。
- `SessionHandshakeTest` 的生产 Accept 路径测试了 major=99、错误 wire 摘要的具名 Refuse、空 union 拒绝、Connect 异 build 成功、Fork 异 build 拒绝、Connect 强制同 build 拒绝；9/9 通过。
- 真实 TCP supervisor 的开发期控制 `/home/swung/p65-supervisor-smoke/protocol-controls.json` 记录 major=99 → code 1、错误布局 → code 2、不同 build 的 Connect → Welcome、强制同 build → code 3，并验证拒绝后 supervisor 仍活着。该记录采于 rebase 前的 `2bd86664` 开发产物；不能据此冒称最新产物的所有进程退出条件均已重新取证。最终 control 复跑由阶段主报告记载。

## G1 与生成器

在同一 `/home/swung/p65-g1-src` 源路径、同一 `/home/swung/p65-g1-build` build 路径先编基线再覆盖 P65 源码，保持 clang/Release、pull、LTO OFF 等选项相同。第三方依赖复用同一只读来源；覆盖后更新时间戳，实际重新编译了受影响对象。

- 定义符号：**27837 → 27837，新增/删除/resize/rename = 0/0/0/0**。
- `.text`：**10822147 → 10822147 B**，抽出后逐字节相同。
- 两侧 `.text` SHA-256：`35115edd2c265162c7f845a87d1a2cb9849609a40ba0febac317b91370b9163c`。
- `.data`、`.bss`、`.rodata` 也各自尺寸不变。

证据：`/home/swung/p65-g1-report.log`、`p65-g1-symbols.json`、`p65-g1-before.so`、`p65-g1-{before,after}.text`。这次 G1 包含 wf 的宿主类型改动；此后的 server/stream/计量收尾均在 disaggregated-only 源中。

`gen_pipe.py --check/--self-test`、`gen_pipe_field_ownership.py --check/--self-test` 全绿；flatc 在临时目录重生的 body 与提交文件逐字相同；ROADMAP/ARCHITECTURE/CURRENT_STAGE_PROGRESS 的 78 条文档引用检查零问题。

## unit 与唯一环境项

最终全 unit 跑了 **2389 项**（`/home/swung/p65-unit-final.log`）：第一次为 **2378 PASS、10 既有条件 SKIP、1 环境 FAIL**。唯一红是 `PipeCatalogue.FrontendNeverTakesAnApplierAddress` 从离树 build 的 cwd 向上查源码，错误落到 `/MobileGL/MG_Impl`；不是发现了前端持有 applier 地址。

已仅为该测试设置 source-root `WORKING_DIRECTORY`，保留测试名和其他测试的 cwd。重配后只重跑这一项，CTest **1/1 PASS、退出 0**（`/home/swung/p65-unit-cwd-fixed.log`）。因此本轮完整计数为 **2379 个已验证通过用例 + 10 个既有条件 skip**，没有把全 2389 又跑一遍。

249 项定向 wire/session/codec/loop/remote/protocol/surface/socket/framing 测试也全绿。最初正则漏了 fixture 名 `StreamPair.*`，不能把这 249 项称为覆盖所有 StreamLink 用例。补跑 8 项时抓到真实接收顺序缺陷：`AdvanceRetired` 先按旧 applied 夹紧，最后 retired 永久丢失。修为先 applied 后 retired 后，8/8 通过（`/home/swung/p65-streampair-fixed.log`），随后纳入上述全 unit。详情见 [数据面实现记录](data-link-implementation.md)。

## OpenRA 真实计量

首条 loopback TCP OpenRA retrace：SSIM **1.0**、零差异像素、外部运行耗时 **2.118 s**。日志在 `/home/swung/p65-retrace-loopback-smoke/OpenRA/output/mobilegl.{client,server}.log`。这次 smoke 在上述退休顺序修复前采集，只作为当次图像与计量记录；修复后的功能验收另记。

计量内部检查：client/server 都恰有 frame 1..29；client 每帧固定 32 桶之和 = `rtt_samples` = `wait_replies`，全跑 reply **221 次**；server 首帧只建基线（valid=0），随后 28 帧有效。两侧有效记录均满足 `0 < thread CPU <= wall`。计时仅在 `MOBILEGL_PIPE_STATS=1` 武装；reply 从 publish 前计至 applied wait 完成，包含 Flush，不包含先前编码/command 空间等待；RTT 分位数明确是桶上界。

OpenRA 的第 5 帧含大批纹理上传，不能仅丢前三帧就把均值叫作平稳性能。保留全跑数据，同时明确取 **frame 6..29** 的 24 帧：

| 指标 | 本次 TCP loopback 单臂 |
|---|---:|
| reply / 帧 | 2.000 |
| RTT 均值 | 288.066 μs |
| RTT p50 / p99 桶上界 | 512 / 1024 μs |
| Stage / 帧 | 130614 B |
| 帧流量速率 | 10.328 MiB/s |
| client 线程 CPU / 帧 | 3.0800 ms |
| apply 线程 CPU / 帧 | 5.5496 ms |
| fps | 82.917 |

全跑 Stage 合计 **233980258 B**；P6 文档的历史 OpenRA 合计为 233979394 B，相差 864 B，不能声称历史两跑逐字相同。当前 snapshot 提前退出、不走 Destroy，所以 client 没有 PipeStats JSON/最终摘要；server 的 `seg=0` 是生产端计数属于 client 的结果，不可用作交叉校验真值。当前两个采样点在同一个 StageAllocate 成功路径、同用 `size`，彼此不相加；这批日志没有提供同跑上游 `TotalBytes(StageSegmentBytes)` 的独立数值证据。

聚合结果：`/home/swung/p65-openra-metrics.json`（warmup 3，仍含尖峰）与 `p65-openra-steady-metrics.json`（warmup 5）。解析器为 `scripts/ci/p65_link_metrics.py`，`--server-arm LABEL:LOG` 合并 apply 线程 CPU；尚不是 spawn/TCP 配对 A/B 结论。真 TCP 64 MiB burst 另测 **917.397 MiB/s**，不可把帧流量速率充作链路峰值，也不可将 loopback 数字代表 Wi-Fi/adb 设备。

## 具名前置债：verify + split 阻止 LF 第四例

`PipeVerifyArmingScenario.Armed` 的 TCP verify 臂在测试 draw 前就遇到：

```text
Fatal{PipeRespecifyScope} resource_respecify {slot=12, gen=0}:
scope (target=258, level=0), and no path in this phase may set one
```

使用同一 fresh `/home/swung/p65-verify` 库、server 和 itest，控制臂也得到同因首阻塞：

| transport | 结果与证据 |
|---|---|
| TCP stream | 既有 scope pin；`/home/swung/p65-lf-verify/verify/forward-on/peer.server.log` |
| inproc | 同一 slot/target/level 的 scope pin；`/home/swung/p65-verify-controls/inproc/test.server.log` |
| spawn | 同一 slot/target/level 的 scope pin；`/home/swung/p65-verify-controls/spawn/test.server.log` |

不是本波 wire 布局差异，也不是不同库混配。`PipeApply.cpp` 的 `PinWholeResourceRespecifyScope` 在 `MOBILEGL_PIPE_VERIFY` 下仍禁止任何 per-level scope，来自 `38f34c13`（2026-09-11）；`WireTables.cpp` 在合法 per-level respecify 上调用 `MGPipeSetRespecifiedLevel` 的 producer 来自 `02f15bb6`（2026-09-16），两处均已存在于 P6 基线。旧 pin 未随 producer 落地退役；本任务没有顺手改这条既有生产语义。

此外 `MGPipe: verify armed` 是 **client PipeFill 的 marker**，即使 verify+split 前置债关闭，关闭 server 日志前送也不应让它消失；它不能成为 server forwarding 的反控。第四例目前是**具名前置阻断，未执行到 arming 断言**，不是 LF 转发失败，也没有被计成通过。其他三个 server-owned marker 的 LF 正负控另由阶段主报告记载。


## 真机发现后的补充：program archive 的宿主尺寸误作 wire 事实

八项 POD 布局相同并不意味着所有 blob codec 都跨工具链兼容。WSL client → Android server 经 adb-forward TCP 已完成 EGL，但首次 draw 在 `CreateShaderState.Reflection` 拒绝 archive（`/home/swung/p65-device-usb-probe/mobilegl.server.log`）。实际原因是旧 ProgramArtifactsCodec v1 的第二个头字段：Linux 写 `MGL_LINKARTIFACTS_SIZE=1056`，Android 未 pin 的 libc++ 分支期待 0，尚未读取任何逐字段 payload 就拒绝。

使用两端实际编译参数导出常量，结果为：

| 宿主事实（不是 wire 格式） | Linux libstdc++ | Android libc++ |
|---|---:|---:|
| sizeof(LinkArtifacts) | 1056 | 1000 |
| sizeof(String) | 32 | 24 |
| sizeof(ResourceReflection) | 128 | 120 |
| 旧 v1 size echo | 1056 | 0 |

证据 `/tmp/p65-archive-portable-0bzpmyxr/{linux,android}-sizes.s`。不能通过简单删除该检查解决：字段顺序与 scalar representation 仍需断言。

修复在 disaggregated 构建启用 archive v2：第二个头字段改为从实际 `VisitFields` 顺序、字段名、容器类别/元素 schema、固定数组长度、scalar 宽度/符号性/浮点格式递归得到的 wire-schema 摘要。字符串只描述计数和内容字节；没有任何 `sizeof(string/vector/map)` 或宿主偏移混入。默认空 record 仅供取字段类型，容器递归 `value_type`，不遍历空元素；摘要每进程只计算一次。glslang UniformInitializer 的八字段表由 writer/reader/schema 共用。原 payload 的逐 scalar 序列化算法保持不变，旧 `LinkArtifacts::program` 仍不运输。

archive codec 版本和该 schema 摘要同时纳入 Hello 的 WireFingerprint，使真正不兼容的 archive 在握手先拒绝。新增负控要求 v2 拒绝旧 native echo 1056、0 和旧 v1；既有测试名不删除。非 disaggregated 分支保留 v1 头和旧读写语句；现有 G1 pull graph 不编译 ProgramArtifactsCodec.cpp。

此补充写入时，Linux/NDK 的 codec 与 CapsCodec、Linux 的两份相关测试已按实际 compile_commands 做独立语法检查，六项全部通过；没有另起 ninja。双端重编后的 codec 单测与真实设备 draw 结果由最终阶段报告更新，不把语法检查写成设备验收。


### portable codec 的窄审与后续实证

按 serializer/deserializer 的分支逐项核对 schema：scalar（类型类别、宽度、enum underlying、浮点格式）、string、map、set、固定数组、vector、UniformInitializer、VisitFields record 均有对应描述；容器的元素类型即使运行时为空也会递归描述。五张实际字段表分别为 TypeFacts 20、ResourceReflection 14、XfbVarying 11、LinkArtifacts 57、SpirvArtifacts 8，名字无重复且与成员一致；唯一跳过的 `LinkArtifacts::program` 仍是既有明确豁免。UniformInitializer 的 8 字段由三个方向共用同一表。该检查针对**实际序列化字段的覆盖**，没有把宿主成员偏移重新引入 wire；未发现漏掉现有 serialized field 的分支。

root 统一重编后，`/home/swung/p65-final2-targeted.log` 的 54 项定向测试全部通过，包括 fully-populated archive、截断/版本拒绝、新 portable header 负控、两项 archive fingerprint 输入的敏感性检查、已有握手正负控。两端 v2 的 Debug 制品已在真实 LAN 上完成 GL shader 与 64 MiB 上传；这是功能通过，不能充作 Release 性能或完整设备车道收官。

最后一次源码冻结（含 glFlush、Present、stage cancellation、portable codec）后又用原路径更新 G1 snapshot，只重编 after、不重建 before。最新结果仍是 **27837→27837，0/0/0/0，`.text` 10822147 B 逐字相同**，SHA-256 与前表一致。证据为 `/home/swung/p65-g1-latest-{build,report}.log`、`p65-g1-latest-symbols.json`、`p65-g1-latest.text`。没有降低 G1 判据，也没有将只看尺寸当作字节恒等。


## 真实 ServerLoop idle publication 的 red-once

此控制没有删除测试里的手动 flush。仅复制生产 `ServerLoop.cpp` 到临时目录，删除 `ApplyThreadMain` 真实 idle 分支的 `session.FlushDataProgress()`；保留 `SessionConsumer::WaitForWork` 的 flush、PostReply 的 progress、64 条/1 ms 批量发布及所有其他代码。使用当前 `compile_commands.json` 只编该 object，再从 `ninja -t commands` 的生产 link 命令只替换该 object 与输出路径，生成临时 `libMobileGL.so`。原 client 不变，复制的 server wrapper 通过子进程 `LD_LIBRARY_PATH` 加载临时库。没有在共享 build 目录执行 ninja build。

具体用例：`ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels`，真实 TCP 控制与数据连接，三个独立 loopback 端口对；没有使用设备的 40613。透明 TCP proxy 原样转发并记录 MGLD 帧，观测提交序号、Reply 序号与 Progress 水位，不修改 codec 或 payload。

| 臂 | 结果 | 实际耗时 | 最后 submitted / Reply / applied |
|---|---|---:|---|
| 原库（先） | 1 PASS / 0 SKIP | 0.665425 s | 57 / 56 / 57 |
| 仅删除生产 idle flush | 8 s 短截止抓到挂住 | 8.000486 s | **1 / 1 / 0** |
| 恢复原库（后） | 1 PASS / 0 SKIP | 0.215740 s | 57 / 56 / 57 |

阴性明确卡在该用例初始化阶段的 **seq 1、opcode 2 `ResourceCreate`**（PipeCalls 的 `kWaitReply` 行，96 B record），不是冒称已进入 ReadPixels。server 已发送 `Reply{seq=1}`，但最后到达 client 的 applied 仍是 0；没有后续命令替它推进水位。8 s 截止时 client、supervisor、session child 仍活着，无 EOF、无 proxy 错误，`mgl-srv-apply` 位于 `futex_do_wait`。这证明健康连接下最后一个真实 reply record 需要生产 idle publication；测试只声明超过短截止，不把观察时间说成无限等待。

三臂都有真实 `control=tcp data=stream ... dial=connect` marker。主 `libMobileGL.so` 与生产 `ServerLoop.cpp` 的 SHA-256 在试验前后相同；APK 与主设备端口未动。证据目录 `/tmp/p65-idle-redonce-c_n59aqs/` 包含原始/变异单 TU、object、临时 library、link argv、`run-control.py`、`results.json`、各臂 `data-frames.json`/角色日志/XML 以及主产物 hash manifest；控制台记录 `/home/swung/p65-idle-redonce-run.log`。
