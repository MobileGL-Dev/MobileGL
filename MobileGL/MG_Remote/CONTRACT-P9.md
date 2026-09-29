# P9：反向通道

2026-09-29。依据 `docs/Disaggregated/notes/p9/`（交接 [`HANDOFF-P9.md`](../../docs/Disaggregated/notes/p9/HANDOFF-P9.md)、裁定 `INTEGRATOR-DECISIONS-P9.md`）。本文只记实现所需的协议与行为；验证结果以 notes 里的运行证据为准。

## 范围

server → client 的结果回传在 split 下不阻塞、不丢、不乱序。本阶段落地：PACK-PBO 回读不等回复（§1）；server 日志按级别转发、不阻塞（§2）；`OnGlError` 的观察时点写成契约（§3）；两条故障注入（§4）；纹理重铸不需要拉取（§5）。事件量的批处理 / 收窄没有实测需求，不做（ID-P9-7）。路线图原文里已由前序阶段做完的（异步 reply、2 MiB 回复槽与分带回读、XFB 删除）不重做，见交接 §2。

## 1. 读进 pack buffer 的回读（W1）

### 线上格式

- 追加两个 op（只追加，序号 82、83）：`ReadPixelsToBuffer`（kCtxVerb，verb = ReadPixels）、`GetTextureImageToBuffer`（kCtxObject，verb = GetTextureImage）。标志 `kNone`（无回复槽、无 blob、无尾），等待类 `kWaitNone`。
- 载荷 `MGPReadbackToBuffer`（104 字节）：`Src` 是回复形式的 `MGPReadbackInfo` 原样（`Src.DstOffset = 0`、`Src.DstSize` = 紧排 w·h·d·bpp，只校验不定位）；`Dst` 是 pack buffer 的资源句柄；`DstBase` = 应用偏移 + `GL_PACK_SKIP_{PIXELS,ROWS,IMAGES}`；`RowStride`（行长与对齐已算入）；`ImageStride`（只在 `Src.Box.D > 1` 时读）；`SwapGroup` = `GL_PACK_SWAP_BYTES` 要反转的字节组宽（2/4/8，0 = 不换）。布局算术只在 client（`ReadbackLayout`）。

### client

- 目的是 pack buffer 且 `PackBufferReadbackTarget` 给出句柄时走新形式：先 `MarkReadPixelsPackBuffer()`（`GpuWritePending` 第 4 行，把 buffer 标成 GPU 写过），再发记录，**不等**。`glReadPixels` 仍按回复槽上限分带（为了 server 的 scratch 有界，不是为了线路）；`glGetTexImage` 整级一条。
- 不走新形式（回到回复形式：等回复、client 自己把行 `UploadSubData` 进 buffer）的五种情况：`MOBILEGL_IPC_PBO_READBACK_SYNC=1`；buffer 没有回读路径（`BufferWritebackIsReachable` 为假）；buffer **已映射**（只有持久映射能跨读存活，它的读者直接读指针，永远不会触发同步）；范围超出 `MGPSubData` 能表达的 buffer 偏移（2³¹−1）；buffer 还没上过线（无句柄）。
- 此后对该 buffer 的任何 CPU 读（`glGetBufferSubData`、`glMapBufferRange`、以它为 UNPACK 源的纹理上传）经 `SyncGpuWrites` → `resource_readback` → `OnBufferWriteback` 取回字节。**UNPACK 源读取在 split 下先 `SyncGpuWrites`**（`GL_Texture.cpp` 的 `MGL_SYNC_PIXEL_UNPACK_SOURCE`，pull 构建展开为空）。
- monolith 不变：后端在自己的 `ReadPixels` 里映射驱动 PBO 回影子。

### server

