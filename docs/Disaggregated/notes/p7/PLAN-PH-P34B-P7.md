# Ph / P3b-P4b / P7 统筹计划（2026-09-22，基线 `feat/disaggregated@b95f5f1a`）

> 用户指令：「查看 ROADMAP，统筹并推进实现，直到 P7 结束。先计划，看看 Ph、P3/4 深化、P7 有哪些任务已经完成、哪些不必要、哪些是必须的，然后开工实现直到 P7 结束。」
>
> 本文是那份计划。判定来自一次对树的逐项审计（19 组读者、每个 done / obsolete / unnecessary 判定两名反驳者、一名完整性评审；66 个代理、3080 次工具调用），**每条判定都要求 `file:line` 证据，不接受文档句子**。原始产出在会话 scratchpad（`audit-digest.md` 411 KB），本文只留结论与可调度的剩余工作。

## 0. 一句话结论

P7 收官的关键路径是：**先造仪器（wave 0）→ 真机诊断（wave 1）→ 三簇 `@P7` 拒绝退役 + Magma 死亡表 + 深度 mip 烘焙（wave 2，并行）→ verify×split + 全门 + 40% 检查点（wave 3）→ 真机 ssim 1.0 + CTS（wave 4）**。P3b/P4b 的 Espryt 项与 P7 的文件完全不相交，作为并行流推进；Ph 按路线图本来就排在 P12 之前而不是 P7 之前，本轮只做它「必需且小」的部分（观测子进程死亡、两处漏斗旁路、五处无界分配、常量时间令牌 + 数据面绑定、SEG_EVENT 弃投闩），fuzz 臂 2（对端字节可达站点的逐站点闩转换）排在 P7 之后。

三个最重要的发现，都是路线图文本没写的：

1. **Magma 今天没有任何两进程集成覆盖。** `mgl_itest_register_split_arms` 的四条臂（Split / Spawn / Tcp / TcpDevice）全部硬编码 `MOBILEGL_BACKEND_TYPE=DirectGLES`（`MG_IntegrationTest/CMakeLists.txt:2002/:2025/:2031`），`MGL_ITEST_MAGMA_WIRE_ENVIRONMENT` 硬编码 `MOBILEGL_TRANSPORT=inproc`（`:2456`）；三个共享标签 `integration-split / -spawn / -tcp` 里 DirectVulkan 条目为 **0**。P7 出口门 1 不只是未达成，它**不可测**。retrace 半边有 Magma × {inproc, spawn}（`test.yml:2449/:2470`），集成半边没有。
2. **184 符号棘轮（契约 §12.1）没有仪器。** `scripts/ci/` 里没有任何脚本算这个数；P6 说「移交 P6.5」，P6.5 行从未接收。P7 出口门 4 与 P3b/P4b 的 14 符号都写在它之上。
3. **真机 DirectVulkan 分歧从未被诊断。** 树上只有两个点值（0.988998 / 0.976494，`CURRENT_STAGE_PROGRESS.md:158`），无 per-case 归属、无重复次数、原始证据不在树内；判别力最高的实验（inproc + `MOBILEGL_IPC_RUN_AHEAD=0` 在 golden 口径下）**从未跑过**，且不需要改任何代码。

## 1. 逐项判定

判定词：**done** = 代码已在树上且有证据；**partial** = 一半落地；**not_started**；**obsolete** = 后续阶段用另一条路关掉了需求。必要性：**必须**（P7 收官或该行出口门依赖）/ **可选** / **不必要**（明确拒做并说明理由）。标 ⚠ 的是审计员判 done/unnecessary 但被两名反驳者高置信推翻、本文采纳反驳意见的项。

### 1.1 Ph（不可信对端加固 + 配对）

