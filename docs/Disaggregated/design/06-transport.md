# 传输与数据面（原 ARCHITECTURE §11）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 11. 传输与数据面（`MobileGL/MG_Remote/`）

### 11.1 段

| 段 | 拥有者 | 默认 | 内容 |
|---|---|---|---|
| `SEG_CMD` | client（server 只读） | 8 MiB | `RingControl` 页 + POD 记录 + 小内联负载 |
| `SEG_STAGE` | client | 32 MiB | bulk 字节：buffer / 纹理 subdata、UBO、client 数组、persistent-map 脏块 |
| `SEG_REPLY` | server | 16 MiB，8 × 2 MiB slot | readback 像素、acceptance 答案（ID-47） |
| `SEG_EVENT` | server | 256 KiB SPSC ring | 回调事件 |
| `SEG_SHADOW` / `SEG_ADOPT` | client / server | — | 零拷贝 shadow（Phase 2）/ ≥16 MiB 采纳（P11）；stream 数据面上具名拒绝 |

段在同机上由 `ShmSegment` 创建（Android `ASharedMemory`、Linux `memfd`、Windows `CreateFileMappingW`），描述符经 `SCM_RIGHTS` 传递；stream 数据面上两端各持一份同尺寸的私有段（§11.9）。

### 11.2 `RingControl`（`Ring.h`）

一页 4 KiB，每个争用组一条 cache line，按写者分组（P6 lk）：producer 线 `cmdHead` / `submittedSeq`；consumer 线两个 tail；四个 consumer 写的水位收成 `Progress`（`appliedSeq` / `retiredSeq` / `completedFrameSerial` / `presentAckSerial`）；最后是 epoch、generation、park 标志与 `eventRingFull` / `eventDropped`。游标是单调字节计数、2 的幂掩码、永不重置；pad 记录处理 wrap；不可能的头 → `Fatal{ProtocolCorruption}`。

### 11.3 双向 doorbell（`Doorbell.h`）

两个方向都是"先自旋、再置 parked、再阻塞"，发布方仅当对端 parked 时敲；丢失唤醒由两个 `seq_cst` fence 关闭。`CondVarDoorbell`（同进程，带 `Kill()`）与 `SocketDoorbell`（跨进程，对端关闭即死亡检测）。stream 数据面上门铃是本地条件变量，由读线程敲。

### 11.4 控制面（`protocol.fbs`、`Framing.h`、`ITransport.h`）

- 一份 FlatBuffers schema：热路径是 `struct` 直接进 ring（与 POD 逐条 `static_assert` 对齐）；罕见 / 变长 / 需演进的走控制连接的 `CtrlMsg`（`Hello`、`Welcome`、`Refuse`、`CapsSnapshot`、`SurfaceOp/SurfaceReply`、`SessionFault`、`LogLine` …），union tag 只追加，控制修订号钉在 `sha256(protocol.fbs)` 上。
- 生成物提交进树，CI `flatc-check` 重生成并 diff；codegen 不进默认构建图。
- 封帧 `[u32 'MGLF'][u32 len][payload]`，64 MiB 上限，读时校验，坏帧立即闩住失败。

### 11.5 WAR 危害、拷贝账与背压

Phase 1（今天）：GL 调用时刻把字节拷进 stage slot，slot 到 apply 越过它为止不可变，危害按构造消除，代价一次 memcpy；Phase 2（`SEG_SHADOW` 零拷贝）未做。server 没有第二份 `BufferObject`，不存在中间拷贝。分配失败升级：扩容 → 对最老未退休批次有界等待 → 硬 drain + generation bump（之后 tracker 全部重推）。

### 11.6 publish、序号与 credit

