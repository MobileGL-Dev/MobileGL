# P6.5 TCP 车道与设备验收记录

**状态：验收采证中，未宣布 P6.5 收官。** 历史 TCP 主机/设备 102 项功能阳性保留，
但当时实际为 lockstep，不能充作 run-ahead 验收。首 caps race 修复后的 READY7 已取得主机 TCP
及 Redmi 各 **102 PASS / 0 FAIL / 0 SKIP**，每条实际 ARMED；正式 trace 矩阵与同模式性能数仍待补。

当前源码基线为 `origin/feat/disaggregated@e6c51d07`，开发树 `codex/p65-all-tcp` 的
HEAD 为 `52ffc6a7` 加未提交工作。两端曾共享的显式 `MOBILEGL_BUILD_STAMP` 是
`410dfa94c3d92241e9ec01ddbba1bb911eb60d4b`，**它不是当前 HEAD**；后续 rebase 没有改变当时的
生产字节，因此保留了这一制品标识。不能拿匹配的显式 stamp 替代每组产物 SHA、实际 arming
状态和运行证据，也不能把较早制品的绿移记到新的修复头。

## 主机结果：历史功能阳性

环境：WSL Arch，clang/Release，Mesa llvmpipe/lavapipe；源码位于 Windows 工作树，
构建目录 `/home/swung/p65-split`。`integration-split` 的完整标签有 186 条，
其中共享注册宏的 DirectGLES 可比集合为 102 条；不能把 186 与 102 的标签总数直接比较。

| 车道 | PASS | FAIL | SKIP | 证据前缀 |
|---|---:|---:|---:|---|
| TCP loopback | 102 | 0 | 0 | `/home/swung/p65-final-tcp` |
| unix + shm spawn | 102 | 0 | 0 | `/home/swung/p65-final-spawn` |
| 完整 inproc 标签 | 186 | 0 | 0 | `/home/swung/p65-final-inproc` |

这张表保留当时的完整结果。**该次 TCP 102 条后来确认实际是 lockstep**：请求了
run-ahead=1，却在真实首 CapsSnapshot 到达之前用 placeholder 的零能力锁存了回退状态。
旧拓扑 proof 证明了 TCP/stream 与远端进程，不证明 run-ahead 已 ARMED。spawn/inproc 行保留其
原功能结果，不因本表改称 READY7。

各前缀均有 `.log`、`.xml`。TCP 的两项 CTest supervisor fixture 另计，
故 CTest 摘要显示 104 条，而真实渲染用例仍为 102 条。
`p65-final-tcp-lane.json` 是 discovery；`junit_tally.py --require-tcp-ran --discovery ...`
逐条读取私有角色日志，确认 `control=tcp data=stream server=... pid=N`，不是只看测试名。

`spawn_lane_parity.py` 比较规范化后的名字集合，保留现有 monolith-only 例外。
`split_log_paths.py` 同时检查 Split、Spawn、Tcp、TcpDevice 的私有日志路径。
TCP 用例还在客户端 `/proc/self/task/<pid>/children` 断言 Connect 没有偷偷启动本地 server。

### READY7 主机 TCP：真实 ARMED 复验

启动改为等待并采用真实首 CapsSnapshot 后再锁存能力；没有强制打开 run-ahead，也没有放宽
后续只允许 demotion 的规则。READY7 主机结果为 **102 PASS / 0 FAIL / 0 SKIP**，含两个
supervisor fixture 的 CTest 总数为 104，耗时 18.35 秒。

逐条私有 client 日志实查：**102 条有 `run-ahead ARMED`，`running lockstep` / `run-ahead DISARMED`
均为 0，缺日志为 0**。这组才是首 caps 修复后的 run-ahead 功能证据。
证据：`/home/swung/p65-ready7-tcp.{log,xml}`、`p65-ready7-tcp-lane.json` 与其所列角色日志。
完整 retrace 与同模式 TCP/spawn CPU 对照不能由这 102 个用例替代。