| 项 | 判定 | 必要性 | 剩余工作（可调度） | 规模 | 波次 |
|---|---|---|---|---|---|
| PH-1 `Session::Fail` 翻成每会话闩 | partial → **(3)(4) done**（F2 latch 包，`p7/f2-latch`：会话子进程武装的 `SessionLatch`、exit 75、inproc 保持 Fatal——`CONTRACT-P7.md` §10 第 7 条；逐站表 `scripts/ci/ph_latch_sites.py --table`；第二批站点、PH-2、生成门记在 §12） | **必须，但重新定界** | 路线图写的「每个 `[[noreturn]]` 站点造真实返回路径」（98 处 `SessionFail` + 6 处 `WireLogFatal`）**拒做**：≥9 处返回引用（`ServerSession&`、`RingControl&`、`Doorbell&`、`ClientSession&`，`ServerSession.cpp:270/283/740/765/774/784/790`、`WireTables.cpp:118`、`EmitTables.cpp:212`）没有诚实返回值；且 §12.3 的进程级 `eglTerminate` 使「闩住但活着的 server 进程」也服务不了第二个会话。P6.5 的 `--serve` fork-per-session（`ServerMain.cpp:302`）**已经**给出「下一连接照常服务」。真正欠的四片：(1) supervisor **观测并命名**子进程死亡——三处 `waitpid(active, nullptr, WNOHANG)`（`:279/:289/:294`）换成读 status、记 `pid exit=/signal=`、计 `sessionsFaulted`；(2) 两处绕过漏斗的裸 `abort`（`Server/StagedShadow.h:115`、`Server/StagedTextureStore.h:344`）改走 `SessionFail`；(3) **只对对端字节可达的站点**做 latch-and-decline（`PipeWireCodec.cpp` R-2 诚实臂 + 解码边界、`PipeApplier.cpp` 七处、`ServerLoop.cpp` 四处、`SurfaceOpCodec.cpp` 三处——它们都已在 `Bool`/`MobileGLResult` 返回的函数里），`DrainRing` 顶部加闩检查；(4) 负控：喂畸形帧 → 会话闩住 → 下一连接得到 Welcome。 | (1)+(2) 1 天；(3)+(4) 2–2.5 天 | (1)(2) 与 P7 wave 0 同一提交（费用极低）；(3)(4) 在 P7 之后 |
| PH-2 slot 预算（`SlotTables.h:347/:371` 两条 `MOBILEGL_ASSERT`） | partial（wave 2-F 未做，见 `ph-f.md` §4）→ 具名拒绝经 `MGPipeSessionFail` 已落地（F2，`CONTRACT-P7.md` §10 第 6 条）；闩已落地（F2 latch）但**改闩未做**，理由与八处站点见 `CONTRACT-P7.md` §12（`@Ph-declined (ID-P7-1)`） | 必须 | 提升为 release 下具名拒绝（先经 `SessionFail`，闩落地后改闩） | 0.5 天 | 与 D11 五处一起 |
| PH-3 readback 尺寸门（`PipeApplier.cpp:444`） | not_started | 必须 | 绝对上限取 P6.5 `LinkTerms.maxReplyBytes`（server 自陈），在 resize **之前**比较，超限回 `PostReply(kStatusError)` 而非 Fatal | 4–6 h | D11 |
| PH-4 `StagedTextureStore` 按 level 范围设界（`:440`） | not_started | 必须 | `CopyRunInto` 传入 level 宣告 extent，`imageOffset+byteSize` 越界即拒绝 | 1 天 | D11 |
| PH-5 `ArchiveVector` 上限（`ProgramArtifactsCodec.cpp:309`） | not_started | 必须 | `TakeCount` 带元素大小，`raw*sizeof(T)` 对 `Remaining()` 设界 | 4–8 h | D11 |
| D11 第一行 `create_render_state` 的 `Cso.Slot+1` 无界 resize（`PipeApply.cpp:1575`） | not_started | 必须 | 加 `kMGPipeMaxRenderStateCsoSlots`，走同族 `RecordAt(table, slot, limit)` | 2–3 h | D11 |
| PH-6 `SEG_EVENT` 不排空对端 | not_started | 必须 | 取 **drop-with-latch** 形状（不依赖 PH-1）：`ReserveEventOrBlock`（`ServerSession.cpp:215-263`）两处 BUSY 拒绝改为置 `m_reverseChannelForfeit` + `CountDrop()` + 返回 scratch，`ServerLoop` park 谓词见闩走正常 Stop；`kEventBacklogWaitMs=30000` 改成 `MOBILEGL_IPC_EVENT_WAIT_MS`（默认 2000）；先判 `Dead()` 免把对端死亡误报成 30 s 超时。两条单测（stonewall / trickle）。路线图「60 秒」实为 30 s（两轮）且涓流对端毫秒级触发 `:259` | 1.5–2 天 | P7 之后 |
| PH-7 配对 / 认证 | partial ⚠ → **(1)–(4) done**（wave 2-F，`ph-f.md`；F 修复轮 `ph-f.md` §6：绑定后关闭 hand-off / 单会话监听者、schema 摘要钉住 revision）；(5) 未做 | 必须 | 已有：非 loopback 无令牌拒绝监听（`SocketTransport.cpp:160`）、Hello 带 token、Refuse。欠：(1) **常量时间比较**且两处策略（`ServerSession.cpp:498` / `ServerMain.cpp:93`）合一；(2) 名字 `Refuse{AuthenticationRequired}` 不在 `RefuseCode` 枚举里——用 `Authentication` 或追加枚举，并给 `fatal_census.py` 加 `Refuse{Word}` 词表门；(3) 令牌最小长度 ≥16 字节（在 **TCP** 监听处检查；unix 端点不问）；(4) **数据面未认证**：`AcceptPair` 按到达顺序配对、从不查 peer（`SocketTransport.cpp:371/:391`）——`Welcome` 带 `dataNonce`，第二连接首帧必须回它；认证挪到 fork 之前；(5) 有界的预认证工作 + 失败退避；(6) OQ-21 裁定（见 §5 ID-P7-3） | (1)(2)(3) 1 天；(4)(5) 2 天 | (1)(2)(3) 与 wave 0 同期；(4)(5) P7 之后 |
| PH-8 尺寸由 server 钳制 | **done**（wave 2-F：单测 + 两进程 smoke `hello_asks_64_gib`；F 修复轮：超出所授条款的请求记一条 `MGLOG_W`） | 必须 | 已陈述；补「client Hello 要 64 GiB 时 server **忽略请求、按自己的尺寸建段**而非分配」的单测（不是钳制：四个计数根本不参与定尺寸） | 0.5 天 | P7 之后 |
| 门：`Session::Fail` 站点普查零新增 | partial ⚠ | 必须 | `scripts/ci/fatal_census.py` 存在且在 CI（`test.yml:2011`），但 (a) `SCAN_ROOT` 只有 `MG_Remote`，server 进程里 ~79 处 abort（DirectGLES.cpp 16、Managers.cpp 12、VkBufferManager.cpp 11、PipeRoute.cpp 7……）看不见；(b) `abort_sites` 记了从不比较；(c) 12 个 family 词无 `.def` 行。改：SCAN_ROOT 扩到 server 镜像（MG_Remote / MG_Pipe / MG_Backend / MG_Impl/Pipe / MG_State），`abort_sites` 变棘轮，补 `.def` 行 | 1 天 | **wave 0**（P7 需要它看见 `@P7` abort） |
| 门：fuzz 臂 1 畸形控制帧 | partial | 必须 | 机制已在（`Framing.h:168` latch、`ControlInbox`），欠端到端脚本 `scripts/ci/ph_fuzz_control_frames.py`（五种畸形）+ `ServerMain.cpp:193` 补 `CtrlEnvelopeBufferHasIdentifier` + ctest 车道 | 1.5 天 | P7 之后 |
| 门：fuzz 臂 2 超界计数 | not_started → **done**（F2 latch 包：`MG_Test/Wire/PeerLatchTest.cpp` 每个转换站点与 D11 五处各一行 raw-peer 负控，每行之后同一 `--serve` supervisor 的下一会话得到 Welcome 并渲染；PH-2 与 D11 的 MG_Pipe 钩仍是具名 Fatal（`signal=6`），见 `CONTRACT-P7.md` §12） | 必须 | = PH-1 (3)(4) + D11 五处 | 见上 | P7 之后 |
| 门：fuzz 臂 3 不排空对端 | not_started | 必须 | = PH-6 + 场景 | 见上 | P7 之后 |
| 门：非 loopback 无令牌具名拒绝 | **done**（wave 2-F：smoke 入 CI，SKIP 即红） | 必须 | 已了：名字改为词表内的 `Refuse{Authentication}`（PH-7 (2)）；`tcp_supervisor_smoke.py` 挂为 `TcpLane.SupervisorProtocolControls`（`integration-tcp`），CI 构建钉版 flatc 并以 `junit_tally.py --require-entry-passed` 要求它**跑了且过了** | 含在 PH-7 | 同期 |
| OQ-21 令牌够不够 / TLS | 已答（ID-P7-3；wave 2-F 落地） | 必须裁定 | 见 ID-P7-3 | 0 | 本文 |
| OQ-24 E1 对照重定义（ID-122） | 无主 | 可选 | 挂到普查门的 S8 (`SessionFaultCount()==0`) 负控设计上 | 0.5 天 | P7 之后 |

