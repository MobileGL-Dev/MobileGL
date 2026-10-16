# P5d 报告：inproc 性能专项（2026-09-18）

> 状态：**三轮，均在 `feat/disaggregated`**——一轮 `cb06538c`、二轮 `56a77348`、三轮 `1f8de61b`（本文所在提交的前一个）。
> 本报告回答三个问题：要做什么、做到了什么、什么没完成。数字表在 §"数字"，三轮的设备记录在 `MEASUREMENTS.md` §9。

## 定位与目标

**插入点**：P5c 之后、P6 之前。路线图的既有纪律是"性能对着 pull 臂只记录、不作阻塞门，专门的优化阶段排在路线图推完之后"——但 P5c 收官后在真实负载上的实测把这件事提前了：FCL + Minecraft 26.3-rc-3、视距 12、Redmi `2f7cbe2e`，inproc 世界内只有 **7-13 fps**，monolith 同场景 **~116 fps**，约 9 倍差距，inproc 不可用。

**目标**：在不改变 wire 语义、不动正确性门（双车道、G1/G2/G5/G14、双生成器门）的前提下，把 `inproc` 与 `monolith` 的逐帧帧率差拉到尽可能接近 1。性能数字的采集纪律与其他阶段一致：CPU 定频（大核 1958400 / 小核 1555200）、风扇二档、只用 Redmi `2f7cbe2e`；A/B 切换靠 `/sdcard/FCL/mg_transport.txt` 与 `/sdcard/FCL/mg_env.txt`（`MOBILEGL_*` 逐行）两个运行时开关，免 APK 重编。

## 起点的瓶颈链（全部实测，非推测）

- persistent-map 推送：每个 verb 把每个映射的**全范围**推上线（实测 ~25 MB/帧，每 64 KB 块一条记录一次 barrier 往返）。
- 脏检查：每 64 KB 块做内容哈希，**每个 draw 对全部 arena 全范围扫**——XXH64/XXH3 一度占整个进程 73%（绑定过滤后仍 32.8%）。
- barrier：每条 verb 记录一次 `appliedSeq` 往返。
- `Doorbell::Wait` 的自旋每次迭代读一次时钟（该设备上 vDSO 的 `clock_gettime` 仍占进程 ~20%）。

## 做了什么

### 一轮 `cb06538c`（15 文件，+1100/-25）

- **persistent-map 推送改为三级臂，按 buffer 在 map 时挑选**：
  - **mprotect 脏页追踪**（主臂）：映射范围的页对齐内部注册 `PROT_READ`，应用写入触发 SIGSEGV，handler 在槽位位图上标脏页并放行，推送时只发含脏页的 64 KB 块、按连续段一次 mprotect 重新武装。归属判定三条，任一不满足即原样链给前一个 handler：只认内核 `SEGV_ACCERR`（用户态 raise 与未映射页不算）、必须在存活跟踪区间内、位图位鉴别重放（已置位页上的立即重 fault 只应答一次，再犯即非写访问，诚实崩溃）。对齐**向内收**——外向对齐的 edge 页含 malloc 元数据，信号被屏蔽的驱动/运行时 worker 线程 fault 即进程死亡（ctest 下 SmallRing 必崩、gdb 下必过的根因）。首推全量一次（与哈希臂 "fresh pushes everything once" 同规则）。
  - **哈希抑制臂**（`MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS`，默认 1）：未追踪 buffer 的逐块 XXH3 对比，只推变化块。
  - **绑定过滤推送**（`PushDrawConsumers`）：只推当前 draw/dispatch 实际消费的 buffer（VAO attribs/element、索引绑定点、indirect/param 槽、**TexBuffer 背存**——顺手修了高速飞行云的抽动，用户已确认）。
- **barrier 批处理**（`MOBILEGL_IPC_BATCH_WAITS`，默认 1；`MOBILEGL_PIPE_VERIFY` 强制 0）：无 reply slot 的 kCtxState/kCtxCso/kCtxObject 值类记录发布即返回，barrier 推迟到下一个拉取类 verb——R-1 的围栏柱不动，只减少往返。两个连带问题被钉死而非口头论证：kCtxObject 类记录依赖同步语义的测试加显式围栏（GenerateMipmap、FramebufferDeath 两条）；批处理下才可能出现的"apply 线程成为前端对象最后一个引用"新增**延期销毁队列**（入队 `(kind, lifetimeId)` POD，GL 线程在 verb 钩子重放七个 DestroyAndFree helper）——没有它 CopyImageLayered 两条必炸 `Fatal{RoleViolation, "MGPipeSlots"}`。
- **自旋时钟批处理**：`Doorbell::Wait` 的自旋改为每 64 次 yield 读一次时钟（clock_gettime 家族 ~20% → ~2%）。