### READY7 最终 ALL、unit 与名字门

最终 ALL 增量退出 0；完整 unit 为 **2399 项 = 2389 PASS + 10 既有条件 SKIP，0 FAIL**，
18.96 秒，新增三条 InitialCapsStartup 均通过。证据为 `/home/swung/p65-final-tests-build.log`、
`p65-ready7-unit.{log,xml}`；没有把条件 skip 隐藏成 PASS。

最终 G14 为 **4016 → 4257，0 removed / 241 added**，新增包括设备 102 条。
`--require-device` 名集合门实跑得到 split 105 条中可比 102，等于 spawn 102、TCP 102、device 102。
证据：`/home/swung/p65-ready7-test-name-parity.json`、`p65-ready7-lane-parity.log`。
完整标签、可比集合和 fixture 数仍分别记录，不能混成同一个计数。

必须另记制品身份：最后一次 CMake 重配置因 SPIRV-Cross git-version timestamp
`2026-09-22T08:02:51` 重编 `spirv_cross_c.cpp` 并重新链接，最终 host library SHA-256 为
`25ca45d756bc5a4e5bd56ad4ce231e4ef89d45ba41131f389575cda0842cd6be`。
MobileGL 自有 production objects 未重编，但这依然是**不同制品**；READY7 device 102 与此前
CPU 样本使用原 host library（SHA 前缀 `29fc...`），这些证据保留原身份，不改记成新 SHA。
正式矩阵使用新的 `25ca45d7...` 制品，不能只凭相同显式 build stamp 混用二者。

### 被旧 skip 掩盖的问题

首次 TCP 与 spawn 都是 97 PASS / 5 SKIP。没有将其记成 102 PASS，随后补齐了五条：

- Triangle 的 verb-stamp 与 persistent-map counting 用例原本只在专门车道获得计数标记。
  现在共享宏和 small-ring 的对应条目也设置真实统计环境；persistent-map 的 acquisition/bytes
  属于 client，分进程时取客户端真实计数差，避免读取 server 的结构性零。
- 三条 Ct 用例原本因为读不到 server 内存而直接 skip。现在复用 `LogFlush`：client 先等待命令前缀
  应用完成，server acquire 水位后输出已有 reset/serial/death 计数，client 读取同步后的角色日志。
  仅 `MOBILEGL_TEST_SERVER_COUNTERS=1` 开启此诊断，没有在 draw 路径加输出。
- 删除死亡用例的 skip 后，真地发现 spawn/TCP 的 ObjectDeaths 一直是 0，而 inproc 正常：
  独立 client 没安装死亡通知。`WireTables.cpp` 为它安装复用现有 wire emitter 的回调，
  不覆盖 inproc 已有 dispatcher。修复后五条 × 三臂 **15 PASS / 0 SKIP**，
  证据 `/home/swung/p65-fivecases-fixed.{log,xml}`。

TCP harness 的 fork pre-flight 也曾先占用远端单会话，使真正测试收到 Busy。Connect 下的驱动
初始化已经在 server child 隔离，因此不再另开本地 pre-flight；TCP 车道强制 REQUIRE_GPU，
初始化失败是 FAIL，不能作为“本机没有 GPU”而 skip。

## 日志前送与 CI

`tcp_log_forwarding_control.py` 直接执行原有 arming 场景，分别给 client 和 supervisor 设置真实特性。
`/home/swung/p65-lf-controls2/summary.json` 及六臂角色日志证明：

| 原有场景 | 后端 / 特性 | forwarding ON | forwarding OFF |
|---|---|---|---|
| UnlocatedIoBlock arming | DirectGLES / `MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS=1` | PASS | 缺指定 server marker，FAIL |
| PrimitivesGenerated reroute arming | DirectVulkan / `MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE=1` | PASS | 缺指定 server marker，FAIL |
| PointSizeDemotion arming | DirectGLES / `MOBILEGL_POINT_SIZE_DEMOTION=1` | PASS | 缺指定 server marker，FAIL |

