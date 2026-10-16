# P6.5 主机逐线程 CPU 前后记录

**已经取得前后数字，但不能称为“逐字不变”。** 本次记录覆盖本机 inproc 与 unix+shm spawn，
用于观察整波改动对保留车道的影响；性能按路线图通用纪律只记录，不据此阻塞正确性验收。
它不代表 TCP 吞吐，也不代表 Android Release 性能。

## 定义与条件

- WSL Arch、DirectGLES + llvmpipe；两臂均使用现有 Release 制品，同一个 retrace 可执行文件。
- before：`/home/swung/lk-split/libMobileGL.so`；after：READY6 的
  `/home/swung/p65-split/libMobileGL.so`。各自启动同目录下的 server。
  before 是既存的 P6.5 前制品，以实际 SHA-256 定身份；旧 CMake 缓存中的 build stamp
  不足以声称它恰好对应当前远端源码头。
- OpenRA 完整 trace 128 帧，选择最后 **100 帧**；rd12 in-world 完整 trace 251 帧，选择最后 **200 帧**。
  均排除起始资源准备窗口，不把全部 replay 时间除以帧数充作稳态结果。
- 每个 case × transport × before/after 均为 **3 次完整 replay**，共 24 个样本。
  每组记录按 repeat 交替 inproc/spawn；全部 before 完成后等待构建结束，再采 after。
  不是版本间随机交错试验，也没有 CPU 定频，因此不据此宣称统计显著性或唯一因果。
- 采集窗口内没有并发重型构建或另一条 host retrace。此前带
  `before-pilot-not-comparable` 标签的试跑仅用于校验采集器，不进入表格。
- `PRESENT_CREDIT=1`、run-ahead=1、verb-barrier=1、batch-waits=1、spin=50us；
  PipeStats=0、strict=0、role-split-state=0、audit=0。完整有效环境随每条样本保存。
- `--benchmark-finish=0`：runner 不额外在每帧插入 glFinish；trace 自己的同步行为仍保留。

**client CPU** 来自已有 runner 的 `CLOCK_THREAD_CPUTIME_ID` 与 `frameCpuTimesMs`，
取选中尾窗口的平均值。它包括该 replay 线程上的解码与 MobileGL 调用，不是整个进程 CPU，
也不是只计 MobileGL 函数的采样 profile。

**apply CPU** 来自 `/proc/<pid>/task/<tid>/schedstat` 的 execution runtime 纳秒计数。
选择最早的、仍存活的 `mgl-srv-apply` TID；后建的 driver helpers 会继承该名字，不能相加冒充
一个 apply 线程的 CPU。每条原始采样保留 pid、tid、线程名、采样时刻和累计 CPU。
如果没有唯一线程完整覆盖选中窗口，采集器报无效，不填 0。

### apply 窗口的精度限制

runner 现有帧数组没有绝对 monotonic 起点，因此外部 apply 采样使用 benchmark JSON 的文件 mtime
近似定位 `benchmark::End()`，再减去已有 `totalSeconds - sum(frameTimesMs)` 的非帧尾段，
最后回退选中尾窗口的 wall 时间。两个边界在线程 CPU 采样点间线性插值。

这存在**未单独测量的 summary/JSON 序列化延迟**，不能写成严格帧边界同步或纳秒精度的结果。
请求采样周期为 5 ms；before 实际最大间隔 **7.159784 ms**，after 为 **15.230268 ms**。
实际间隔与原始点均保留，后续可重新归约。下面多保留的小数位用于复算，不提高测量精度。

## 结果

每项是三次 replay 的“尾窗口 CPU 均值”的中位数，单位 **ms/帧**。
百分比为 `(after / before - 1) × 100`；完整未舍入值在 comparison JSON 中。

| trace | transport | 线程 | before | after | 差值 ms/帧 | 差值 % |
|---|---|---|---:|---:|---:|---:|
| OpenRA | inproc | client | 0.681840 | 0.705350 | +0.023510 | +3.448023% |
| OpenRA | inproc | apply | 3.040668 | 3.108000 | +0.067333 | +2.214402% |
| OpenRA | spawn | client | 0.806240 | 0.786330 | -0.019910 | -2.469488% |
| OpenRA | spawn | apply | 3.242036 | 3.198453 | -0.043583 | -1.344321% |
| rd12 | inproc | client | 8.287390 | 8.362680 | +0.075290 | +0.908489% |
| rd12 | inproc | apply | 64.884249 | 64.917254 | +0.033006 | +0.050868% |
| rd12 | spawn | client | 10.276510 | 10.285330 | +0.008820 | +0.085827% |
| rd12 | spawn | apply | 67.614988 | 67.059490 | -0.555499 | -0.821561% |

