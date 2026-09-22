# P7 集成者裁定日志

> 形式沿用 P5e 的 `INTEGRATOR-DECISIONS-P5E.md`：一条裁定一个 ID，只记「定了什么、为什么、影响哪些文件」，不记来回。计划正文见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md)。

| ID | 日期 | 裁定 | 理由 | 影响 |
|---|---|---|---|---|
| ID-P7-1 | 2026-09-22 | PH-1「`Session::Fail` 翻成每会话闩」按四片重定界：(1) supervisor 读 `waitpid` status 并命名子进程死亡；(2) 两处头层裸 `abort`（`Server/StagedShadow.h:115`、`Server/StagedTextureStore.h:344`）改走漏斗；(3) 只对对端字节可达站点做 latch-and-decline；(4) 负控。**98 处 `SessionFail` 的字面翻转拒做**。 | ≥9 处返回引用的站点没有诚实返回值；§12.3 进程级 `eglTerminate` 使闩住的进程也服务不了第二会话；P6.5 fork-per-session 已给出「下一连接照常服务」。 | `ServerMain.cpp`、`Server/Staged*.h`、`PipeWireCodec.cpp`、`PipeApplier.cpp`、`ServerLoop.cpp`、`SurfaceOpCodec.cpp` |
| ID-P7-2 | 2026-09-22 | PH-6 取 drop-with-latch：`ReserveEventOrBlock` 两处 BUSY 拒绝改置 forfeit 闩 + `CountDrop()` + 返回 scratch，`ServerLoop` park 谓词见闩走正常 Stop；`kEventBacklogWaitMs` 改 `MOBILEGL_IPC_EVENT_WAIT_MS`（默认 2000）。**不等 PH-1**。 | 一个函数、一个标志、一个配置字段即闭；30 s 的「测量」从来不是测量；解耦掉 6–10 天的闩迁移。 | `ServerSession.cpp:215-263`、`ServerLoop.cpp:403-406`、`ConfigLoader` |
| ID-P7-3 | 2026-09-22 | OQ-21：**令牌足够，不做 TLS**。令牌常量时间比较、≥16 字节、无令牌只 loopback；数据面以 `Welcome.dataNonce`（CSPRNG 128 位）绑定到已认证的控制连接，认证在 fork 之前完成。TLS 作为 P12 的开放项保留。 | Ph 门的威胁模型是「任何人都能连的远程代码路径」，不是窃听：局域网 GL 命令流不含凭据。令牌 + 连接绑定关掉前者；TLS 是另一个多天项且应有自己的路线图行。 | `protocol.fbs`（`Welcome` 追加字段 → `wireFingerprint` 随之变）、`SocketTransport.cpp`、`Handshake.h`、`ServerMain.cpp` |
| ID-P7-4 | 2026-09-22 | 出口门 3（真机 ssim 1.0）的分母排除 `minecraft-1.21.4-rd12-odinlite-in-world` 与 `create-indirect`；`iterationrp` 按 `apk.yml:460-462` 带三个 Magma 旋钮。 | OQ-17：两者在 `dev@81b17c0b` 就在 Adreno 830 上坏，非本分支造成。 | `CONTRACT-P7.md`、设备矩阵跑器 |
| ID-P7-5 | 2026-09-22 | `SetupDrawSnapshot` 探测字段塌 dirty mask、`VertexInputStateFactory` 内容寻址 CSO **本阶段拒做**，记入优化阶段。 | `VulkanRenderer::SetupDraw` 在非 monolith 传输下分支到 `SetupWireDraw`（`:6946`），monolith 快照机制在 split 下不跑；无性能门；改 monolith 表动 pull `.text`。 | 无 |
| ID-P7-6 | 2026-09-22 | D18 取 21-memo 普查的读法（节点稳定容器纪律；负控 C 必须仍能到达已发货臂）。 | 两名反驳者对指涉不一致，必须由集成者定一个。 | `CONTRACT-P7.md` 一句 |
| ID-P7-7 | 2026-09-22 | 184 符号棘轮归 P7 wave 0；baseline 存符号列表而非计数；`--assert-monotone` 只对新增符号红，消失只提示重基线。 | 裸计数的棘轮在「清一个加一个」时通过；P6.5 行从未接收该移交。 | `scripts/link_ratchet.py`、`scripts/data/link_ratchet_baseline.txt`、`test.yml` |
| ID-P7-8 | 2026-09-22 | 实现方 `opus`、审查方 `fable`；每包合并前聚焦审查一次，阶段收官整体审查一次。 | ID-66「实现方与审查方错开」；本环境无 Codex。 | 过程 |
| ID-P7-9 | 2026-09-22 | 性能只记录不设门（沿用 2026-09-08）；wave 2-B 若实测 CSO 缓存为热点才做 P7-5 的 server 半边。 | 路线图纪律。 | 过程 |
| ID-P7-10 | 2026-09-22 | wave 0 之前不派任何 `@P7` 拒绝退役的活。 | 今天没有一条车道能让 Magma 两进程用例变红，每个修复都需要 red-once。 | 过程 |
| ID-P7-11 | 2026-09-22 | pull 构建（`MOBILEGL_PIPE_PUSH=OFF`）必须**连测试一起**编过：`MG_Test/Util/PipeStatsTest.cpp:137` 用了 push-only 的 `PipeStats::Gauge`，已加 `#if MOBILEGL_PIPE_PUSH` 并保留用例名以维持名集合奇偶。CI 的 pull control 作业关了 `MOBILEGL_BUILD_TEST` 所以看不见——这是 CI 的一处盲区，随 wave 0 的 CI 改动补一条「pull 构建含测试」的编译步骤。 | 「默认 ALL target 必须完整构建」。 | `PipeStatsTest.cpp`、`test.yml` |