### 1.2 P3b / P4b 深化（Espryt）

| 项 | 判定 | 必要性 | 剩余工作 | 规模 | 波次 |
|---|---|---|---|---|---|
| 按存储属主键控的发射游标 + view 索引重映射 | **代码半 done**（P4a：`TextureEmit.h:1075-1108` 游标按属主 `{slot,gen}`；`TextureObjectView::ToOwner*` 一直存在；wire 半 P5e tx2） | 验证半必须 | 新 `TextureViewAliasScenario`（view 上传 / 属主采样、跨 draw 两次 drain，断言 `SubDataCount()`/`DrainListSize()`），注册 split 三臂并给 `MGL_ITEST_GLES_{SPLIT,SPAWN,TCP}_ENVIRONMENT` 加 `MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1`；red-once：让 view 自己 `PipeNoteLevelDirty` | 1–1.5 天 | wave 2-D |
| `g_fboTextureSyncList` | 分离安全（`DirectGLES.cpp:3922` 记录臂先返回；退路是 `Fatal{UnmigratedPipeInput}`）⚠ | 小件必须；文本删除**不属本阶段** | `ResolveFramebufferSubsystemArm`（`Managers.cpp:4299-4344`）及三个同族 resolver 在有传输且 verdict≠Handles 时改成启动期具名 stop（`StopOnArmlessPipeSubsystem`），把帧中 Fatal 变成启动期拒绝；文本删除受 P5e 裁定 1 / `Config.h:398` 约束，归 P13 | 2–4 h | wave 2-D |
| `ResolvedTextureBindingMemo` / `SamplerPassMemo` / image sweep / program registry 重键 | done（P4a/P5f） | 门必须 | 一条纯度 grep 门：split 构建下这些 memo 键里不得出现前端指针类型；接 CI | 0.5 天 | wave 2-D |
| XFB scatter 搬到 client | **不做搬迁**（P5c/P5f 走了相反的路：server 读自己的 staged shadow，经 `OnBufferWriteback` 回）⚠ | 两处真洞必须 | (1) 孤儿目标：`ScatterCapturedRecords` `:1946` 的 `continue` 把 `HasDefinedContent==0` 的合法目标**静默丢弃**——改成 monolith 的行为（零填充 staged 再上传），`RequireStagedCoverage` 以 `ResourceContentIsDeclared` 守卫；(2) **无界回写事件**：`:1969`/`:1782` 各投一条整段 `OnBufferWriteback`，>128 KiB（`EventRingBytes/2`）即 `Fatal{EventRingOverflow}`——按 `EventRingCapacityBytes()/4` 切片（`BufferObject.cpp:601-606` 的形状）；(3) 删死声明 `OnXfbScatterReady`（`MGPipeCallbacks.h:49-51`，`kMGPipeCallbackCount` 10→9）；(4) 注册 `XfbRepeatedCaptureScenario` / `XfbAfterClipDistanceScenario` split 三臂 | 1.5–2 天 | wave 2-D |
| 删 fragColor 重推导 workaround 与 `g_broadcastMemo*` | partial | 可选（全删动 pull `.text`，归 P13） | 只做注释 / 标签修正 | 1 h | wave 2-E |
| raw-depth-fetch sampler 的 monolith 清理 | partial | 可选 | 步骤 1+2（参数化测试） | 4 h | wave 2-D 尾 |
| 回读 / pack state | **pack 半 done**（`set_pixel_pack_state` APPLIER_DERIVED、server 中性读、client 侧 PBO 散射 `EmitTables.cpp:994-1007`）；**readback 半开**⚠ | 必须 | (1) 先武装车道：`F1WireScenario.TextureAndFramebufferReadsPreservePackBufferPadding` 等进 split 三臂 + `PixelStoreSweep` / `DepthStencilReadbackMatrix` / `PackedWordReadback` / `LayeredTextureReadback` 注册（DS 需镜像 `MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION`）；(2) `get-tex-image-shadow` / `copy-image-shadow-mirror` 在非 monolith 下仍 `std::abort`（`DirectGLES.cpp:14851-14855` 自己写着归 P3b/P4b）——改具名拒绝或实现；(3) 新用例：绑定 PACK PBO + 非默认 `PACK_*` 读回；(4) 路线图 `:244` 的「P5 具名拒绝」是错的（`70df57bb` 起已实现） | 2–3 天 | wave 2-D |
| P4a R-3 / R-4 / R-5 / R-7 / R-10 / R-11 | 混合 | R-5、R-11 必须；余可选 | R-5：D-K2 子系统依赖规则六处合一处（`MGPipe.h:115-125` 注释过期）；R-11 = TUS 升门（下）；R-3 契约一句话；R-4 G9 推导（1–2 天，可选）；R-7 三通道 CPU mip 重武装（1–2 h，可选）；R-10 ABA 对照 Magma 半依赖 P7 | R-5 0.5–1 天 | wave 2-D |
| `ProgramArtifacts.h` NDK 尺寸钉 | **obsolete**（两名反驳者均维持）：P6.5 portable archive v2 + 布局指纹取代了 `sizeof` 钉 | 不必要 | 只删两处过期文档句 | 1 h | wave 2-E |
| D12 的 14 个符号 | not_started | 可选（P7 棘轮目标不含它们） | Tier 1（0.5 天）在棘轮落地后做 | 0.5 天 | wave 3 后 |
| 门：`TextureUploadShapeScenario` 升级成门 | partial | 必须 | 把 `RecordProperty` 变 `EXPECT_EQ`（金标从当前游标实测，不用 P4a 的 emit=6 box=6）；加第三张 view 上传纹理；加 split 臂（需 `PipeStatsWindow` 认识角色派生日志名）；red-once 翻 union-box 谓词。**Mali 帧时增量不可调度**（只有一台 Redmi）——按 Adreno 记录并写明偏差 | 1–1.5 天 | wave 2-D |
| 门：纹理 / program 场景 split 覆盖 | partial | 必须 | ~20 个候选场景（TextureView / TextureParamsWithoutASamplerView / SampledSetStaleness / ImageSizeAfterRespec / … / ProgramPipeline / PostLinkAttach / RelinkStageSet / UniformInitializer …）先在 inproc 跑一遍记首阻塞，按 P7/P8/P9 归属决定注册或具名排除，写 `notes/p34b/split-scenario-census.md`；DirectVulkan 臂等 P7 | 3–5 天 | wave 2-D（后半） |
| 门：每一个 Iris trace | partial | 必须 | CI 管道 P6 已建（retrace-split 全矩阵 × 双后端 × {inproc, spawn}）；欠**结果**：在当前头重跑 39 case × 2 后端 × 2 传输的普查（上次是 P5b 的 72/6/1），写 `notes/p34b/split-trace-census.md`；唯一已知阻塞 `texture-remint-pull`（`Managers.cpp:6359/:6376`）归 P9——门文本按名排除并列 P9 票；模拟器臂 2–6 例/轮 flaky，分 gating / informational 两档 | 1–2 天 | wave 3 |
| 门：CTS `texture_*` / `shader_image_*` / `packed_pixels`（+ P4a 欠的 `direct_state_access.framebuffers*`）0.5 pp | not_started | 必须 | `tools/cts/` 有完整 harness 但**无 caselist 入库、无 baseline、无 delta 工具、三个跑器不认 `MOBILEGL_TRANSPORT`**。做：caselist 入库（`tools/cts/caselists/`）、`$BASE`（monolith 读数）、`cts_delta.py`、`packed_pixels` 一条 split 臂 | 4–6 天（多为墙钟） | wave 1（`$BASE`）/ wave 4 |
| OQ-14 memo 命中率（`sve ≈ draw 数`） | 无主 | 可选 | 四个重键 memo 家族的命中率计数 + 一次设备读数 | 0.5–1 天 | wave 4 同窗 |