逐轮均值的范围也必须保留，避免只给一个点：

| trace / transport / 线程 | before 三轮范围 | after 三轮范围 |
|---|---:|---:|
| OpenRA / inproc / client | 0.678730–0.688320 | 0.690650–0.715600 |
| OpenRA / inproc / apply | 3.038111–3.112060 | 3.064185–3.172090 |
| OpenRA / spawn / client | 0.799590–0.810700 | 0.780370–0.792480 |
| OpenRA / spawn / apply | 3.239807–3.299042 | 3.187598–3.211357 |
| rd12 / inproc / client | 8.195700–8.325585 | 8.311060–8.510350 |
| rd12 / inproc / apply | 64.826829–64.939705 | 64.915537–65.039914 |
| rd12 / spawn / client | 10.274680–10.279320 | 10.250475–10.306380 |
| rd12 / spawn / apply | 67.489521–67.649413 | 67.010901–67.281044 |

存在双向变化：inproc client 的两个样本组上升，spawn client 一降一基本持平；
apply 也不完全相同。没有删除正增量，没有扩大阈值把它改写成“零开销”。
整波还补齐了独立 client 的死亡记录等正确性行为，故不能把全部差值唯一归因于 lk2 的调用形式。

## 制品与复现

共同 runner：
`/home/swung/tr-split/tools/trace_replay/mobilegl_trace_replay`

| 制品 | SHA-256 |
|---|---|
| runner（前后相同） | `700907be83c2c1c1c83eec8de37300a6fdab4c7f06387116fa8aaee0169b7291` |
| before libMobileGL.so | `a84d0c09b78b312c600e172c03bd9a6e4ce804585661859d888b20992b4e516d` |
| after libMobileGL.so | `d783080c5e1cb7db209a362db4ad742634d7525ae0c9683c4f6e9c5c54803361` |
| server launcher（前后相同） | `7a95cb4d276b40e4843c9d9f8c4d0f3486bfa6b5fc3e3b9a0dd842e1e9403ed6` |

server 是薄 launcher，`NEEDED libMobileGL.so`、`RUNPATH=$ORIGIN`；它自身字节相同并不表示加载了同一库，
两臂分别从各自目录解析生产库。

```bash
python3 <tree>/tools/trace_replay/measure_transport_cpu.py \
  --runner /home/swung/tr-split/tools/trace_replay/mobilegl_trace_replay \
  --library /home/swung/lk-split/libMobileGL.so \
  --server /home/swung/lk-split/libMobileGLServer.so \
  --fixtures /home/swung/w7/pipe/tools/trace_replay/fixtures \
  --inputs /home/swung/p65-host-cpu-input \
  --out /home/swung/p65-host-cpu-before --label before-p65 --repeats 3
```

after 只将 library/server 换成 `p65-split` 目录，out/label 换成对应 after 名。
脚本拒绝覆盖已存在的 benchmark 证据；重新测量应换新的 out。
新工作树中的某些 fixture 是 LFS 指针，故本次使用已核实的 Linux fixture 实体，并保存每条 trace SHA-256。

证据位置：

- `/home/swung/p65-host-cpu-before/summary.json`
- `/home/swung/p65-host-cpu-after/summary.json`
- `/home/swung/p65-host-cpu-comparison.json`
- 各目录的 `<transport>/<case>/run-N/{benchmark.json,cpu-summary.json,apply-samples.json,runner.log}`

这些数关闭了“必须真的量到逐线程 CPU”这一记录缺口，不关闭其他尚缺的 P6.5 设备/trace/吞吐指标，
也不把性能变化当正确性失败。

## 当前 spawn 与 loopback TCP 的专门比较：首轮负控

另外启动了仅用于本机测量的 `tcp://127.0.0.1:40616` supervisor，使用当前库、同一 runner、
credit=1，client/server 均开启 `MOBILEGL_PIPE_STATS=1`、`MOBILEGL_PIPE_STATS_PERIOD=1`。
`measure_loopback_cpu.py` 用 `P65LinkMetrics` 与 `P65ServerMetrics` 的线程 CPU 时钟，
逐个核对 client/server 帧号：OpenRA 29–128（100 帧），rd12 52–251（200 帧）。
这组 apply 数据直接来自帧时钟，不使用上节的 `/proc` 外部窗口近似。

**首轮不能作为纯传输成本对照。** 虽然两臂都请求 run-ahead=1，实际日志证明 spawn 已 ARMED，
而 TCP 在真实第一份 CapsSnapshot 到达之前，错误地用 placeholder 的零能力锁存为 lockstep，
随后“只允许 demotion”的规则使它一直无法武装。该差异由真实测量揭露，不能当成 TCP 固有成本放过。

