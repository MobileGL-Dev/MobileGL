# P3a — handle wave 1（Espryt）：buffer、VAO（`fde5fda3`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：九条 `resource_*` + 五条 vertex-input 调用接到句柄形 `MGPipeResourceOps`；第七张 Espryt slot 表；`ResourceRespecify` 的 `kNeedsAck` 谓词；VAO twin 六条身份 memo 在句柄臂退役、四条重键；`mpr` 定义；子系统位 7/8，push 默认 `0x1ff`。
- 门：五部分门全绿，G1 0/0/0/0 `.text +0`；G5 十一函数；单元 1619×3；`integration-gpu` 958 五臂；verify 842；retrace 79/79 ×2。实际 1 天（绊线 27 天）。
- 记录项：rd12（VAO 切换密）上 P3a 把 Release 帧 p50 差距从 +11% 推到 +27–30%；DriverBench 每 draw 多 Espryt 699 / Magma 422 ns。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P3a** handle wave 1（Espryt）：buffer、VAO
- **状态**：✅ `fde5fda3`
- **落地什么 / 范围**：九条 `resource_*` + 五条 vertex-input 调用接线到句柄形 `MGPipeResourceOps`；第七张 Espryt slot 表；`ResourceRespecify` 的 `kNeedsAck` 谓词；VAO twin 六条身份 memo 在句柄臂退役、四条重键；`mpr` 定义；位 7/8，push 默认 `0x1ff`
- **验收门 / 证据**：五部分门全绿：G1 0/0/0/0 `.text +0`；G5 十一函数；单元 1619×3；`integration-gpu` 958 五臂；verify 842；retrace 79/79 ×2。实际 1 天。§4

## 实测：P3a（`fde5fda3`，基线 `5cb826b0`，44 个提交，37 文件 / +9273 / −230）（原 `MEASUREMENTS.md` §4）

### 4.1 五部分门（本地 `~/w7/pipe`）

| 门 | 结果 |
|---|---|
| 构建 pull / push / verify | rc 0 / 0 / 0（fail-fast，绝不拿陈旧二进制下判决） |
| A 门 include 闭包 / C 门 `pGLContext` / G13 | 4 probes 0 problem / 空 / 空 |
| **G1** | `.text` 10806323 → 10806323（+0）；27811 符号 **0 / 0 / 0 / 0** |
| **G5** 十一函数（pool / 延迟释放 / ring / `FlushPendingRangesNow` 与 `FlushPendingRangesFrom`） | 对 `44c2b5cf` 与 `5cb826b0` 都逐字节相同；self-test 两个阴性对照按名变红 |
| `integration-verify` / 79 retrace verify | 842/842 零 `Fatal{` / 79/79 armed 零分歧 |
| G2 / G14 | 差 0 / 0 删除 +58 |
| 单元 | 1619 × 3 |
| `integration-gpu` | 958/958 × {pull, push, `PIPE_PUSH=0`, `0x7f`（G12 子系统关闭臂）, `ESPRYT_DISABLE_INVALIDATE_FLUSH=1`} |
| buffer/VAO 族 / `RenderStateSpans`+`Residual`+`VertexInputEmit`+`ResourceEmit` | 202/202 / 48/48 |
| `HandleRecycle`（verify） / 子系统对照 / verify 对照组 | 60/60 / 10/10 / 64/64 |
| G7 vertex-input 阴性对照 | 变红并点名 `IsBgra` |
| 79 retrace push | 79/79 |
| G3b 具名五条 | 桌面全绿（都在 79 例扫描内），但单独的具名扫描因门脚本把正则写成逗号清单而选中 0 例；设备侧按开放问题 17 排除 `create-indirect` → 记为"桌面全绿、设备侧部分" |
| `gen_pipe` / `gen_pipe_dirty_surface`（扫描根扩到 `MG_State/GLState`） | self-test 7 / 21 个阴性对照全部触发；75 个 mutator 全映射，0 COARSE / 0 UNDECIDED |

### 4.2 "重键前红"与验证轮找到的三个缝隙缺陷

buffer 类的红前证据（package C 尚未注册 `MGPipeResourceOps` 时）：`HandleRecycle` 的 `.AbaControl` 臂通过（它断言的就是被污染的内容）、`.Legacy` 通过、`.Handles` 可见地 skip；C 注册后两条 skip 转为断言，最终 60/60。

| # | 缺陷 | 表现与修法 |
|---|---|---|
| F1 | vertex buffer 条目按属性的 GL 绑定点解析，而不是按属性下标 | 两者只在 `KHR-GL43.vertex_attrib_binding` 题材上分叉；`VertexAttribBindingScenario` 6 → 3 |
| F2 | ensure 路径读 `MGPResourceDesc::HasDefinedContent`（上一次 respecify 时的事实）判断影子有无字节 | `glBufferData(size, NULL)` + `glBufferSubData` 后应用字节被静默丢掉；改为持有前端对象时读 `HasDefinedContent()` |
| F3 | 惰性生成的 twin 从不发布影子基址，fp64 收窄拒绝每个 draw | `hostBytes` 终身 null → Adreno workaround 禁用属性；在算出基址处补发布，只对非采纳资源 |