### 二轮 `56a77348`（2 文件，+34/-11）

一轮上设备后发现 epoch skip 从未生效：负载里总有子页 persistent map，任何"未追踪"成员都否决跳过，绑定游走照样每 draw 跑（~8%），而直接扫所有未追踪成员的替代方案实测更差（41-43 vs 58-62 fps）。

- **零页注册**：子页范围不再落哈希臂，注册为"空内部 + hasEdges"的追踪槽（不保护任何页），全范围由两个有界的边块哈希服务——不再否决任何跳过。
- **shadow 页对齐分配**（`SHADOW_ALLOCATION_ALIGNMENT = 4096`；`GL_MIN_MAP_BUFFER_ALIGNMENT` 查询仍答 64）：shadow 占有的每一页字节全是自己的，追踪器永远不必碰邻居分配的字节；边块与零页形态在真实负载中基本消失。monolith 共用该分配器，A/B 无影响。

### 三轮（`1f8de61b`，40 文件，+2474/-297）：按符号化 profile 逐项切

二轮收官报告把 apply 线程 ~48% 的 mutex 时间归给后端 `pendingMutex`，那是猜的。对二轮头 `56a77348` 的 simpleperf 数据（cpu-cycles、整进程、世界内 15 s）符号化后，调用图说的是另一回事，三轮的四个包就按它切：

| 线程 | 热点（self %） | 真因 |
|---|---|---|
| apply | `mutex::lock/unlock` + `pthread_mutex_*` 合计 ~49%，`ApplyThreadMain` 直接调用者占 58-60% | **空转轮询**：`ready` lambda 里的 `ControlIsPending()` 与每圈的 `PumpControlRequest()` 各锁一次 `m_controlMutex`；真正的 `DrainRing` 只占该线程 29% |
| apply | `__emutls_get_address` 7.4%（monolith 渲染线程上 7.7%，是**第一热点**） | `MGPipeFrontendKeyedRegistryScope` / `MGPipeReverseAnnouncementScope` 的 `thread_local` 深度计数器，在 DirectGLES 约 20 个站点（含 `HandleOf` 内）每 draw 多次构造 |
| client | `WaitForApplied` 27.9%（含 34.2%） | R-1 lockstep：每个 draw 等 apply |
| client | persistent-map 推送含 22.5%：`XXH3` 8.5%、`PushBlocksForChecked` 5.6%、`MappedData/IsMapped/GetMappedRange` 2.2% | shadow 分配只对齐了基址、没把**尺寸**补到页倍数，几乎每个 buffer 的映射末尾都不在页界上——**每个 draw 对每个有边的存活映射哈希两块边** |
| client | 时钟读取 6.6%（`steady_clock::now` / `__kernel_clock_gettime` / `clock_gettime`） | `Doorbell::Wait` 每次等待至少读两次时钟；~900 次等待/帧 |
| client | `MGPipeValidateForVerb` 含 10.9%；`OnApplyThread()` 2.66% self；`MarkWritableImageBufferTextures` 3.6% | 残余填充每 verb 对 63 个字段算 7 项合取、每 draw 扫 192 个 image unit；角色守卫 `OnApplyThread` = 函数静态守卫 + 三个原子 + `pthread_equal` |

四个包（每包在自己的 worktree 实现、两视角审查、一轮修复；lockstep 可行性另有一份只读研究）：

