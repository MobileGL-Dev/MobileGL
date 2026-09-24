# 构建布局、开关与计数器（原 ARCHITECTURE §16、附 A、附 B）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 16. 构建布局

```
MobileGL/MG_Pipe/            永远进构建：*.def 目录与表、MGPipe*.h payload / 句柄 / 回调、PipeApply、PipeRoute、generated/*.inc
MobileGL/MG_Impl/Pipe/       Tracker、PipeFill、SlotAllocator、CsoCache、CompositeResolver、ResourceTracker、SetHashSuppressor、*Emit.h
MobileGL/MG_Backend/MGPipe/  PipeInputs.{h,cpp}
MobileGL/MG_Remote/          仅 MOBILEGL_BUILD_DISAGGREGATED：CONTRACT-*.md、CapsCodec、FatalFamilies.def / FatalFunnel、Handshake.h
  Protocol/   protocol.fbs、generated/、SurfaceOpCodec、mg_protocol_base.h
  Transport/  ITransport、SocketTransport、InProcessTransport、ILink、ShmLink、StreamLink、Ring、SessionRings、ReplySlot、EventRing、
              Doorbell、ShmSegment(+Posix/Win32)、FdPassing、Framing、AuthToken、ControlInbox、LinkMetrics、WireLog
  Wire/       PipeWireCodec（记录编解码、WireVerbSink）
  Client/     BackendObject_Remote、ClientSession、EmitTables、WireTables、CapsMirror、PersistentMapTracker、GpuWritePending
  Server/     PipeApplier、ServerLoop、ServerSession、ServerMain / ServerEntry / ServerSpawn、PreAuthGate、StagedShadow、StagedTextureStore、
              SurfaceControlFrame、ServerDisplay、InProcessServer、DisplayServerJni
```

- `MOBILEGL_BUILD_DISAGGREGATED`（默认 OFF）追加 `MG_Remote/**`；OFF 时 `MG_Config::Transport` 是 `constexpr Monolith`。`MOBILEGL_BUILD_DISAGGREGATED_INPROC` 隐含前者并加角色隔离：两个角色靠 apply 线程与控制邮箱分开，而不是给 1494 个 `pGLContext->` 读点加 TLS。
- `MOBILEGL_TRANSPORT = monolith | inproc | spawn` 是**拓扑**（G1 相关）；控制面与数据面由 §11.9 的两个变量选。**split build 不设 `MOBILEGL_TRANSPORT` 时是 monolith 对照臂**。
- 四个构建 flavour：pull（默认）、push、verify、split；Android 三份 APK flavour（pull / push / split）。
- 测试接线：ctest `ENVIRONMENT` 用 `mgl_itest_join_environment(...)` 构造；每条 split / spawn / tcp 条目带独立日志路径，日志按角色分文件（`<base>.client.log` / `<base>.server.log`）；spawn、tcp 车道与 split 名集合一致（`scripts/ci/spawn_lane_parity.py`），每条目记 arm 证明。
- CI（`.github/workflows/test.yml`、`apk.yml`）：`pipe-gates`（G1–G8、生成器 self-test、符号报告、dirty-surface、字段归属、G5、文档引用 lint）、`flatc-check`、include 闭包、各 flavour 的 build / integration / retrace 车道、APK + AVD。两份 workflow 里的 `feat/disaggregated` 触发器是临时的，合入 `dev` 前移除。

## 附 A：开关

CMake：`MOBILEGL_BUILD_DISAGGREGATED`（OFF）、`MOBILEGL_BUILD_DISAGGREGATED_INPROC`（OFF，隐含前者）、`MOBILEGL_PIPE_PUSH`（OFF；push / verify / split flavour 打开）、`MOBILEGL_PIPE_VERIFY`（OFF；隐含 push，永不出货）、`MOBILEGL_PIPE_LEGACY_MEMOS`（ON）、`MOBILEGL_BUILD_SERVER_SPIKE`（仅 Android spike）、`MOBILEGL_FLATC_EXECUTABLE`（只服务 `flatc-check`）。

运行时，MGPipe（`MobileGL/Config.h`、`ConfigLoader.cpp`）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | pull `0`；push `0x1fff` | 子系统位图（`MG_Pipe/MGPipe.h`，位永不复用；依赖两侧都拒）；位 63 是"关 CSO 内容寻址"的行为对照 |
| `MOBILEGL_PIPE_VERIFY` / `_VERIFY_FATAL` / `_VERIFY_CORRUPT` / `_POISON_OMIT` | 0 / 1 / 空 / 空 | 影子比对与两个阴性对照（verify 构建才有） |
| `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` | 0 | 故意打掉句柄身份的阴性对照 |
| `MOBILEGL_PIPE_STATS` / `_STATS_PERIOD` / `_STATS_FILE` | 0 / 120 / 空 | 边界计数器（附 B）；dump 按角色写 `<base>.client.json` / `<base>.server.json` |
| `MOBILEGL_PIPE_LEGACY_MEMOS` / `_TEXEL_RETAIN_MB` / `_INDEX_MIRROR_MB` | ON / 0 / 64 | pre-handle 臂、纹理拉取保留、索引镜像预算（P8） |