### 1.3 P7 DirectVulkan（Magma）全量迁移

**已落地（只欠文档）**：`38919d45` server buffer consumers + Android 旋转 blit；`194382c9` run-ahead / 延迟退休 / VAO 身份碰撞；**`ed507b1a..d56c0e83` 29 个 DirectVulkan 提交的 shape 波**（占位图像 `WirePlaceholderImages.inc`、packed-float mip 原生 blit `5310ff62`、resolve `ec4ca06d`、copy-image 端点、XFB 捕获 span、纹理回读）——路线图 P7 行**一字未提**；`70fb6689` 链 inproc RD32 性能回归关闭（86.85→116.73 fps）；占位纹理原生化 **done**（但无 Magma split 车道条目跑它）；D-B8 具名 UBO 走了「直接描述符绑定常驻 `VkBuffer` range」的备选（`UniformManager.cpp:2375` 直绑分支）——欠一次测量 + 裁定；`DynamicBackendParameters` 定宽 **done**（P6.5 wf，两名反驳者维持）。

| 项 | 判定 | 必要性 | 剩余工作 | 规模 | 波次 |
|---|---|---|---|---|---|
| 出口门 1：集成 + trace 在 Magma push 与 split 下全绿 | partial | 必须 | (1) `mgl_itest_register_split_arms` 加 backend 维度（`MGL_ITEST_${backend}_${arm}_ENVIRONMENT`，新增 `MGL_ITEST_VULKAN_{SPAWN,TCP}_ENVIRONMENT` 拼 ICD 钉）；(2) **`WireTables.cpp:740`** 死亡通知安装条件去掉 `ActiveBackendType == DirectGLES`（否则 Magma spawn 的 `CtWireScenario` 死亡用例静默 `ObjectDeaths=0`——P6.5 在 GLES 上踩过的同一个洞）；(3) 73 条 `integration-magma-split` 重铺三臂（`test.yml:1570-1632` require_green 名单同步）；(4) retrace-split 作业补 iterationrp × DirectVulkan 的三个旋钮导出（`test.yml:2575` 对齐 `:2183`）；(5) 首轮红名单 = 后续工作清单 | 3–5 天 | **wave 0** |
| 出口门 2：verify 零分歧 | partial | 必须 | verify 今天只注册 monolith（CMakeLists:1684-1760）；verify+split 撞旧 `PipeRespecifyScope` 断言（`validation-status.md:118`）——放宽到「按 level 的 producer 各自覆盖其宣告范围」+ 正反单测；加 `integration-verify-split` 环境（双后端 × inproc）；两条负控（VERIFY_CORRUPT / POISON_OMIT）在新臂各红一次 | 2–3 天 | wave 3 |
| 出口门 3：真机 DirectVulkan split ssim 回到 1.0 | not_started | 必须 | 见 §3 wave 1 的实验 E0–E6；修复在定位前不估 | 定位 1–2.5 天；修 2 天–1 周 | wave 1 / wave 4 |
| 出口门 4：184 符号棘轮下降 P7 的 101 个 | not_started | 必须（两名反驳者推翻「不必要」） | `scripts/link_ratchet.py`：按 a6-link-experiment-data.md 的路径规则分 SERVER / FRONTEND / SHARED 对象，`nm` 求 (SERVER undefined) − (SERVER defined) − (SHARED defined) ∩ (FRONTEND defined)，输出**符号列表**（不是裸计数）+ `--bucket` 按引用对象分桶；baseline 入库 `scripts/data/link_ratchet_baseline.txt`；CI 步骤在 `build-linux` 作业内；red-once；`--self-test` | 1–1.5 天 | **wave 0** |
| 出口门 5：CTS 0.5 pp | not_started | 必须 | `run_cts_local.py` 透传 `MOBILEGL_TRANSPORT` / `MOBILEGL_IPC_SERVER_PATH` + spawn 臂 arming 断言；块：`texture_*` / `shader_image_*` / `packed_pixels` / `direct_state_access.*` / `uniform_buffer_object*`；先 monolith×DirectVulkan baseline 入库，再 inproc×DirectVulkan 比对，pass 率差 ≤0.5 pp 且**新 crash = 0 单独硬红** | 2–4 天 | wave 1（baseline）/ wave 4 |
| `@P7` 具名拒绝（树上 **15 处 / 4 文件**，路线图印 8 条） | partial | 分必须 / 可选 | **全部是 `std::abort()` 且绕过 `Session::Fail` 与普查门**。先 (0) `MagmaWireFatal`（`WireFramebuffer.inc:4-7`）/ `WireDescriptorFatal`（`UniformManager.cpp:68-71`）/ `WireBufferLegacyFatal` 改走 `MG_Remote::SessionFail`（wave 0）。**必须退役**：`.6 uniform-buffer-byte-tail`（`UniformManager.cpp:2391`，真机 MC 报告 §5 的活边界）、`.8 copy-image-in-place`（`VulkanRenderer.cpp:10383`，GENERAL 布局单次转换 + 重叠拒绝）、`.9 vertex-layout`（`WireDraw.inc:245`，保留 `buffer-window` 一种为协议错，其余六种改 monolith 的「屏蔽该属性继续画」）、`.11 default-color-blit-shape`（`WireFramebuffer.inc:601`，恒等变换回落 `vkCmdBlitImage`，旋转走 scratch）、`.12 multisample-blit-shape/-aspect`（`:631/:641`，scratch resolve-then-blit + 深/模板 resolve）、`.13 depth-stencil-mipmap`（`:753`，随深度 mip 烘焙）。**可选（改为 decline 即可）**：`.1 texel-buffer-native-format`、`.2/.3 unaligned-{texel,storage,atomic}-range`、`.4/.5 *-native-range`（钳/拆而非拒绝）、`.7 dynamic-offset >4 GiB`、`.10 vertex-format-conversion` | 必须簇 7–9 天；可选簇 2 天 | wave 2 A/B/C |
| Magma 无 `StateObjectDeathOps` | not_started | 必须（车道前提） | 镜像 `g_glesStateObjectDeathOps`（`Managers.cpp:335-474`）装表；删六处 Magma 特判（`CtWireScenario.cpp:151`、`PipeFill.cpp:1882` 等）；`PipeSlotPeek` 断言死亡后槽位回落 | 1–1.5 天 | wave 2-C |
| 内部 shader 烘焙 | partial（blit 已烘 `WireColorBlitSpirv.h`，仅 split 臂） | (A)(D) 必须 | (A) 深度 mip 程序烘焙（`WireDepthMipmap.{vert,frag}` + `.inc`，`GenerateWireMipmap :752-769` 深度分支接入）；(D) `MOBILEGL_BAKED_INTERNAL_SHADERS` 新鲜度门（表驱动，覆盖 `MG_Util/SelfTest` 两个已有烘焙头）；(B) monolith 臂退役（动 pull `.text`，归 P13）；(C) 直通 TCS 烘焙（64 模块，归 OQ-7 窄半，可选） | (A)+(D) 2.5 天 | wave 2-B |
| 两处 `MagmaP7AllocatorDebtScope` | partial | 必须 | 证明在非 monolith 下从不武装（临时断言 + split / 双块车道），然后删；随 (A) 一起 | 0.5 天 | wave 2-B |
| OQ-8 一份反射归档服务三个消费者 | partial | 必须（小） | `MagmaProgramSource::EnsureStorageBlocks`（`.h:191`）**每 draw 重跑 SPIRV-Reflect**——在 `LinkArtifacts` 加有序 `storageBlocks`（stage 序 + 规范名 + binding 去重），codec 版本 +1，wire 与 monolith 消费者都改读它（顺带删 `g_programResourceCaches` 的悬挂哈希风险 `DirectVulkan.cpp:884-895`）；索引序等价单测 | 1.5–2 天 | wave 2-C |
| OQ-10 `kCapResidentSubData` 未接线 | partial | 必须（小） | Magma 实现已补（`VkBufferManager.cpp:123/:148`），但 `MG_Backend/Init.cpp:192-219` 从不 OR 该位 ⇒ **split 下两个后端都退回就地 memcpy**。按 server 自己的 wire 表判 `SubDataResident != nullptr` 发布；`RemoteClientTest.cpp:594` 改期望 + red-once；integration-magma-buffers 加白盒用例；26.3 in-world 回归 | 0.5–1 天 | wave 2-C |
| `SetupDrawSnapshot` 探测字段塌成 dirty mask | not_started | **不必要（本阶段拒做）** | `VulkanRenderer::SetupDraw` 在任何非 monolith 传输下分支到 `SetupWireDraw`（`:6946`），monolith 的 `TrySetupDrawFastPath` / 快照数组在 split 下**根本不跑**；这是无性能门路径上的性能项，且改 monolith 探测表会动 pull `.text` | — | 记入 P13 / 优化阶段 |
| `VertexInputStateFactory` 内容寻址 CSO | not_started | **不必要（本阶段拒做）** | 同上：`m_cache` 是 monolith 路径；只有 wave 2-B 的测量要求时才做 server 半边 | — | 同上 |
| D18 容器纪律 | 歧义 | 可选 | 两名反驳者对「D18 指哪个」意见不同（BRIEF-P2 的 D18 vs 21-memo 普查的 D18）——由集成者一句话定读法（ID-P7-6）后 ~1 h | 1 h | wave 2-E |
| D-B8 具名 UBO host payload / OQ-6 | partial | 必须（小） | 直绑备选已在树上；补一次 `stage-ubo-named` 字节/帧测量 + 在 CONTRACT-P7 里批准备选、关 OQ-6 | 0.5 天 | wave 2-E |
| OQ-7 `MG_Util` 切割缝 | not_started | 可选 | 窄半 = 烘焙 (C)；宽半（头/对象库拆分）归 P13 | — | P13 |
| OQ-13 烘焙 shader 能否表达 uniform / UBO 布局 | 颜色 blit 已答 | 可选 | 窄化到深度 mip，随 (A) 关闭 | 0 | wave 2-B |
| 债务表「22 → P4b/P7 texture readback」「3 → P7 query」 | 待核 ⚠ | 必须（一次比对） | 两条「obsolete」判定都建立在 inproc 硬门上，两进程半边无覆盖；`MEASUREMENTS.md` §7.2 的同名比对**自 P5b 起没重跑过**——在当前头重跑一次，四行债务一次结清 | 0.5 天 | wave 3 |
| 「再基线检查点：中点完成子系统 < 40% 立即重定基线」 | **不可执行** | 必须定义 | 树上没有「子系统」分母、没有 `CONTRACT-P7.md`、没有 `notes/p7/`。wave 0 定：分母 = §4 的 9 个子系统，中点 = wave 2 合并完成 | 0.5 天 | **wave 0** |
| OQ-17 create-indirect / rd12 Magma 在 Adreno 830 上的 dev 侧崩溃 | 已知 | 必须裁定 | 出口门 3 的分母排除这两条，写进 CONTRACT-P7（ID-P7-4） | 0 | wave 0 |

