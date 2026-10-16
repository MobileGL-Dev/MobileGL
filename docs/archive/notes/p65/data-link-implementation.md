# P6.5 数据面实现记录

## 已落的边界

`ILink` 工厂创建共享段或 stream 链路。`ShmLink` 持有段、command/event 环端点；`StreamLink` 持有独立私有镜像及专用 reader。session 只缓存链路借出的 endpoint 指针；Reserve/Pop 与 spin 内的原子读取仍非虚。生产 encoder 保存 `LinkProgress` / `LinkSignals`，decoder 与 writeback 的 span 解析经 `ILink::ResolveSpan`。

共享车道不装 stream publication hook；共享段 reply 的 `ReadView` 借出已经发布的 payload，由 client 只拷一次。两种 link 均经过 `ILink::PostReply/ReadReply`。旧 raw-ring constructor 与 `Session::Control/Shm` accessor 仍供既有 white-box 测试及共享段 bootstrap 使用；这些兼容点未被冒充成字面 grep 的零命中。

纯度入口：`python3 scripts/ci/link_seam_purity.py --self-test`。检查物理段/环/slot 池不得回到 session 所有权、上层不得实例化具体 link、apply loop 不得直接读 RingControl、codec 不得持有 RingControl、writeback 不得绕开 link；七个具名负控随脚本执行。cached endpoint 类型与旧测试 constructor 是明确保留的兼容边界。

## 数据帧

共用 `Framing.h::FrameReader`；默认 MGLF，data reader 指定 MGLD。长度为 20 字节 data envelope 加 payload，最大 64 MiB；envelope 是 kind:u8、reserved:u8[3]、a:u64、b:u64。所有多字节 data 字段按小端编码。Stage/Cmd/Event 的单帧 payload 上限因而为 64 MiB − 20；64 MiB Stage burst 会拆两帧。

reader 先验证 framing header 与 data envelope 的角色、长度和窗口，再接收 payload；一条专用 reader 始终排空连接，writer 在调用线程串行发送完整帧。没有无界消息队列。

Progress 除四个 seq/frame 水位，还携带 **cmdAppliedTail 与 cmdRetiredTail**。原 roadmap 简写只有 applied tail，但现有 `RingProducer::FreeBytes` 只读 retired tail；遗漏会在首次回绕挂死，混成 applied tail 则提前释放 borrowed slot。stage 的三个旧 ring cursor 保持零；原 encoder allocator 保持退休 mark/rebase 规则，链路用 `NoteStage` 记录新字节并先于 Cmd 发送。

单槽 reply 接受按 seq 前进的最新回复：`wantReply=false` 的 ResourceSubData 仍回复时不会堵住 reader。同步调用只在自身 applied 后读取，Reply 帧严格先于该 applied 进度发送。若 applied 已到而 reply 不存在，立即返回 protocol mismatch；EOF/错误在同一 reply mutex 下唤醒等待者。stream 不读 shared reply slot，不做取模或 stamp 自检。

## 发布与跨连接顺序

每次 producer park / stage retirement wait 前 Flush；server apply 的真正 park 点（`ServerLoop`）及 transport 的 `SessionConsumer::WaitForWork` 都 FlushProgress。另在 PostReply、present/completed-frame 变化和每 64 条或 1 ms 发布。水位只晚不早、比较用 >=、倒退具名 Fatal。

event bytes 先于宣布其完成的 Progress；client drain 返回 event tail，server 只在真实 tail 前进时清除 full latch。控制连接的 SurfaceReply 不能替数据连接保证先后，因此它携 eventHead fence；client 等本地数据 reader 收到该 head 后再排空事件。RPC 前也冲刷并等此前 command 完成，等待过程中继续排空反向事件。

## 测试与证据边界

`StreamLinkTest` 覆盖 Stage-before-Cmd、idle 尾水位、160 次记录/多次回绕、无用 reply 后的同步 reply、event credit、EOF、等待中的 reply 被 EOF 释放、缺失 reply 拒绝、独立连接的 event fence、Progress 倒退与 Stage 越窗。另以真实 IPv4 loopback TCP 传送 64 MiB Stage 并记录耗时/MiB/s；socketpair correctness 结果不充作 TCP 吞吐。