运行时，传输与 IPC：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | 拓扑：`monolith` / `inproc` / `spawn` |
| `MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA` | `fork` / `auto` | 控制面端点与数据面（§11.9）；`MOBILEGL_IPC_SERVER_PATH` 定位 fork 出的 server |
| `MOBILEGL_IPC_TOKEN` / `MOBILEGL_IPC_REQUIRE_SAME_BUILD` | 空 / 0 | 令牌（≥16 字节；无令牌只允许 loopback）；要求两端同一构建（车道纪律） |
| `MOBILEGL_IPC_PREAUTH_*` / `MOBILEGL_IPC_AUTH_BACKOFF_*` | — | server 预认证的上限与失败退避（Ph） |
| `MOBILEGL_IPC_CONTROL_TIMEOUT_MS` / `MOBILEGL_IPC_COLD_START_MS` | 5000 / 20000 | 控制回复的稳态上限与冷启动预算 |
| `MOBILEGL_IPC_LOG_FORWARD` | 远端时开 | server 日志经控制连接前送，client 写进自己的 `<base>.server.log` |
| `MOBILEGL_IPC_SURFACE` | `offscreen` | `server` = 使用 server 自有窗口，client 无头（P12） |
| `MOBILEGL_IPC_RING_MB` / `MOBILEGL_IPC_STAGE_MB` | 8 / 32 | `SEG_CMD` / `SEG_STAGE`；stage 的 1/4 是内容分块预算 |
| `MOBILEGL_IPC_RUN_AHEAD` | 1 | P5e run-ahead；只是合取式的一半（server 须发布 `kCapRunAheadApply`）；`0` 是 A/B 对照 |
| `MOBILEGL_IPC_VERB_BARRIER` | 1 | `0` 只作阴性对照，且同时关掉 run-ahead |
| `MOBILEGL_IPC_PRESENT_CREDIT` | 1 | 1..8，§14 |
| `MOBILEGL_IPC_BATCH_WAITS` | 1 | lockstep 下值类记录发布即返回；verify 强制 0 |
| `MOBILEGL_IPC_EVENT_WAIT_MS` | 2000 | 反向事件遇满环时 server 等 client 排空的整笔预算（§11.7） |
| `MOBILEGL_IPC_WIRE_DEFERRED_MB` | 64 | Magma wire 臂延迟回收的水位线；`0` 是 M2 阴性对照 |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` / `_PERSISTENT_HASH_SUPPRESS` | 64 / 1 | persistent-map 推送粒度 / 只推变化的块；`0` 是对照 |
| `MOBILEGL_IPC_ADOPT_TIER` | 2 | `auto/0/1/2`；T0 / T1 今天是 Fatal-at-use |
| `MOBILEGL_IPC_STRICT_ERRORS` / `MOBILEGL_IPC_AUDIT` / `MOBILEGL_IPC_ROLE_SPLIT_STATE` | 0 / 0 / 0 | 残余输入读升级为 Fatal / 退休 staging 填 `0xDD` / 双块演练（P5f） |
| `MOBILEGL_IPC_SPIN_US` / `MOBILEGL_IPC_SERVER_AFFINITY` | 50 / `auto` | park 前自旋预算 / apply 线程亲和（Redmi 内核忽略） |
| `MOBILEGL_IPC_RESPAWN` | — | 具名拒绝：没有阶段实现 server 重启后的全量重推 |

显式不设立：`MOBILEGL_IPC_PROGRAM`（没有 relink 档）、`MOBILEGL_IPC_VALIDATE_SERVER`。计划中未接线：`MOBILEGL_IPC_POLL_ESCALATE`（P10）、`MOBILEGL_IPC_IDLE_EXIT_S`（未解析）。

## 附 B：边界计数器（`MobileGL/MG_Util/Metrics/PipeStats.h`）

关闭时每站点一次全局 load + 一条永不命中的分支。字节类（stage buffer / texture / UBO / client 数组、`pmap`、`resid`、`csob-blob`、`seg` = 写进 `SEG_STAGE` 的字节）、调用类（`draws`、`accessor-calls`、纹理上传 emit / box / rect / jobs、`csom` / `csob`、`mpr`、`trp`、`rsp`、`wrec`、`maxrec` / `ringwraps` / `ringpads` / `ringwaits`、门铃 `srv` / `srvpark` / `cli` / `clipark`）与六个 memo 门的 hit/miss。每 `MOBILEGL_PIPE_STATS_PERIOD` 帧一条 `MGPipe stats:` 汇总行，按角色写日志。读法陷阱：`accessor-calls` 是静态下界，判性能只看 CPU 时间；spawn 下 client 不推进帧窗口（`frames=0`），窗口化字段在 spawn client 上无效，run total 仍有效（[`notes/p6/README.md`](../notes/p6/README.md) §12.1）。站点清单（哪些路径**没有**接线）写在 `PipeStats.cpp` 头部，那份清单是契约。