## 2. 路线图文本需要更正的地方（汇总，wave 0 的文档提交一并改）

- P7 行：`@P7` 清单 8 → 15（补 `vertex-layout`、`default-color-blit-shape`、`storage-buffer-native-range`、`uniform-block-native-range`；`unaligned-*-range` 展开三个；`multisample-blit` 两个名；`depth-stencil-mipmap` 已窄化到 D|S 合并）；补 29 提交 shape 波；`内部 shader 烘焙` 半已落地；`DynamicBackendParameters` 定宽已由 P6.5 落地（`CONTRACT-P6.md:666` 仍写 P7）；「§2.8」是死引用（`CURRENT_STAGE_PROGRESS.md` 已为 P6.5 重写）；真机分歧改成「两次观测 0.988998 / 0.976494，case 归属与重复次数未记录，原始证据不在树内，且取自 run-ahead ARMED 下、lockstep 对照从未在 golden 口径下跑过」。
- P3b/P4b 行：`XFB scatter 搬到 client` 删（改为两处真洞）；`回读 / pack state` 拆成「pack 已关 / readback 开」；`g_fboTextureSyncList` 文本删除不属本阶段；`按存储属主…` 从交付列挪到门列；CTS 门补第四块 + 注明读数传输与基线；`每一个 Iris trace` 注明管道已在、欠结果与 P9 例外表；`:244` PACK-PBO「P5 具名拒绝」过期。
- Ph 行：「~90 站点」→ 98 + 6；「收口到一处」→ 两个漏斗 + 两处头层旁路 + `PipeApply.cpp:70` 的第三个宏；「60 秒」→ 30 s / 涓流毫秒级；`非 loopback 无令牌拒绝` 已由 P6.5 ct 落地但名字不在词表、数据面未认证；「P12 非 loopback 监听之前」——P6.5 **已经**在 LAN 上跑了 `tcp://0.0.0.0`，顺序陈述落后于树。
- 「184 棘轮移交 P6.5」——P6.5 行从未接收，本计划把它归 P7 wave 0。