六臂均无 skip；关闭前送时 server 本地仍真实产出 marker，client 本地没有同名替代 marker，
且失败必须发生在该 marker 断言，不能拿无关失败当负控成功。

第四条 `PipeVerifyArmingScenario.Armed` 的 marker 来自 client 的 `PipeFill`，所以正确预期是
关闭 peer forwarding 后仍绿，不应硬造一个 server 负控。已配置并真实执行当前 verify+split 构建，
但它在 arming 前遇到旧 `PipeRespecifyScope` 断言；同代码 inproc/spawn 同样复现。
这是既有 verify-only “全资源 respecify”断言与后来合法 per-level producer 的冲突，
不是 TCP 前送失败。证据 `/home/swung/p65-lf-verify/` 与 `/home/swung/p65-verify-controls/`；
此项尚未宣称通过。
（P7 wave 3 V1 更新：断言已放宽为「按 level 的 producer 覆盖其宣告范围」，verify × split 车道 1070/1070，见 [`notes/p7/verify-split.md`](../p7/verify-split.md)。）

CI 的 `runtime_mode_proof.py` 曾读取无角色后缀的日志名，导致真实两后端用例通过后仍 FileNotFoundError。
修复为读取 client/server 两份，并保留任一缺失为硬失败。除了正反 self-test，
还真实执行了 strict + role-split-state + run-ahead 下的两条 P5fRsp 用例，**2 PASS / 0 SKIP**，
随后运行完整 `logs` proof 成功；证据 `/home/swung/p65-ci-runtime-proof.{log,xml,json}`。

TCP 工具自身有名字漂移、伪 arm marker、ready 探针不消耗 accept 连接、PID 复用、
设备 idle 配置恢复等正反控制；`tcp_lane_tools_test.py` 在 WSL 为 **4/4 PASS**。
link seam 的七个负控与生产 ownership 门已接入 `test.yml`，结果见相邻实现报告。

正式 trace driver 为 `tools/trace_replay/run_tcp_matrix.py`，工具 stub 回归 **12/12 PASS**；
没有用 stub 冒充真实渲染。默认按正规 manifest loader 选择 39 个 CI split case，非 CI 的 rd12
只能显式选择且单列。新尝试独立目录，失败/超时/陈旧结果不作为 passed checkpoint；严格 resume
不能复用旧 lockstep 图像阳性。

正式 GLES 矩阵使用 `--require-run-ahead`：显式请求 RUN_AHEAD=1 与 VERB_BARRIER=1，逐例要求
私有 client 日志 ARMED 且没有 lockstep / DISARMED，并保存 requested_arm / actual_arm。
默认 300 秒日志无进展截止与 7200 秒绝对上限分开，不沿用本机 main-menu 的 180 秒预算；
旧脚本误杀 CMake 而遗留 retrace 子进程造成 Busy 的失败保留诊断，不计为 GPU 差异或矩阵通过。
新 driver 的超时清理覆盖整个进程组并回收孤儿，正式 39 条完成结果仍待登记。

## 真机进展与环境事实

设备为 Redmi `2f7cbe2e`，aarch64 / Adreno 830，IP `192.168.21.181`。
已安装并启动 trace flavour 的前台 Service，supervisor 监听 `tcp://0.0.0.0:40613`，
两端令牌一致；使用显式设置的 `MOBILEGL_BUILD_STAMP`，不依赖 APK 的 `nogit` 文件名。
当前沿用的 `410dfa94...` 是共享制品标识，不是当前 `52ffc6a7` 源码 HEAD。
已经收到真实 Welcome 与 Adreno caps，wire/build 指纹均接受。
此前安装的 traceDebug 原生实际只有 `-g`、没有优化，以上只作功能诊断，不能作为 release 性能数。
后续功能复验已使用原生 `-O2/NDEBUG` 的 debuggable traceRelease APK，并保持受信任签名；
各组模式与边界见下，不能把 Debug 诊断数当优化制品性能。

必须区分已经定位出的三类问题：