- 读取与回复形式逐字节相同（`ReadBoundFramebufferTight`：中性 PACK 读进 scratch；`ReadTextureImageTight`：两后端各自的 `ReadTextureImageWire`）；`ReadPixelsToBuffer` 的紧排大小仍受本端 `maxReplyBytes` 约束（PH-3）。
- 落地走 applier 的 buffer 写入口：按 `SwapGroup` 反转后，每段连续区间合成一条 buffer 半边的 `MGPSubData`（`HasLiveHostWrites = 0`）调 `MGPipeApplyResourceSubData`——布局紧排时整读一条、行紧排时每层一条、否则每行一条；行间 padding 不写。于是两后端都与"client 写了同样字节的 `glBufferSubData`"同形：Espryt 进 R-11 暂存影子并排队范围，Magma `WriteWireBuffer` 与在飞读取排序；serial、epoch 与后续记录的顺序都照旧。**不用驱动的 PBO 读**：它只写后端存储，下一次从暂存影子排空的范围会把旧字节盖回去。
- **读之前**先查完全部形状：盒与格式、紧排大小是否溢出、PH-3 上限、`Src.DstOffset/DstSize`、布局（`RowStride` 不小于紧排行、`ImageStride` 不小于行、`SwapGroup` ∈ {0,2,4,8}、最后一行的起点 ≤ 2³¹−1、紧排宽度 ≤ 2³²−1）。任何一条不过 → 每个 sink 唯一的闩锁点 `Fatal{ProtocolCorruption, "ReadPixelsToBuffer.shape"}` / `"GetTextureImageToBuffer.shape"`（原因写在其后）。查过之后落地不会再遇到要拒绝的范围；范围是否落在 buffer 内由 applier 自己的写入口判。
- 读失败是响亮的：无 `GL.ReadPixels` → `Fatal{ReadbackDeclined, "ReadPixelsToBuffer"}`；纹理级读不出 → `Fatal{ReplyError, "GetTextureImageToBuffer"}`。回复形式的这些情况在 client 端以同名 Fatal 结束，新形式没有回复可载，就在 server 端闩。三组闩锁点各有 `PeerLatchTest` 行或不可达论证（`ph_latch_sites.py`）。
- 准入推导：一个 verb 只有在**盖它戳的每个 op**都等待时才算静态屏障（`gen_pipe_field_ownership.py`）；`ReadPixelsToBuffer` 盖 ReadPixels 戳而不等，所以 ReadPixels 不再静态屏障（现有生产表无 BARRIER_PULLED 字段，只影响合成夹具）。
- 计数：`ServerVerbSink::BufferReadbacks()` / `BufferReadbackWrites()`。

## 2. server 日志转发（W3）

- **级别端到端**：`LogForwarder` 为 `void(*)(void* user, int level, const char* line)`（`SetLogForwarder` 与 `SetSessionLogForwarder` 两种），`level` 是 `MOBILEGL_LOG_LEVEL_*`，由 `Log()` 已收到的 Android 优先级映射（`Log()` 签名不变，G1）。线上用 `LogLine.level`（`protocol.fbs` 本来就有，**不改 schema、不动控制修订号**）；未知线上级别按 ERROR 收，不降级。client 写 `<base>.server.log` 时保留级别：server 格式化过的行原样，裸行加 `[peer/LEVEL]: `。
- **不阻塞任何打日志的线程**：server 装的转发器是 `Transport/LogForward` 的队列 + 独立发送线程（`mgl-log-fwd`）；打日志的线程只加锁、拷贝，不碰 socket。

| 级别 | 行为 |
|---|---|
| ≤ WARN | 有损：队列里这类行满 1 MiB 后丢弃并计数；每段缺口在流内补一行 `LogForward{Dropped} - N line(s) …` |
| ERROR | 无损：限速 256 行/s、突发 1024，超速的按序等待；超过 8 MiB 上限时多出的合成一行 ERROR `LogForward{Coalesced} - N …`，不静默消失 |
| FATAL | 最多等 2 s 把本行写出；若上一次 FATAL 等待已超时且其后无任何写出，后续 FATAL 不再等（树里 `MGLOG_F` 也用于可恢复的 EGL 加载消息） |

- `LogFlush` 的 ack 经同一队列排在此前所有已转发行之后（`MGPipeSyncPeerLog` 的承诺不变）。会话每个出口：先清转发器，再最多 2 s 排空，发送线程仍卡在写上就切断控制 socket 让它返回。会话结束时 server 本地记一行计数（`forwarded / dropped(<=WARN) / coalesced(ERROR) / paced(ERROR) / notices / abandoned / fatal-flush-timeouts / transport-failed / max-offer-us`；有损失为 WARN，否则 DEBUG）。
- `MGPipeCallbacks::OnLog` 删除（零生产者、零消费者、无 `EventKind`）。

## 3. `OnGlError` 的观察时点（W5）