- 每条记录 release-store `cmdHead`，仅当 consumer parked 时敲门铃；显式门铃点：`present`、`kNeedsAck`、`eglMakeCurrent`、`glFlush`、stage 余量不足、轮询类入口。
- seq = 记录序数；字节 credit 与 present credit 两个独立窗口。
- **等待规则由 `WaitClass` 列决定（P5e）**：`kWaitReply` / `kWaitPresent` / `kWaitApplied` / `kWaitNone`；run-ahead 武装时 `kWaitNone` 发布即返回。barriered 谓词（`MGPipeBarriered`）client 与 server 用同一个函数、同一份数据算。
- 强制等待点：`MapBuffer(READ)` / `GetBufferSubData` 源走 `SyncGpuWrites`；`glFinish` = 等 `appliedSeq` 追上再排空反向通道；每个 `Server*` EGL forwarder 之前等一次。`glGetError` 最多晚一个 present credit。

### 11.7 事件回传与溢出

client 在 `glGetError`、query / sync 查询、`eglSwapBuffers`、buffer 回读与每轮等待循环中排空 `SEG_EVENT`。日志有损；语义事件无损：环满时 server 置 `eventRingFull`、在记录边界停 apply、敲 client，client 排空后清标志并敲回（两半都在，才不死锁）。run-ahead 下 producer 阻塞等排空，整笔预算 `MOBILEGL_IPC_EVENT_WAIT_MS`；client 不排空 / 已走 / 会话停止 → `ReverseChannelForfeit{…}` 弃投并闩住（Ph PH-6），而不是 `Fatal` 打掉 server。不发布 `kCapRunAheadApply` 的 server（lockstep 臂）保留 P5c 的 Fatal。

### 11.8 fence 与无 present 负载（P10）

fence 完成度必须来自真的逐 fence 退休，不是 present 水位（DirectGLES 的 `g_completedFrameSerial` 今天只在 `Present` 前进）；无 present 循环需要 server 插非 present fence tick。Magma 那半已随 run-ahead 落地。

### 11.9 传输栈的两根轴（P6.5）

终局（2026-09-22）：client 与 server 可在不同机器、不同 OS / 架构上经 TCP 连接，同机 `spawn` 保留。传输栈拆成**两根独立可选、握手协商、可混搭**的轴，上层只见两个接口：

| 轴 | 接口 | 实现 | 选项（`MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA`） |
|---|---|---|---|
| 控制面 | `ITransport`（只承载控制帧，不承载描述符） | `SocketTransport` | `fork`（继承 fd）、`unix:<path>`、`tcp://host:port` |
| 数据面 | `ILink`（记录预留 / 提交、水位、reply、event、span 解析） | `ShmLink`（共享段 + `RingControl` + 门铃）、`StreamLink`（分块封帧，水位与 reply 变消息，发送窗口） | `shm`、`stream`、`auto` |

- 配对在会话建立时选定一次；`LinkTerms`（数据面、窗口尺寸、`maxReplyBytes` …）由 **server 陈述**，client 请求可被钳制。
- 共享段交付与控制面解耦：`ShmLink` 自带 AF_UNIX aux 汇合名，在 `Welcome` 里公布，所以同机"TCP 控制 + 共享段数据"是合法配对。`auto` 的判据是**段实际交付成功**，回落到 `stream` 必须具名进日志。
- **上层零链路种类分支**：`ShmLink` / `StreamLink` / `RingControl` 等只出现在 `Transport/` 与其测试（纯度 grep 门）；角色谓词回答"谁在哪个进程"，不回答"用什么链路"。
- 两端是不同二进制：`wireFingerprint`（`PipeFields.def` 派生的逐成员布局摘要 + 目录摘要 + 字节序 / 指针宽度）永远比较；`buildFingerprint` 只在 `Dial == Fork`（或 `MOBILEGL_IPC_REQUIRE_SAME_BUILD=1`）下比较；不一致是 `Refuse`，不是 abort。
- TCP 上每条 `kWaitReply` 就是一个 RTT、`SEG_STAGE` 字节对的是 30–100 MB/s 的链路：这两列从"记录项"变成可行性数（开放问题 19、20）。

第一波（全 TCP 控制 + stream 数据）已落地；同机混搭（nd `auto` / sd）是第二波。细节 [`notes/p65/README.md`](../notes/p65/README.md)，契约 `MobileGL/MG_Remote/CONTRACT-P65.md`。