1. 手机 Dozing 时曾看到 native supervisor `wchan=do_freezer_trap`，前台 Service 的 partial wake lock
   显示 DISABLED。仅给测试包增加 deviceidle 白名单不足以立即解除所有冻结；Awake 下进程与 wake lock
   恢复。没有关闭全局 Doze/freezer，也没有靠延长控制超时掩盖冻结。
2. Awake 后仍有 LAN 回包只到第一个 MSS 的独立问题。WSL 默认到手机经 `eth2`、源 `172.18.0.1`，
   对应 Windows `xray_tun`。USB `adb forward` 对照通过 EGL；加一条仅针对手机的临时 host route 后，
   LAN 同样通过 EGL，完整 peer 日志由十余行恢复到数百行。该环境修复不是 transport timeout 改动。
3. 真正开始跨 ABI draw 后，暴露 archive 头对 native C++ `sizeof` 的比较（Linux 1056 / Android 0）。
   portable archive v2/schema 已落地，并进入握手指纹；重编后的两端已通过真实 LAN shader/draw 与
   64 MiB 上传阳性。最终 golden retrace 和性能数仍须使用优化制品采集。

### Debug 真机首轮 102 条

archive v2 的 Debug 制品首轮结果为 **73 PASS / 29 FAIL / 0 SKIP**，61.44 秒。
29 条全在 `eglInitialize` 前失败：20 条为明确 `Refuse{Busy}`，9 条为 Welcome 前 transport closed；
没有理由把它们称为 GPU 像素或能力差异。当前证据指向连续用例的客户端退出与远端 child 清理完成
之间的会话收尾竞态，必须处理后再跑完整车道，不能用 skip 消掉失败。

逐名诊断在 `/home/swung/p65-debug-device-differences.json`，完整 `.log/.xml` 为同前缀。
175 份已存在的角色日志已复制到 `/home/swung/p65-debug-device-rolelogs/`，
避免后续同名用例覆盖首轮证据。Discovery 与正常 Awake 维持记录分别为
`p65-debug-device-lane.json`、`p65-debug-device-awake.json`。

### 优化 Release 真机复验：历史 lockstep 功能阳性

有序会话收尾修复后，客户端等待 server EOF 完成退出；Busy 拒绝先消费 Hello，
避免带未读请求关闭 socket 时的 RST 丢失拒绝帧。优化 APK 的 native 实际编译参数为
`-O2 -DNDEBUG`，签名与已安装调试证书相容。

该制品 SHA-256 为 `70a4a3c776a61e03a3c0ad02cbdee38fd6af483a6c7cedeaf33bb1f8531fd228`，
真机完整车道为 **102 PASS / 0 FAIL / 0 SKIP**，98.59 秒。
全部 client 日志中 `Refuse{Busy}`、`no Welcome`、`ALIVE BUT SILENT` 均为 **0**；
逐条 TCP/stream/远端 endpoint/pid arm proof 通过，名字集合与 spawn 的 102 条完全一致。

证据：`/home/swung/p65-release-device.{log,xml}`、`p65-release-device-lane.json`、
`p65-release-device-awake.json`，以及已保全的 204 份角色日志目录
`/home/swung/p65-release-device-rolelogs/`。没有删用例、放宽像素断言或恢复 skip。

**这组 102 条后来确认实际为 lockstep，而非 ARMED。** 它关闭的是当次连续会话的 Busy/Welcome
初始化失败，保留为有意义的功能阳性；它没有关闭 run-ahead 形态下的设备验收。
上面的 APK SHA 只属于这组历史制品，不移记为 READY7 的产物身份。

### 首 caps race 与 READY7 真机复验

性能配对的日志揭露了启动竞态：控制 reader 尚未交付真实首 CapsSnapshot 时，client 就把
placeholder 的零 capability 当作 server 的回答；后续“只能 demotion”使本次 session 一直停留在
lockstep。修复要求先收到并采用真实首 caps，再宣布 session started 并锁存 run-ahead。