- **T 传输**：控制邮箱加原子影子位 `m_controlPosted`，空转轮询零锁（邮箱握手、C2 临界区、NOT_INITIALIZED 答复原样）；`Doorbell::Wait` 的自旋改为一次性校准的迭代预算，稳态**不读时钟**（park 阶段才算 deadline）；client/server 的 wait 与 park 计数进 `MGPipe stats:` 行的 `wait[srv= srvpark= cli= clipark=]` 与 wire ledger 的 `cliwait=/clipark=`。
- **D 角色守卫**：`OnApplyThread()` 内联为"一个 relaxed 原子 key 与调用线程的线程指针比较"（aarch64 走 `__builtin_thread_pointer()`，其余回落 `std::this_thread::get_id()`），删掉 `m_applyThreadId` 与函数静态守卫；三个 scope 深度计数器退掉 `thread_local`——它们唯一的读者是 apply 线程的守卫，所以只在 `OnApplyThread()` 为真时计数（单写者论证写在文件里，并有"GL 线程持有 scope 不豁免 apply 线程"的红一次）；守卫链按代价重排。
- **B persistent map**：shadow 分配**尺寸**补到 4096 倍数，`TrackWriteMap` 在 shadow 页粒度可证时**向外**对齐（子页映射 = 一个受保护页），边块与边块哈希在稳态消失；向内臂只作回退。审查抓出一个一轮就存在、被边块哈希掩盖的洞：epoch skip 之前的绑定游走只推消费者，一个 fault 过但本 draw 不消费的映射会被标记为"已消费的 epoch"、之后永不推送——修法是游走前先排空每个有脏页的追踪成员（位图字扫描，稳态零成本），并在注册时推动 epoch 让新映射在 draw 路径上首推一次；`kernel page != 4 KB` 时诚实拒绝 mprotect 臂。
- **C 客户端每 verb**：image unit 高水位（push 构建独有的 `TextureState` 标记，`glBindImageTexture` 喂）替代 192 单元扫；残余填充的 7 项合取按其四个输入 memo 成位掩码；`MGPipeServerStampVerbBoundary` 的 63 字段 per-verb 推导变 constexpr 按 verb 类的掩码表；`ParsePoisonOmissionKnob` 的每 verb 字符串比较改尺寸快路径。
- **两处竞争修复（集成时）**：`generate_mipmap` 目录里是 kCtxObject，但它的 sink 是后端 `GenerateMipmap`，会读 `MGB_CTX->GetActiveTextureUnit()`——批处理下不等 apply，客户端紧接着的 validate 把 serial 与 server stamp 移走，apply 线程读到陈旧输入即 `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@ReadPixels"}`（F1 的两条 GenerateMipmap 用例在门禁树上复现）。现在它与四个 stamping verb 一样等待；规则写进代码：**只有 apply 不读残余填充任何字段的记录才可以免等**。`TextureDeathCrosses…` 测试补上 FramebufferDeath 已有的围栏（`object_death` 免等，计数只在 apply 时动）。

## 数字（VD12，同场景，世界内稳定 30 s 窗口，CPU 定频、风扇二档）

两个这台设备上不受控的变量，读表前先知道：(1) **monolith 的 fps 大多数运行被 120 Hz vsync 封顶在 ~115**（渲染线程只 66-75% 忙），偶有一次未封顶跑到 206（窗口内 110→280 摆动）；inproc 各臂 client 线程 97-99% 忙、GPU 342 MHz 空转，是纯 CPU 受限，fps 可信。(2) 内核对 app 线程忽略 `sched_setaffinity` / `taskset`：split 下常驻自旋的 apply 线程占大核 cpu7，client GL 线程被放到中核（cpu2/4/5，1555 MHz），而 monolith 渲染线程在 cpu7。所以配对比较以**逐线程 CPU ms/帧**为主指标，fps 只作参考。GPU 的 `min_pwrlevel` 每次启动被厂商守护进程打回 12，未能钉住。

| 阶段 | split (inproc) | monolith | client / apply 逐线程 CPU ms/帧 | 关键 profile 行 |
|---|---|---|---|---|
| 起点 | 7-13 fps | ~116 | — | XXH64/3 73%→32.8%，clock_gettime ~20% |
| 一轮 `cb06538c` | 60-66 fps | 112-115 | — | WaitForApplied 11.6%，绑定游走 ~8% |
| 二轮 `56a77348`（当日复测 p50） | **64.3** | 116.5 | 15.03 / 12.42 | 见上表；client 线程被调度在中核（cpu2/4/5），apply 常驻大核 |
| 二轮 + `MOBILEGL_IPC_SPIN_US=2000` | 68-73 | — | 14.0 / 13.6 | park/unpark 是二阶成本；`SPIN_US=10000` 反而 63 |
| 三轮（审查前构建） | **114.0** | 115.7 | 8.68 / 6.93 | `spin2000` 113.0，不再有收益 |
| 三轮（审查后 + 竞争修复，本提交） | 103-106 | 115（封顶）/ 206（一次未封顶） | 9.2 / 7.0-7.4（client 仍在中核） | 复跑 106.1 / 103.3 / 101.9（交错三次） |

读法：inproc 从 64 → 103-114 fps（+60-77%），client 线程 CPU 从 15.0 → 8.7-9.2 ms/帧（−40%）、apply 线程从 12.4 → 7.0-7.4（其中大半仍是自旋等待）。对着封顶的 monolith（~115）比值 ~1:1.1；对着那次未封顶的 monolith（206，渲染线程 5.0 ms/帧、大核）比值 ~1:2。按 CPU/帧看：inproc 的 client 线程在中核上做 9.2 ms 的活，monolith 渲染线程在大核上做 5.0-6.2 ms——同核折算后 client 侧仍多约 1.2-1.5 倍工作，加上 lockstep 里每 draw 等 apply 的往返，这就是剩余差距的构成。画面与 monolith 无可见差异。

## 验证