## 3. 波次

资源模型：一名集成者（本会话）+ N 名实现代理（各自 git worktree，`~/w7/pipe` 派生）+ WSL `~/w7/pipe` + lavapipe 跑主机门 + **一台** Redmi `2f7cbe2e`（串行，设备需求成批）。无 Mali。

### wave 0 — 仪器（集成者，~3–4 天，无并行）
今天 P7 没有一条门能被测；先造仪器，仪器没有之前不派活。
1. **Magma 两进程车道**：`mgl_itest_register_split_arms` backend 维度 + `WireTables.cpp:740` + 三臂重铺 + retrace-split 旋钮导出。首次跑 `integration-magma` × {spawn, tcp}——**它的红名单取代本文所有猜测**。
2. **`scripts/link_ratchet.py`** + baseline + CI 步骤 + red-once。一个脚本，退掉五个重复项。
3. **普查门扩容**：`fatal_census.py` SCAN_ROOT 扩到 server 镜像、`abort_sites` 变棘轮、补 `.def` 行；同一提交把 Magma 三个 wire fatal 漏斗改走 `SessionFail`，两处 `Staged*` 裸 abort 改走漏斗，supervisor 读 `waitpid` status 并命名（Ph (1)(2) 顺手）。
4. **`MG_Remote/CONTRACT-P7.md` + `notes/p7/`**：子系统分母（§4）、40% 检查点的中点定义、OQ-17 排除表、D18 读法、钉住的 `@P7` 普查数、OQ-21 裁定。
5. **设备矩阵跑器**：`run_tcp_matrix.py` / `run_android_retrace_local.py` 能按 backend 选 DirectVulkan、`--matrix`、三行传输环境修正（`TraceReplayActivity.java:374-376`）。