2026-09-22 开发中 build4 曾产生 server `NoteStage` fatal。只读 objdump 证明是编辑 virtual 接口与同轮并行构建交叠：旧 `Ring.cpp.o` 的 NotifyClient 跳 vptr+0xc0，新 StreamLink 此 slot 已为 NoteStage，ProgressChanged 已挪一格。该产物不作验收证据；接口冻结后重建所有受头影响的 object 才可验证。

## 最后一次退休进度的 red-once

核心 `StreamPair.StagingArrivesBeforeItsCommandAndLastRecordProgressFlushesOnIdle` 在稳定产物上曾真红：收到 applied=1 后 retired 仍为 0。原因不是网络可见性，而是 receiver 先调用 `AdvanceRetired`；此 helper 会按本地 applied（仍为旧值）夹紧退休值。若此后再无 apply，最后一次退休永远丢失，大 stage 窗口回收可因此互等。

修复是按依赖顺序先应用 incoming applied，再应用 incoming retired，随后通知。测试分别等待两种语义，不假设 applied 是 retired 的同步屏障；它只有一次 idle progress，不能靠后续批次掩盖这个错误。

WSL 真 TCP 64 MiB stage 记录（该退休修复前、传输字节已核对的测量）：0.069763 秒，917.397 MiB/s。它只代表本机 loopback 的框架与复制吞吐，不代替 Redmi/Wi-Fi 测量。

## Present 与 glFlush 的提交边界

阶段审查的真实负控发现：本地 `PublishAndNotify` 不是 stream delivery；run-ahead Present 可以在没有任何后续 wait 的情况下返回，最后一个 present 因此永远留在 client。credit=3 的前三帧尤其能暴露它。

修复把传输 Flush 放在每次 Present publish 后，并给 exported `glFlush` 接入只提交、不等待 applied 的 client 路径。run-ahead、present credit 和一般记录的批处理均保留；纯 pull 的 glFlush 经预处理仍是原空函数。

`StreamClientPublicationTest` 用真实 CapsSnapshot 启动 run-ahead ClientSession 和 StreamLink reader，peer 故意不 apply。它断言前三个 Present 各自到达 peer、以及 exported glFlush 发送之前发布的 MemoryBarrier，同时 client waits=0、applied=0。测试没有调用 client wait、事件 drain 或日志同步来暗中冲刷；五秒 alarm 会抓住误加同步等待的回归。

## 挂断发生在 staging 时的取消

设备日志曾记录真实顺序：DrawVbo 等待已报告 DEVICE LOST，随后大 BufferSubData 仍进入 stage allocator，并以 `RetirementWaitFailed` 终止 client。现有 codec 与 emitter 没有传播这个已确认的会话取消。

取消现在是 encoder 本地状态，借用 session 的 lost atomic，并只在 staging/失败等待边界询问生产 doorbell。只有 `PeerHungUp` 会触发 session latch；普通 Dead 可取消 teardown，但不伪造设备丢失。`StageAllocate` 返回 null 时，`StageBytes` 不执行 memcpy 或 NoteStage；session/encoder 在 EncodeRecord 前拒绝取消请求，不产生新 seq 或无效 blobref。stage 的生产等待预算对齐 120 s barrier，给真实 TCP keepalive 检测留下余量；健康 peer 超时仍保持具名 Fatal，不把 deadline 解释成 loss。

没有发出记录时 seq=0；四个 mailbox emitter 保留调用者已 minted 的非零本地 ticket，避免 ReplyToNoSlot。ResourceSubData 的 buffer 快路径也保留取消的 DECLINED，不再在 wire emitter 或 route 中强制改成 OK。

回归共三条：已断后大于 stage window 的上传不拷贝/不发记录；填满 stage 后在 producer 确认 park 时 SIGKILL 一个真实 reader child，第二个 blob 不覆盖窗口且无新 seq；健康 peer 的 5 ms 测试预算仍以 RetirementWaitFailed 结束，signal 侧确认 device_lost=0。生产预算不受该单测缩短。