READY7 Redmi 车道已经完成：**102 PASS / 0 FAIL / 0 SKIP**，72.23 秒。
严格 `--require-run-ahead` proof 通过：102 条实际 ARMED，lockstep / DISARMED / Busy / noWelcome
均为 0。证据为 `/home/swung/p65-armed-device.{log,xml}`、`p65-armed-device-lane.json`，
204 份角色日志保全于 `/home/swung/p65-armed-device-rolelogs/`。
这组与 READY7 主机结果构成首 caps 修复的实际模式复验；正式 39 条 CI split golden 矩阵与
同模式性能对照仍待验证，不以这一组结果宣布整个阶段关闭。

### 真实对端死亡与 Wi-Fi 半开连接门

以下故障注入已有当次独立证据，无需把它们继续列作“从未采集”：

| 故障 | 观测到 peer hangup / device-lost | 结果与证据 |
|---|---:|---|
| 真正 kill server child（PID 18824） | 133 ms / 133 ms | PASS；`/home/swung/p65-device-peerloss/kill.log` 与两角色日志 |
| 直连 LAN 禁用 Wi-Fi（PID 18979） | 5053 ms / 5053 ms | PASS；`/home/swung/p65-device-wifi-loss/probe.log`、`timing.json` 与两角色日志 |

两个 probe 都通过生产 barrier 观察挂断并闩 device loss，`gl_reset=0x8255`，10 秒截止内通过；
不是发送“请退出”控制消息，也不是把 apply timeout 冒充挂断。Wi-Fi 门使用 **LAN direct eth0，
不是 adb forward**；父控制器从 disable 计到返回为 5.090431 秒，probe 自身记 5053 ms。
它们证明故障闩机制，不代替 READY7 完整图像/性能验收。

### 临时环境的恢复责任

`tcp_device_server.py --allow-idle` 是显式选项，不默认修改设置。原状态已保存为
`/home/swung/.cache/mobilegl/tcp-server/2f7cbe2e-top.mobilegl.plugin.trace.json`，
内容 `originally_whitelisted=false`。重复 start 不覆盖这个原值。所有设备工作完成后，在 WSL 运行：

```bash
python3 <tree>/tools/trace_replay/tcp_device_server.py stop --serial 2f7cbe2e \
  --state-file /home/swung/.cache/mobilegl/tcp-server/2f7cbe2e-top.mobilegl.plugin.trace.json
```

它停止本测试包并撤销本次添加的包级 idle 豁免。root 另负责撤回原本不存在的网络辅助配置：

```bash
ip route del 192.168.21.181/32 via 192.168.31.1 dev eth0 metric 10  # WSL root
adb -s 2f7cbe2e forward --remove tcp:40615
```

## 尚未关闭的出口项

- READY7 真正 ARMED 的主机 TCP 和设备 102 已取得；仍需把最终冻结制品的 ALL、必要回归、
  discovery/逐条 mode proof 与阶段结论对应起来，不能引用旧 lockstep 102 代替。
- 正式 WSL → Redmi **39 个 CI split case** 的 golden 矩阵（DirectGLES 按现有阈值，OpenRA 1.0）；
  `ci:false` 的 rd12 作为额外 golden/性能负载单列。旧 scratch 的前两条阳性保留，不能自动变成
  新 driver/checkpoint 的通过。DirectVulkan 按本波口径记录，不作为相同的硬像素门。
- 同一真实 ARMED 模式下 OpenRA / MC in-world 的 wait-reply、RTT、Stage bytes/frame 与链路
  吞吐对应、credit 1/2/3、loopback TCP 对 spawn 的逐线程 CPU 配对数。旧 TCP lockstep 的 CPU
  样本是揭露错误模式的负控，不作为传输成本结论。
- inproc/spawn 的 before/after 逐线程 CPU 已测，但结果不是“逐字不变”；其口径和局限见
  [performance.md](performance.md)。数据不会被抹除，也不能把变化全部归因于 lk2 虚调用。
