# P6.5 第一波：TCP 控制与 stream 数据

2026-09-22。依据 `docs/Disaggregated/ROADMAP.md` 的「立即的下一项」。本文件记录实现所需的协议细节；验收结果单独记入阶段报告，不能用本文件代替运行证据。

## 范围与配置

本波实现 POSIX client/server 的全 TCP 连接；主验收形态是 WSL x86_64 client 与 Redmi aarch64 server。保留 inproc 与 fork/UNIX 的共享段路径。Windows client、TCP 控制加共享段、压缩、内容去重、异步 reply、真实窗口均不在本波。

`MOBILEGL_TRANSPORT=spawn` 表示两进程拓扑。`MOBILEGL_IPC_CONTROL` 选择 `fork`（默认）、`unix:<path>` 或 `tcp://host:port`，`MOBILEGL_IPC_DATA=auto|shm|stream` 选择数据面。本波 TCP 只接受 stream，fork/UNIX 只接受 shm；不支持的显式组合具名拒绝。Connect 不启动本地子进程，实际 server pid 来自 Welcome。

TCP 两条连接依次为控制和数据。每条均设置 NODELAY、KEEPALIVE，Linux/Android 另设置 KEEPIDLE=2、KEEPINTVL=1、KEEPCNT=3、USER_TIMEOUT=5000。网络错误和 EOF 才触发 peer-hung-up；apply 超时不是 device loss。无 `MOBILEGL_IPC_TOKEN` 的 listener 仅能绑定 loopback；错令牌返回 Refuse，不打印令牌值。（P7 包 f2-auth，PH-7 (5)）`--serve` 的 TCP supervisor 在 fork 之前自己读每条连接的首帧、先验令牌：未认证对端只得到 `Refuse{Authentication}`，不带 wire 指纹与 build stamp，也不花一次 fork；首帧须在 `MOBILEGL_IPC_PREAUTH_MS`（默认 2000）内到齐；同时待认证的连接至多 `MOBILEGL_IPC_PREAUTH_MAX`（默认 8，上限 256），满时新连接顶掉占位最多的地址最老的一条（`Refuse{Busy}`），自己地址已占最多时才被拒；同一地址失败（错令牌、畸形首帧、超时、静默占位超过 250 ms）达 `MOBILEGL_IPC_AUTH_BACKOFF_AFTER`（默认 5，0 关）次后在 accept 处拒绝，窗口自 `MOBILEGL_IPC_AUTH_BACKOFF_MS`（默认 1000）起逐次翻倍、上限 60 s，一次认证成功清零。这四个旋钮只读环境变量（supervisor 不跑 ConfigLoader）。细节 `MG_Remote/Server/PreAuthGate.h`，记录债见 CONTRACT-P7 §12。

## 握手与布局

Hello/Welcome 追加 `wireFingerprint`、`LinkTerms`，Hello 带 dial 与 token，Welcome 带实际 backend。原 union 标签不重排；Refuse 与 LogFlush 追加。Refuse 是正常的拒绝结果，不能改成 ABI mismatch abort。

wire 指纹覆盖 PipeFields 逐成员名称/offset/size、按序 opcode/payload/flags/WaitClass、记录头、句柄、render-state 字段表、blob codec 版本、协议版本、字节序、指针宽度与 caps/dynamic-parameters 布局。build stamp 单独比较：Fork 必须相同，Connect 不同只警告，`MOBILEGL_IPC_REQUIRE_SAME_BUILD=1` 则拒绝。窗口尺寸由 server 陈述，不进入布局指纹。DynamicBackendParameters 使用定宽整数；没有借机改 GLFunctionsTable ABI。

启动必须收到并采用真实的首个 CapsSnapshot，之后才能锁存 run-ahead 并宣布 session started。
后台控制 reader 尚未把 caps 放入 inbox，不能解释成 server 不支持能力；也不能先用 placeholder
永久锁成 lockstep，再期待后来的快照自动提升。后续快照仍只允许 demotion，缺少首 caps 是有界启动失败。

program archive 在 split 构建使用 v2，头中校验真实序列化 schema 摘要，且版本和摘要进入握手指纹。v1 的宿主对象大小 echo 在 Linux 是 1056，在 Android 是 0，不能用于跨端兼容判定；libstdc++ 与 libc++ 的字符串/容器布局不相同，即使两端都是 LP64。payload 按字段序列化，schema 递归覆盖字段名、次序和类型，不包含容器宿主 sizeof/offsetof。宿主大小编译绊线保留为本地维护检查；非 split 路径保留 v1，G1 不因此让步。

## 数据面与生命周期

ILink 拥有数据面存储与端点。ShmLink 保留映射、reply slot pool 和现有 ring 实现；StreamLink 拥有私有镜像。session 只缓存端点/进度指针；reserve/pop 仍使用原有非虚 ring 操作。测试兼容的原始 ring 构造入口和启动时的 descriptor 交付不作为生产链路选择入口。