- 三轮门禁（WSL `~/w7/p5d-gate`，split 构建）：unit **2203/2203**（tcache_count=0 下）、`integration-split` **111/111**，两条曾 flaky 的用例 `--repeat until-fail:5` 全绿；一/二轮头上 unit 2188、split 111。
- `gen_pipe.py` / `gen_pipe_field_ownership.py` 的 --check/--self-test 绿，include 闭包 0 问题，dirty-surface rc=0，doc 引用 0 问题。
- 每处关键修复都有 red-once 形态的证据（一二轮：SmallRing SEGFAULT、GenerateMipmapFieldsCross exit 101、CopyImageLayered `Fatal{RoleViolation}`、余数块计数、117 MB/帧；三轮：每包报告的 red-once 表——控制影子位的 take-clear 窗口、校准自旋两条、`AThreadCreatedAfterTheLoopStoppedIsNotMistakenForTheApplyThread`、GL 线程 scope 不豁免 apply、子页映射一页保护、fault 过未绑定的映射仍在下一次消费前推送、image unit 高水位两端、残余 memo 重键）。
- 二分定位：四包分别单独上门禁树，`onlyT` 复现 GenerateMipmap abort、`onlyD` 复现 TextureDeath，`onlyB` 全绿——两条都是时序相关的既有竞争（见上），不是包的逻辑错误。

## 什么没完成

1. **R-1 序列化仍在**：每个 draw 仍等 `appliedSeq`。只读研究（`~/w7/notes/p5d/reports/E-lockstep-feasibility.md`）的结论是**不要在 P5d 做"draw barrier 延后一个 verb"**：inproc draw 路径上除 `GetBufferBindingSlot` 外每个对象类行都被 server 活读（texture unit 槽的借用指针、UBO 绑定点表、FBO 槽与附件、memo 未命中时的 VAO 属性、program 内部），且身份路径（五张前端键控 twin 表 + `HandleOfBuffer` 经客户端分配器）只能退役不能快照；`GetBufferBindingPoint` 一族根本没有 wire 形式（`set_shader_buffers` 要到 P4b）。真正的路径是 P3b/P4b（按句柄键控的 twin、`set_shader_buffers`）之后的 P11（`gPipeInputs` 版本化）——届时 barrier 延后只是把一处 `WaitForApplied` 搬到下一次 fill 之前。ROADMAP 的 P11 行还没把"`gPipeInputs` 版本化"写进去，本轮补上。
2. **线程放置**：这台内核对 app 线程的 `sched_setaffinity` / `taskset` 一律接受并静默忽略（`MOBILEGL_IPC_SERVER_AFFINITY=auto` 请求 0xc0、解析 0xff），root 下把 GL 线程 tid 写进 `/dev/cpuset/<x>/tasks`（cpus 6-7）也没有改变它的运行核（仍在 cpu5，115 fps / 8.6 ms/帧，在漂移之内）；client GL 线程常被放在中核；无法从库内解决，记录。
3. **小项**：`SetHashSuppressor` 的 XXH3（P5 遗留的 set 记录去重哈希）；`ReadDrawBindings` 每 draw 重解析 element buffer 句柄；`wants()` 门先读消费者再测脏位（R-8 计数规则未改）；两个 `RunsAsTheServerRole` / `OnServerRole` 出线调用为分层保留。
4. **验证债**：79-trace 全集本机未跑；G1 由 CI 的 pull 符号门核（四包自报 0/0/0/0，PipeStats/PipeFill/TextureState 的改动全在 `MOBILEGL_PIPE_PUSH` 或 `MOBILEGL_BUILD_DISAGGREGATED` 之内）；x86 32 位的 `MGHostSpan` 32B 断言与 `__builtin_thread_pointer` 的非 aarch64 回落只按检视；FCL 侧测试接线**未提交**（`settings.gradle.kts` 的 `:MobileGL` 指向、`FCLauncher.java` 的 renderer env 注入含 `mg_transport.txt` / `mg_env.txt` 开关、`MobileGL-disagg/build.gradle` 属性映射与 arm64 abiFilters、gradle wrapper 回退 8.13）。插件 APK 构建路径里的 `MOBILEGL_TRANSPORT=inproc` 行在打插件包前必须拿掉。
5. **平台可移植性备注**：mprotect+SIGSEGV 臂只覆盖 POSIX 且要求 4 KB 内核页（16 KB 页内核诚实回落哈希臂——是正确答案但是性能悬崖）。Windows 桌面若需要，应加 `GetWriteWatch` 实现臂而非 `#ifdef` 信号路径；哈希臂作为全平台兜底永久保留。

## 下一轮的入口

P5d 到此收官。剩余项按收益：P3b/P4b 的 twin 表句柄化（同时消掉 apply 线程剩余的 `HandleOf`/emutls 热点与 `HandleOfBuffer` 探针）→ P11 的 `gPipeInputs` 版本化 → barrier 延后。这些都是路线图既有阶段，不再单开性能轮。