方法学：包 worktree 里 `tools/trace_replay/fixtures` 是 LFS 指针，对着指针跑 retrace 报 `passed 2 / 79`、`ssim=None`——**假红**；base-instance 对照在 llvmpipe 上要先关掉原生 `GL_EXT_base_instance` 才可证伪。

### 4.3 Track H 单位成本

日历：四个包（contract/wire、client、espryt、gates）各一轮实现 + 对抗性评审 + 至多两轮返工，全部在 2026-09-08 一天内，对 27 天绊线 1/27。memo 账：直接删除普查 11 条里的最后一条（`ConvertedVertexStreamKey::sourcePin`）；句柄臂上退役 VAO twin 五个同步 memo 成员；重键 VAO twin 索引槽 memo、`ResolvedDrawBuffers`、twin 同步门、`ConvertedFloat64Stream`；**保留**（`MOBILEGL_PIPE_LEGACY_MEMOS` 下仍编译）：以上全部成员、`g_pendingFetchBaseInstance` 族、D-K 那组——真删会移走 pull 符号或改 `sizeof`，G1 的 0/0/0/0 就是这条的度量，随 P13 退役。

### 4.4 两机 Release 配对 A/B、MC 26.3 的 p99、DriverBench（记录项）

协议：reboot-clean、`pin_device.sh` 钉频并前后 `check`、两臂背靠背、`--benchmark-repeats 3 --benchmark-tail-frames 200`、best-of-3、`--benchmark-no-finish` 主臂；APK 都是 `3e298c9a` 的 Release trace 构建（`.text` 9.4 MB）。臂：pull = pull 库；P2 = push APK `MOBILEGL_PIPE_PUSH=0x7f`；P3a = push APK 默认 `0x1ff`。`create-indirect` 排除（开放问题 17）。单位 ms/帧。

| 设备 | trace | 后端 | pull p50 | P2 p50 | P3a p50 | pull p99 | P3a p99 | 备注 |
|---|---|---|---|---|---|---|---|---|
| 小米 | `improved-transparency-minecraft-26.3` | Espryt | 10.810 | 11.485（+6.2%） | 11.697（**+8.2%**） | 25.457 | 26.322 | |
| 小米 | 同上 | Magma | 10.675 | 11.473（+7.5%） | 11.459（**+7.3%**） | 25.014 | 26.035 | |
| 小米 | `minecraft-1.21.4-rd12-odinlite-in-world` | Espryt | 8.220 | 9.117（+10.9%） | 10.650（**+29.6%**） | 21.895 | 24.487 | |
| 小米 | 同上 | Magma | — | — | — | — | — | 三臂都 SIGABRT（`scudo::reportMapError`，`dev` 侧） |
| 小米 | `minecraft-1.21.4-fabric-sodium-in-world` | Espryt | 1.292 | 1.349（+4.4%） | 1.382（**+7.0%**） | 2.418 | 2.443 | |
| 小米 | 同上 | Magma | 0.485 | 0.504 | 1.010 | 1.469 | 2.187 | 亚毫秒帧，噪声 |
| 小米 | `create-instancing` | Espryt / Magma | 944.4 / 1003.9 | 945.2 / 1019.5 | 949.9 / 1015.6 | — | — | 两帧 fixture |
| Oppo | `improved-transparency-minecraft-26.3` | Espryt | 9.741 | 10.570（+8.5%） | 10.542（**+8.2%**） | 26.312 | 27.022 | |
| Oppo | 同上 | Magma | 8.162 | 8.920（+9.3%） | 8.973（**+9.9%**） | 22.851 | 23.638 | |
| Oppo | `minecraft-1.21.4-rd12-odinlite-in-world` | Espryt | 9.938 | 11.009（+10.8%） | 12.963（**+30.4%**） | 27.137 | 29.765 | |
| Oppo | 同上 | Magma | 8.001 | 8.933（+11.6%） | 10.161（**+27.0%**） | 24.723 | 27.355 | |
| Oppo | `minecraft-1.21.4-fabric-sodium-in-world` | Espryt / Magma | 1.896 / 0.399 | 1.770 / 0.431 | 1.841 / 0.437 | 2.993 / 1.206 | 3.079 / 1.409 | 噪声内 |
| Oppo | `create-instancing` | Espryt / Magma | 1075.1 / 758.3 | 1045.1 / 731.0 | 1060.1 / 760.0 | — | — | 两帧 fixture |

读法：(1) **P2 边界在 Release 下的真实代价是 +6–12%**，两机两后端一致；(2) P3a 在 26.3 与 sodium 上几乎不再加价，**但在 rd12 上把差距从 +11% 推到 +27–30%**——rd12 每帧 VAO/buffer 绑定切换远多于 26.3，每次切换走一遍 `set_vertex_buffers` 构造 + `ContentHash` + applier 记录 + 逐属性走查（P4a 补的 `csob-blob` 计数器给了字节口径：rd12 1.06 MB/帧，其余用例中位数 2.9 KB/帧；DirectVulkan 恒 0）；(3) **MC 26.3 在 Adreno 上的 p99**：25.46 → 26.32 ms（+3.4%），仍在采纳基线的 21–26 ms 档；(4) `mpr` 在设备上成立（26.3 两机都是 8，P2 臂恒 0）；(5) 计数器三臂逐字相同。