### wave 1 — 设备窗口 #1（~1 天在手机上，与 wave 2 派活并行）
一次 reboot-clean、钉频（`pin_device.sh 2f7cbe2e pin`），一个窗口打三笔债：
- **P7-7 实验 E0–E3**（全部不改码）：E0 归属与确定性（monolith / inproc 各三遍，存 actual 图，得出**哪些 case 分歧**、三遍是否逐位相同）；**E1 `--env MOBILEGL_IPC_RUN_AHEAD=0` lockstep 配对**（判别力最高：回 1.0 ⇒ run-ahead 排队 / 延迟退休 / present credit；不变 ⇒ server 内容路径，run-ahead 整条线出局）；E2 `PRESENT_CREDIT` 1/3 扫描；E3 两臂 caps 日志比对（`CapsMirror` placeholder 警告、`GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`）。排除 rd12 与 create-indirect（OQ-17）。若 E0–E3 未定位：E4 重打包计数诊断补丁（`ResolveWireUniformBufferPayload` 逐帧 direct/repack/`EndRenderPass` 次数，设备 vs lavapipe）→ E5 `FORCE_REPACK=1` 主机强制复现（成立则后续全在桌面做）→ E6 RenderDoc。
- **P6.5 残余 39 例 device golden 矩阵**（同跑器同窗口）。
- **CTS `$BASE`**：monolith × DirectVulkan 的五个具名块——基线必须先于迁移落地。