保持 P5e 的放宽（`CONTRACT-P5E.md` §"glGetError relaxation"、`CONTRACT-P5C.md` §4.2）：run-ahead 下一个动词产生的 GL 错误在**下一个 drain 点**被 client 观察到，最迟晚一个 present credit；`glGetError` 不触发强制追平。收紧（`glGetError` 前 `WaitForApplyToCatchUp`）会新增同步点，需用户裁定。

## 4. 故障注入

- **F1**：pack buffer 读已发出、server 在落地前死亡，随后 client 读该 buffer。device lost 使 `resource_readback` 被 DECLINED，影子仍是读之前的字节；`AwaitBufferWriteback` 在 `ClientSession::DeviceLost()` 时以 `Fatal{ReadbackDeclined, "buffer-writeback"}` 结束，**不把旧字节当像素返回**（与回复形式的 `ReadbackDeclined` 同名）。门：`RemoteClientControls.APackReadOwedWhenTheDeviceIsLostNeverReadsBackAsTheOldBytes`。

- **F2**：client 停读控制 socket（`ClientSession::PauseControlReaderForTest`），server 以测试旋钮 `MOBILEGL_TEST_APPLY_LOG_FLOOD=<n>` 让 apply 线程每条记录打 n 行 WARN + 1 行 ERROR。apply 线程不被日志卡住（约 16 MiB / 400 条记录 0.4 s 过完，单行交接最长 53–148 µs）；≤ WARN 的丢弃全部有流内说明；≥ ERROR 全部按序到达。控制与 SEG_EVENT 都不排空时，会话经既有 `ReverseChannelForfeit{NotDraining}` 以 exit 0 结束，同一 supervisor 接下一个连接。门：`LogForwardTest`（15 例）、`EventForfeitPeer.StreamLogFlood*`（2 例）。

## 5. 纹理重铸（W2）

- **没有纹理拉取**。重铸（为 image 绑定把纹理升级成可绑定存储）是 server 本地路径，反向通道不含任何"向 client 要纹素"的请求：
  - Espryt（`RequireImageBindableStorageByHandle`）：只读回驱动上**确实存在且尺寸相符**的层（`NativeTextureLevelHasExtent`），pending 盒用 `StagedTextureStore` 合并；驱动没有的层——store 覆盖就重放 store，否则跳过（GL 里其内容本就未定义）；驱动拒绝读回而 store 覆盖该层时退回 store（记一次 `remint-readback-fallback`），两者都没有才 `Fatal`；已 immutable 且无需加宽的核心格式原样保留。
  - Magma：重建带 `STORAGE` 用途的 VkImage，`PreserveTextureContentsOnRecreate` GPU→GPU 拷贝。
- `TextureRemintPulls`（`trp=`）在 server 路径计数（重铸了已持有的存储时；保留路径不计）。
- `MGPipeCallbacks` 删去 `OnTexturePullRequest`；op 50 `ResourceSubDataComplete` 留作退役行（解码仍拒收），`protocol.fbs` 不变。两处 `Fatal{UnmigratedEmulation, "texture-remint-pull"}` 删除（`EveryUnmigratedEmulationIsNamedOnce` 5 → 4）。
- 测试旋钮 `MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE`（server 读）强制读回失败，驱动 fallback 用例。
- 门：`TextureRemintPullScenario`（14 例）× 两后端 × monolith/Split/Spawn/Tcp，外加 Espryt Split/Spawn 的 4 条强制失败条目；monolith Espryt 上 7 个 GPU 写过的用例按名跳过，指向 dev 缺陷（ID-P9-8）。

## 不变量

- G1：pull 构建符号增 0 减 0、`.text` 不变（新代码全在 `MOBILEGL_BUILD_DISAGGREGATED` 下；`GL_Texture.cpp` 的宏在 pull 构建里为空）。
- 目录只追加：`MGP_CALL_LIST_DOCUMENTED_COUNT` 81 → 83，`kMGPipeVerifiedPayloadCount` 82 → 83，stamp 表 32 → 34 行。
- 回调 9 → 7（`OnLog`、`OnTexturePullRequest` 删除；剩 `OnGlError`、`OnGpuWritten`、`OnBufferWriteback`、`OnTextureWriteback`、`OnMipLevelsGenerated`、`OnSurfaceChanged`、`OnCapsInvalidated`，其中后三个仍无生产者，不在本阶段）。
- 控制协议修订号不变（3）；`protocol.fbs` 未改。