- lf 第四条仍有具名 verify+split 前置债，且其 client-owned marker 本来不能作 server forwarding
  负控。三个 server marker 的正负控与故障闩门已有证据，不重复列成缺采。
- 阶段审查已完成，见下；剩余验收结果与最终制品证据索引仍需收口，本账本不宣布 P6.5 收官。

## 阶段审查状态

原上下文已完成一次 `phase_review` 阶段审查，以及一次针对 half-close 的极窄 follow-up。
按 ID-66 不再另跑完整阶段审查。

阶段审查发现 Present / `glFlush` 的尾批在 client 随后空闲时未投递；已修复为发送边界，并用
真实生产入口的零等待回归证明没有偷加 apply wait：

- `StreamClientPublication.AllThreePresentCreditsReachAnIdlePeerWithoutAnyClientWait`
- `StreamClientPublication.ExportedGlFlushDeliversPublishedCommandsWithoutWaitingForApplied`

证据在 `MobileGL/MG_Test/Wire/StreamClientPublicationTest.cpp` 和对应定向测试记录。
half-close follow-up 未发现 normal-path 死锁或 UAF；它指出 `WaitClosed == true` 也包含 terminal
protocol error，不能仅凭该布尔值声称正常 EOF。该语义限制保留，不把 follow-up 改写为所有错误
路径均正常关闭的保证。最终证据索引由收尾报告统一串联。

## 主机 CPU 配对采集方法

`tools/trace_replay/measure_transport_cpu.py` 复用同一个已有 retrace runner，分别载入旧、新 library
和与之配套的 server。OpenRA 的完整 128 帧取尾 100 帧，rd12 的 251 帧取尾 200 帧；
每条臂保留完整 benchmark JSON、逐线程原始采样和三个制品的 SHA-256。
client 读现有 `CLOCK_THREAD_CPUTIME_ID` 帧数组；apply 读取 `/proc/<pid>/task/<tid>/schedstat`
的执行纳秒数，选最早的 `mgl-srv-apply` TID，排除继承该名字的 driver helpers，不以进程 CPU 代替。

apply 的帧窗口用 benchmark JSON 的写入时间和已有 wall-frame 数组近似对齐，边界在外部采样点间插值。
结果明确保留实际最大采样间隔，并说明尚有 summary/JSON 写入延迟；不能把它写成周期精确的测量。
已有 `before-pilot-not-comparable` 试跑只验证采集机制，发生在并发构建期间，不纳入正式 A/B。
正式 before 已在重型构建停止后完成，原始记录为 `/home/swung/p65-host-cpu-before/summary.json`。
使用现存的 P6.5 之前 `lk-split` 制品；三个库/runner 与 trace 的哈希均随样本保存，
不把缓存中的旧 build stamp 冒充精确源码基线。以下是各三轮逐轮均值的中位数，单位 ms/帧：

| trace / transport | client CPU | apply CPU（外部窗口近似） |
|---|---:|---:|
| OpenRA / inproc | 0.68184 | 3.04067 |
| OpenRA / spawn | 0.80624 | 3.24204 |
| rd12 / inproc | 8.28739 | 64.88425 |
| rd12 / spawn | 10.27651 | 67.61499 |

after 已按同参数完成，各 case/transport 前后均为三轮；完整差值和精度限制见
[performance.md](performance.md)。这组前后数包含整波正确性修复（例如补齐独立 client 的死亡记录），
不能把全部差值归因于 lk2 虚调用，也不能称为“逐字不变”。


上述 inproc/spawn before/after 表不是 READY7 的 TCP 传输成本表。此前另做的 TCP/spawn 比较中，
spawn 已 ARMED 而 TCP 被首 caps race 留在 lockstep；原样保留在
`/home/swung/p65-loopback-cpu/{summary,summary-with-arm-proof}.json`，不进入正式传输结论。
正式 TCP 对照与 golden driver 必须检查实际 `run-ahead ARMED`，同时拒绝 lockstep fallback / DISARMED。