完整首轮样本保留在 `/home/swung/p65-loopback-cpu/summary.json` 与
`summary-with-arm-proof.json`；OpenRA 每臂三轮，rd12 每臂一轮。
其中 rd12 的实际 client/apply CPU 为 spawn **10.377163 / 66.994613 ms/帧**，
错误 lockstep 的 TCP **105.151038 / 160.624839 ms/帧**。
这是具名负控证据，不进入正式 transport 比较结论。

修复由启动路径等待真实首 caps 完成，不强制打开 run-ahead，也不放宽后续只允许 demotion 的规则。
采集器已增加实际 ARMED、无 lockstep fallback、无 DISARMED 的检查；下节记录修复制品的正式重跑。

### 首 caps 修复后的正式两角色 CPU 数

修后 TCP 已完整重跑，所有样本均通过实际 run-ahead ARMED 检查，没有 fallback 或 DISARMED。
OpenRA 各臂 3 次取逐轮均值的中位数；rd12 各臂 1 次，单次样本不用于统计显著性推断。
两角色使用相同精确帧号窗口，不把 setup 算入稳态，也没有使用外部 `/proc` 插值。

| trace / 窗口 | 线程 | spawn unix+shm ms/帧 | TCP+stream ms/帧 | TCP − spawn ms/帧 | 差值 |
|---|---|---:|---:|---:|---:|
| OpenRA / 29–128 | client | 0.913889030 | 0.725938440 | -0.187950590 | -20.566019% |
| OpenRA / 29–128 | apply | 3.414349090 | 3.123573070 | -0.290776020 | -8.516294% |
| rd12 / 52–251 | client | 10.377163490 | 6.507507265 | -3.869656225 | -37.290115% |
| rd12 / 52–251 | apply | 66.994613060 | 53.903945275 | -13.090667785 | -19.539881% |

这是**指定两个线程**的实测变化，不能据负差值宣称 TCP 的进程总 CPU 更低：
TCP 另外有专用 I/O reader，以上 client/apply 时钟不包含那些线程的 CPU，也不包含其它 driver workers。
本表开了 PipeStats，前节 P6.5 前后表关闭 PipeStats；两表不能交叉相减。

spawn 控制复用本轮首 caps 修复前已真实 ARMED 的样本，其运行中的数据路径未改变；
TCP 使用修后制品。这个复用边界明确保留，不称两个 library 文件哈希相同：

- spawn library：`d783080c5e1cb7db209a362db4ad742634d7525ae0c9683c4f6e9c5c54803361`
- TCP library：`29fcff6b23557be6b1cc3765db382b3422ed0780643c719937616259298d3116`
- runner 与输入 trace 保持相同，完整哈希在各样本 `role-cpu.json` 中。

并发检查：最后一项 syntax 编译结束于 WSL 07:56:32.732；TCP OpenRA 三轮尾窗口分别从
07:56:56、07:56:58、07:57:00 开始，rd12 尾窗口从 07:57:55 开始，均无编译重叠。
这组精确帧时钟证据为 `/home/swung/p65-loopback-cpu-fixed/summary.json`，
归约比较为 `/home/swung/p65-loopback-cpu-comparison.json`；旧 lockstep 数据仍原样保存为负控。

最小复现（在没有其它 host 重负载时运行；不占手机端口）：

```bash
p65_tree=/mnt/c/Users/geekerwan/.codex/worktrees/p65-all-tcp/MobileGL
LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless \
MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 \
python3 "$p65_tree/scripts/ci/tcp_server_fixture.py" start \
  --server /home/swung/p65-split/libMobileGLServer.so \
  --endpoint tcp://127.0.0.1:40616 --state /tmp/p65-cpu-server.json
trap 'python3 "$p65_tree/scripts/ci/tcp_server_fixture.py" stop --state /tmp/p65-cpu-server.json' EXIT
python3 "$p65_tree/tools/trace_replay/measure_loopback_cpu.py" \
  --runner /home/swung/tr-split/tools/trace_replay/mobilegl_trace_replay \
  --library /home/swung/p65-split/libMobileGL.so \
  --server /home/swung/p65-split/libMobileGLServer.so \
  --inputs /home/swung/p65-host-cpu-input --out /tmp/p65-role-cpu
```

该命令用同一当前制品重测两臂；本次修复后仅 TCP 重测使用了 `--transport tcp`。
脚本按帧号核对两份日志、拒绝缺帧/重复帧/非正 CPU，并复用 `p65_link_metrics.py` 的归约逻辑。