### wave 2 — 并行实现（按**文件**分簇，不按项，避免合并冲突；以 wave 0 车道为门）
- **A 对齐簇**（`UniformManager.cpp`）：`.2/.3/.4/.5/.6/.7`。3–4 天。
- **B blit/copy/mip 簇**（`WireFramebuffer.inc`、`VulkanRenderer.cpp`）：`.8/.11/.12/.13` + 烘焙 (A)(D) + 两处 AllocatorDebtScope。5–6 天，最长，先派。
- **C 顶点 + 对象簇**（`VertexInputStateFactory`、`WireDraw.inc`、`VkBufferManager`、`Init.cpp`、`MagmaProgramSource.h`）：`.9/.10`、Magma `StateObjectDeathOps`、OQ-10 接线、OQ-8 归档 storageBlocks。4–5 天。
- **D Espryt 流**（`MG_Backend/DirectGLES`、`MG_State`、`MG_Impl/Pipe`、`MG_IntegrationTest`——与 A/B/C 零文件重叠）：XFB 两洞、readback 收口、TextureViewAlias + TUS 升门、四个 resolver 启动期 stop、memo 纯度 grep、R-5、raw-depth 步骤 1+2、纹理/program 场景普查。8–10 天。
- **E 文档 / 裁定**（~1 天）：已落地项的文档更正、OQ-6/13 关闭、`§7.2` 同名重跑、`TextureParamsWithoutASamplerView` 待核、D18 读法。
- **F Ph 小件**（`MG_Remote/Transport`、`Server/ServerMain.cpp`、`Handshake.h`——与 A–D 零重叠）：常量时间令牌合一 + 名字入词表 + 最小长度、D11 五处上限 + PH-2 提升、PH-8 单测。3–4 天。
集成者串行合并，每次合并后重跑 Magma 三臂 + 棘轮 + G1，**任何让棘轮上升的提交拒收**。每包 red-once（R-16）。

### wave 3 — 集成者
verify+split 前置债（`PipeRespecifyScope` 放宽）→ `integration-verify-split` → 主机全门（unit / integration-gpu 双传输 / split 三臂 × 双后端 / retrace 全矩阵 / G1 / G2 / G14 / 双生成器 / include 闭包 / doc citation）→ 棘轮逐符号归属 → Iris trace 普查 + P9 例外表 → `§7.2` 同名比对 → **按 wave 0 的分母评估 40% 检查点**：若完成子系统 < 40%，停下重定基线。

### wave 4 — 设备窗口 #2 + CTS
出口门 3 复测到 1.0（同 case 三遍 + monolith 对照同会话）→ CTS AFTER 读数（五块，inproc × DirectVulkan）→ **同窗口取 P3b/P4b 的 CTS `$BASE`** 与 OQ-14 memo 命中率读数，免第三次设备窗口。

### wave 5 — P7 收官后
P3b/P4b 余项（场景普查后半、CTS AFTER）→ Ph 大件（PH-1 (3)(4)、PH-6、PH-7 (4)(5)、fuzz 三臂）→ P12。每阶段收官一次异模型审查（ID-66）。

## 4. P7 子系统分母（40% 检查点用）

1. Magma 两进程车道（三臂 × 73 + 全量过滤器）；2. `@P7` 必须簇六项退役；3. `StateObjectDeathOps`；4. 内部 shader 烘焙 (A)(D)；5. OQ-8 反射归档；6. OQ-10 resident 接线；7. verify×split；8. 真机 ssim 1.0；9. CTS 五块 ≤0.5 pp。**中点** = wave 2 三簇全部合并之时；届时完成 < 4/9 即触发重定基线。

## 5. 裁定（ID-P7-*，正文见 `INTEGRATOR-DECISIONS-P7.md`）

- **ID-P7-1** PH-1 按四片重定界；98 站点的字面翻转拒做，引用返回的 9 处标 `@Ph-declined` 并写明理由。
- **ID-P7-2** PH-6 取 drop-with-latch 形状，不等 PH-1。
- **ID-P7-3** OQ-21：**令牌足够**——常量时间比较、≥16 字节、无令牌只 loopback、数据面以 `Welcome.dataNonce` 绑定到已认证的控制连接；**不做 TLS**（局域网 GL 命令流不含凭据；TLS 归 P12 作为开放项保留，理由：加密不是 Ph 门的威胁模型——威胁是「任何人都能连的远程代码路径」，令牌 + 绑定已关）。
- **ID-P7-4** 出口门 3 的分母排除 `minecraft-1.21.4-rd12-odinlite-in-world` 与 `create-indirect`（OQ-17 dev 侧 Adreno 崩溃）；`iterationrp` 带 apk.yml 的三个 Magma 旋钮。
- **ID-P7-5** `SetupDrawSnapshot` dirty mask 与 `VertexInputStateFactory` 内容寻址 CSO **本阶段拒做**（monolith 路径、无性能门、动 pull `.text`），记入优化阶段。
- **ID-P7-6** D18 取 21-memo 普查（`notes/recovered/wf2/final/part2.md:526`）的读法：节点稳定容器纪律；负控 C 必须仍能到达已发货臂；写进 CONTRACT-P7 一句话即闭。
- **ID-P7-7** 184 棘轮归 P7 wave 0；baseline 存符号**列表**，`--assert-monotone` 只对新增红，消失只提示重基线。
- **ID-P7-8** 实现方 `opus`、审查方 `fable`（本环境可用的两条模型线），以满足「实现方与审查方错开」；每包合并前一次聚焦审查，阶段收官一次整体审查。
- **ID-P7-9** 性能只记录不设门（沿用 2026-09-08 用户裁定）；wave 2-B 若测得 CSO 缓存是热点，才做 P7-5 的 server 半边。

## 6. 纪律（每个提交）

沿用 ROADMAP「通用纪律」：ALL target 完整构建（**含 pull 构建的测试**——b95f5f1a 的 `build-linux` 因 `PipeStatsTest.cpp:137` 编不过，已修）；G1 pull `.text` 恒 `0xa52203`、符号 0/0/0/0（基线 `~/w7/p7-before/`）；G2/G14 名集合只增不删（split 4155 / linux 3049）；每门 red-once；性能只记录；拆分不顺手修 `dev`；先 commit+push 再进下一波（用户 2026-09-11 常规）。