小米 rd12 + Magma 三臂（含 pull）都 `SIGABRT`：`scudo::reportMapError` ← `scudo_calloc`，pull 库与 P3a 前的 Magma 路径符号一致——既有 bug，`dev` 侧任务。

DriverBench（`wsl_p3a_bench.sh`，`c20e2f2b`，`mc_vanilla_draw` ns/draw；T2 = `0x7f` 臂）：

| | Espryt | Magma |
|---|---|---|
| **T1** = push(`0x1ff`) − pull | **+1048**（+20.5%） | **+611**（+3.6%） |
| **T2** = push(`0x7f`) − pull | +349 | +189 |
| **T1 − T2 = P3a 自己加的** | **+699** | **+422** |
| 位 63 − push | −6 | +20 |
| blend-toggle / pass switch | +5.2% / ≈ 0 | +2.8% / ≈ 0 |

读法：P3a 的 buffer/VAO 句柄路径每 draw 多花 Espryt 699 / Magma 422 ns（Magma 没有句柄化的 VAO 消费者，全是 client 侧发射 + applier 记录）；进优化清单：vertex-input 发射的 per-draw 抑制与 applier 记录就地更新。`DriverBench` 必须经 `DRIVERBENCH_EGL_LIB` 显式 `dlopen` provider，不得靠 `LD_LIBRARY_PATH`（glvnd 会把 GPU 悄悄换成 llvmpipe）。

### 4.5 决定与遗留

- **ID-15**：`Managers.cpp` 的三层 flush 排水梯每个预处理臂各一份——push 构建跑 `FlushPendingRangesFrom`，pull 构建跑逐字节不变的 `FlushPendingRangesNow`；两个名字都是 G5 行，各对自己的 pin 比。转发函数不可行（G5 提取器不认预处理）。
- **M-3**：fp64 顶点数组收窄在句柄臂上少做一处 `SyncGpuWrites`（`SyncFloat64AttributeAsFloat32ByHandle` 手里只有句柄）——默认掩码下两臂唯一的行为差异（fp64 数组 + shader 写过的源 buffer + 无显式回读），P8 把拉取搬到 client 侧即关闭。
- 整体 diff 终审抓出两个跨包接缝：client 为每个 VAO 铸的 `VertexElementsCso` 槽在 Magma 下泄漏（修为 `~VertexArrayObject` 走后端无关的死亡路径）；`FlushPendingRangesFrom` 在 G5 覆盖之外（→ ID-15）。
- CI 独有的 teardown 崩溃（`exit()` 时静态析构顺序 UAF）：`MGPipeSlots()` 等单例与四个持有前端 `SharedPtr` 的静态 `PipeInputs` 改为退出时泄漏（`d54ec57a`、`6515c8e6`、`fde5fda3`），ASan 10/10 干净；复现钥匙 `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`。
- `dev` 侧跟进（拆分不顺手修）：`g_uploadRing` 不被 `OnBackendContextDestroyed` 重置（良性，`RingAvailable` 自愈）；`ScopedDefaultUnpackState::s_synced` 没有失效路径。
- `MG_Test/Buffer/BufferTest.cpp` 的 fixture 只 scope 了 `BufferBackendOps` 不 scope `MGPipeResourceOps`，26 条被路由进 pipe——修后 86/86；给它一个 pipe 形 mock 是跟进项。
- 峰值 RSS（push vs pull，79 例）：`retrace_gate.py` 不报，没有数。

## 里程碑记录（原 `ROADMAP.md`）

- **P3a / P4a 出口（2026-09-08）**：各 1 天，远低于 27 / 39 天的再基线绊线，未触发重定基线。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P3A.md`](BRIEF-P3A.md) | P3a implementation brief — handle wave 1 (Espryt): buffers and VAOs behind the `resource_*` / vertex calls |
| [`INTEGRATOR-DECISIONS.md`](INTEGRATOR-DECISIONS.md) | P3a integrator decisions (feat/disaggregated, 2026-09-08) |
| [`scout-docs-spec.md`](scout-docs-spec.md) | 1. The P3a row of the phase table |
| [`scout-espryt-buffers.md`](scout-espryt-buffers.md) | P3a scout — Espryt (MobileGL/MG_Backend/DirectGLES) buffer surface |
| [`scout-espryt-vao.md`](scout-espryt-vao.md) | P3a scout — Espryt's vertex-array surface |
| [`scout-pipe-and-tests.md`](scout-pipe-and-tests.md) | P3a scout — the MGPipe scaffolding and the gates |
| [`ab/`](ab/) | 3 个文件：两机 Release 配对 A/B |
| [`bench/`](bench/) | 1 个文件：DriverBench |
| [`p3a-results/`](p3a-results/) | 19 个文件：各包的实现、评审与终审稿 |