数据帧含 MGLD magic、长度、kind、两个 64-bit 字段及 payload。Stage 在引用它的 Cmd 之前发送；Cmd/Event 按逻辑 cursor 严格顺序接收并检查容量。接收线程先写数据，再 release 发布 head/水位。每侧数据连接有一个独立 reader；发送在调用线程串行进行，不引入无界发送队列。

进度保留单调、`>=`、只能晚不能早的规则。Progress 必须包含 **cmdRetiredTail**：FreeBytes 依赖退役尾，只有 applied tail 不能回收 borrowed record，也不能保证环回绕。event credit 以 ClientProgress 返回。越窗、非法方向、倒退水位是 ProtocolCorruption。

server 在 park 前、Reply 之后、present/completion 变化时冲刷进度，并按 64 条记录/1 ms 合并普通进度。client 每个等待点先 Flush。Stream reply 使用 seq 标识的消息邮箱；已经观察到 applied 却没有相应 reply 必须拒绝，不能永久等待。ShmLink 保留旧 slot 语义。

Present 与 `glFlush` 也是 client 的发送边界：将已经发布的批次交给对端，保持 run-ahead，不新增 apply 等待。单靠下一次 credit wait 冲刷，会让最后一帧在 client 随后空闲时永久留在本地，credit=3 时还会把前三帧积在发送端。

已经丢失连接或在 stage/cmd 空间等待中观察到真实挂断时，编码器取消本次及后续发射：不复制新 blob、不增加 wire seq，返回 DECLINED，并保留本地 reply ticket 的合法性。只因超时不能闩 device loss；stage 空间等待使用与 barrier 一致的 120 秒上限，让传输层有时间报告半开连接的失败。

两条 TCP 连接之间没有到达顺序保证。因此 surface RPC 前先等待此前提交的命令；SurfaceReply 另携带 **eventHead**。server 冲刷事件后发回复，client 等数据 reader 收到该 head 后才完成 RPC。仅按两个 send 的先后推断 SurfaceChanged 已到达是错误的。

## 控制、日志和进程

client 的专用控制 reader 直接落盘 LogLine，其他控制消息进入有界 inbox，避免 GL 线程等数据时 server 因日志 socket 写满而停住。server 同时写本地日志并前送；client 写自己的 `<base>.server.log`。`MGPipeSyncPeerLog()` 先同步命令，再以 LogFlush/ack 对齐日志观察点。正常的无远端情形是 no-op，损坏的 ack 不能静默成功。

supervisor 接受连接后 fork，每会话独立进程，父进程不创建 backend/EGL；同时只允许一会话，第二个返回 Busy。父进程关闭自己的 socket 副本时 **不调用 shutdown**，否则会关闭 child 仍使用的连接。后端由 Hello 请求，显式 server 环境钉住的后端不匹配则拒绝。server 子进程保留 P6 的 PipeStats Init/Shutdown。

正常 client 结束请求时只先关闭控制 socket 的写半边，等 server 完成 backend 销毁、最终日志和进程退出后的 EOF，再完成本地关闭。否则连续的独立 client 会抢到前一会话仍在清理的时间窗。Busy 拒绝在读取 Hello 后发送，避免带未读请求直接 close 引发 TCP RST，丢掉已经排队的 Refuse。

trace flavour 的前台 MobileGLServerService 从 nativeLibraryDir 启动可执行 server，设置服务端角色与反递归标志，持 wake lock。`am force-stop` 终止该包的服务进程。

## 带宽和测量口径

第一波不启用压缩或内容去重。P6 gate 8 的 rd12 stage 中位/均值是 816,248 / 1,534,555 B/帧，60 fps 对应约 49 / 92 MB/s；OpenRA 短样本均值明显受加载尖峰影响。该数据已经说明 Wi-Fi 可能成为瓶颈，但没有压缩率、额外 CPU 或纹理重复率证据，不能据此选定压缩算法，也不能宣称已满足 60 fps。

`MOBILEGL_PIPE_STATS=1` 下，P65LinkMetrics 记录逐帧 reply 次数、RTT 固定桶、stage bytes、wall time 与 client thread CPU。p50/p99 为桶上界；回复的测量包含必要的发送、排队与 apply，不能冒充空载网络 ping。Stage 字节/帧除以 wall time 是负载流量；链路吞吐另用真实 TCP 64 MiB 突发测量。与 PipeStats 的同源字节计数是独立观测，不能相加。

## 验收中的两处澄清

- 车道 parity 比较 DirectGLES 的可比用例集合，而不是不同标签下的总条数；当前基线是 102 条。TCP 每条还需真实 `control=tcp data=stream server=... pid=N` 证明与本地零子进程断言。fixture 不算用例通过数，禁止用 EGL 启动失败的 SKIP 代替通过。
- lf 文档列出的四个 arming 场景中，UnlocatedIoBlock、PrimitivesGeneratedNoXfb、PointSizeDemotion 的 marker 来自 server；关闭日志前送应让对应断言变红。PipeVerifyArming 的 marker 来自 **client PipeFill**，关闭 server 前送本来就不应使它变红；它应继续通过，作为角色分离对照。不得伪造一个 server marker 来满足错误的负控预期。
