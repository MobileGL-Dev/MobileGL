# P5e：退役 Espryt draw path 的 lockstep（run-ahead）

头 `5c007b22`，`feat/disaggregated`。契约 `MobileGL/MG_Remote/CONTRACT-P5E.md`，裁定
`~/w7/notes/p5e/INTEGRATOR-DECISIONS-P5E.md`（ID-80..136）。**run-ahead 已武装，阶段未收官**（见"什么没完成"）。

## 目标

P5c 让 `inproc` 成为只经 wire 交换的诚实两角色，P5d 把它的 CPU 代价压下来，但 R-1 的 lockstep 还在：
客户端每条 `kCtxVerb` 都要等 `appliedSeq == emitSeq`。P5e 退役 draw path 上的这道等待——**客户端发了就走**，
只有"设障"的记录才等。

一条记录能否未设障，取决于**应用它时会不会读客户端内存**。所以这个阶段的绝大部分工作不是改等待逻辑，
而是把后端仍在读前端对象的那些点迁到记录上；等待规则本身是最后一步。

## 落地了什么

十二个包，三波：

| 波 | 包 | 一句话 |
|---|---|---|
| 1 | c0e | 契约与线上行：`PipeCalls.def` 第五列 `WaitClass` + `MGPipeWaitClassFor`、`set_program_bindings`（opcode 80）、`kCapRunAheadApply`（位 10，只在 DirectGLES 臂）、`kDrawClientArrays`、子系统位 13、两个旋钮、跨包 seam |
| 1 | id | 身份：registry 按 `{slot, gen}` 重键、by-handle resolver、分配器守卫。ID-96 的答案是空集 |
| 2 | vi | 顶点输入 / VAO / 索引 |
| 2 | sb | 缓冲绑定点；`MGPShaderBuffers::WritableMask` 加宽（ID-104） |
| 2 | pg | program：逐 link 的反射归档进 SEG_STAGE、`set_program_bindings` 的三条尾 |
| 2 | tx2 | 纹理与 sampler、staged store 的按句柄读 |
| 2 | fb | framebuffer、附件、image、blit |
| 2 | ra | 传输机制：等待规则、present credit、`gPipeInputs` 变 server 角色内存 |
| 2.5 | fix1 | 设备发现的 monolith 臂回归（ID-107），见下 |
| 3 | pa | program 家族在 draw / dispatch 路径上离开前端 |
| 3 | mv | multi-draw 的前端 VAO 读点；并给五个 multi-draw 档位补上门禁条目 |
| 3 | gl | 门禁机制本身，加两处潜伏崩溃 |
| 3 | ra2 | 翻开开关后暴露的竞态，以及间接绑定那次 pull |

**最重要的一条实现事实**：退役 `GetProgramForDraw@DrawArrays`（每帧每次 draw 都触发的那次拉取）
**没有改动任何线上结构**。记录早就带着逐 link 的 `ProgramArchive`；缺的只是仍去问前端对象的读者。
本阶段多数迁移是这个形状——数据已经在线上，读者没跟上。

## 三类缺陷，以及各自教的东西

这一节是本阶段最可复用的部分。

### 1. 一整条运行时臂没有被门禁（ID-107 / 109 / 110）

push 构建有**两条服务端臂**，运行时由 `MG_Config::Transport` 选；而 `integration-split` 的**每一条**条目
都导出 `MOBILEGL_TRANSPORT=inproc`，split 专属场景又都在 `SetUp` 里自跳过——**手机实际发布的那条臂在
门禁集合里零条目**。`44f91c74` 上 unit 2250/2250、`integration-split` 113/113 全绿，同一个头
`ctest -L integration-gpu` 是 **1157/1294，137 个 SEGFAULT、横跨 38 个 scenario**，设备上表现为 monolith
启动即崩。`66621767` 修复，`gpu` 成为常设门禁步。

代码形态的教训：**"已经记下句柄"不等于"记录臂会被选中"**，这是两句不同的话；只因"所有写入点碰巧都被
传输门住"而安全的座，是靠链条安全，要改成自述式的传输测试（ID-110 的 `MGB_TEXTURE_RECORD_ARM_SELECTED()`）。

### 2. 构建 flavour 也没有被门禁，而且第一次补的门禁是假绿（ID-124）

门禁只配 split flavour。补上 `flavours` 步后实测：**四个 flavour 里只有 split 编得过**。

更值得记的是补门禁本身翻了一次车：第一版 `pull` 臂报 rc=0 / 982 TU，是假的——
`CMakeLists.txt:496-500` 规定 `MOBILEGL_BUILD_DISAGGREGATED=ON` **强制** `MOBILEGL_PIPE_PUSH=ON`，
所以 `-DMOBILEGL_PIPE_PUSH=OFF` 被静默覆盖，那一臂其实是 split 构建的第二份拷贝；**而 CMakeCache 里
那一行仍然写着 OFF**。改成从 `compile_commands.json` 断言**实际生效的宏**、不符即失败，才拿到真数字
（743 TU，rc=1）。

**规则：一条臂的证据必须包含"它确实是那条臂"的证明。** 传的 flag 不是证据，cache 不是证据。
同源：`ctest -L` 是正则，新标签不得以既有标签为前缀，且要数一遍（ID-131）。

### 3. 一个永远打不响的守卫（ID-132 / 135）

翻开两个常量后 `unit` / `gens` / `flavours` 仍全绿，而 `isplit` 掉到 **112/181**、`gpu` 掉到
**1290/1359**，**且 flaky**（-j 4 红 71、-j 1 红 68，单跑一条先过后崩）。全量普查：70 条红、
只涉及**四个字段**、横跨**十一个 verb**。

**判据**：一个字段在一个 verb 上稳定失败 = 漏迁移；**四个字段散在十一个 verb 上且 flaky = 竞态**。

根因不是当初的猜测。`gPipeInputs` 的**字段值**已由 `SuppliedFieldMask` 分区、不竞态；竞态的是
**元数据标量**（`CurrentVerbSerial`、`FilledGen[]`、`m_currentVerb`、`m_serverStampedVerb`）。
而让它长期不可见的，是守卫本身：`RefusePipeInputsTouchWhileApplierOwnsIt` 的 run-ahead 臂对任何
`isBarrieredFill` 一律豁免，豁免依据是一句**关于未来**的断言——"客户端马上就要 park"——而真实顺序是
fill → emit → park。修法是把那句断言**变成事实**（写块之前强制等待），并让守卫去测事实。

同源的第二个缺陷：`MGP_VERB_OP_LIST` 把整个 draw 家族并到一行，于是十九个索引 draw verb 都答"设障"、
都去填充、都不 park。

契约 §3.2 早就**规定了 per-role 的 stamp 存储，却从未落地**——那个缺口正是这次竞态成立的前提。
一份描述"设计意图"而代码另做一套的设计文档，是这类缺陷得以存活的方式；`ARCHITECTURE.md` 的对应段落
已按落地实现改写。

### 贯穿三者的方法论结论（ID-132）

**strict 车道硬绿是必要条件，而且结构性地不是充分条件。** strict 跑在 lockstep 下，`ApplyOne` 把每条
记录都盖成 barriered，所以"只在客户端不再等待时才发生"的事，它按构造看不见。它量的是**剩余欠债**，
不是 run-ahead 的**正确性**。阶段出口必须有一条独立的、**翻开开关之后**的车道。

## strict 车道成为可被脚本判定的硬绿

`GetProgramForDraw@DrawArrays` 62 → 0，`GetProgramForDispatch@DispatchCompute` 7 → 0，
`GetBoundVertexArray@DrawArrays` 9 → 0。采纳规则最终是**三个析取项**，没有一条是预先设计出来的，
每一条都是被一条真实条目逼出来的：

1. 该 verb 的线上 op 静态设障（ID-116）；
2. 该字段的退役相不指向本阶段（ID-125）；
3. 记录是**按升级**而非按静态类设障的（ID-128）——XFB 活跃与客户端数组两种 draw，契约把两者都放在
   P5e 之外。

白名单由 `scripts/gen_pipe_field_ownership.py` **生成**而非手抄：ra 原本落在 CI 里的 13 行手抄表
**在两个方向上都错**（漏了 `GetFramebufferBindingSlot@ReadPixels` 的 21 条，又多了不该采纳的
`GetTextureObject@CopyImageSubData`）。被采纳的 pull **响而不致命**（ID-117）：同样的标记语法，头部
标签换成 `Admitted{`，按 `(field, verb)` 去重。车道是**两侧棘轮**——新出现的对要认领，不再出现的对
要删除，否则会"烂绿"。

棘轮在合并当天就自己点了名：`GetProgramForDraw@DrawArrays` 不再出现（pa 退役了它），
`GetBufferBindingSlot@DrawArrays` 新出现（mv 的间接档位逼出了它，而 mv 自己在报告里**先命名了**这行
潜伏项，然后才让它触发；它退役在 P8）。

## 门

| 门 | 阶段初 | 现在 |
|---|---|---|
| `unit` | 2250 | **2256 / 2256** |
| `integration-split` | 116 | **179 / 179** |
| `integration-gpu` | 1294 | **1357 / 1357** |
| 构建 flavour | 只有 split 编得过 | **pull / push / split 三个全绿** |
| 逐条 strict 普查 | 7 绿 / 109 红 | **179 绿 / 零 `Fatal{`**，八行 `Admitted{` |

新增的 65 条车道条目，全部覆盖此前**一条门禁条目都没有**的臂或档位。两条预期红车道各自具名断言：
`integration-clientarrays-split`（`(Multi)DrawArrays+CLIENT_ARRAYS`）、`integration-magma-split`
（`GetFramebufferBindingSlot@Clear`）。

## 数字

设备：Redmi `2f7cbe2e`，FCL fordebug + Minecraft 26.3-rc-3 世界 `test`，DirectGLES。

### 先讲定频，因为第一轮矩阵作废了

第一轮**没有定 CPU 频**：**monolith 自己那一臂**的逐帧 CPU 在两次运行之间从 4.43 跳到 7.07 ms，
而慢的那次**温度更低**（53.5 vs 58.9 °C）——是 `walt` governor 把 policy0 从 2745 拉到 748 MHz，
不是热降频。未定频的逐线程 CPU ms/帧**不能跨臂比较**。

`tools/device_bench/pin_device.sh` 不认识本机 serial 并**明确拒绝猜**（正确，猜错会静默失败）；
`~/w7/notes/p5e/pin_redmi.sh` 按本机节点写，写序 **min→硬件底 → max→目标 → min→目标**，并**回读
`scaling_cur_freq` 验证**，每轮跑前跑后各 check 一次。

### 为什么值得做（侧写，`7c6f6886`，翻开关之前）

symbolized `simpleperf cpu-cycles`，客户端 GL 线程：`SessionProducer::WaitForAppliedOrEventBacklog`
**31.58% 自身时间**，第二名只有 3.72%。计数器同期显示客户端每帧进入 doorbell 等待约 **916 次**
（对 ~849 次 draw），99.8% 靠自旋解决；`MOBILEGL_IPC_SPIN_US=0` 让每次都 park，帧率塌到 **20.9**。
**代价是会合本身，不是自旋参数**——所以修法是"不再每次 draw 会合"，不是"把每次会合做便宜"。
等待之后，客户端线程是一条 1-3% 的长尾：**没有第二个杠杆**。

### 渲染距离 12：两条臂都撞面板上限

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.4 / 115.6 | 117.5 / 117.2 | 6.25 / 6.45 | — |
| inproc run-ahead | 118.0 / 117.6 | 118.6 / 118.5 | 6.87 / 6.92 | 4.35 / 4.43 |
| inproc lockstep（`RUN_AHEAD=0`） | 111.6 | 113.7 | 8.16 | 6.53 |

monolith 与 run-ahead 都撞到 **120 Hz 面板**，只有 lockstep 撞不到。所以这一档回答的是"能不能撞到
上限"，不是"谁更快"。

### 渲染距离 32：可比的一轮

`renderDistance:32`，**~3550 draws/帧**（VD12 的 4.2 倍），**两条臂都远离任何上限**（游戏自身
`enableVsync:false`、`maxFps:260`，实测 max 只有 37-71）。静置提到 75-150 s——VD32 的区块流式加载
远比 VD12 长。每臂五次，交错。

| 臂 | n | fps p50 中位 | p50 范围 | client ms/帧 中位 | apply ms/帧 |
|---|---|---|---|---|---|
| monolith | 5 | 58.5 | 56.3 – 67.9（±10%） | 16.19 | — |
| **inproc run-ahead** | 5 | **59.9** | 56.8 – 61.1（±4%） | **15.72** | 11.1 – 12.1 |
| inproc lockstep | 2 | 35.5 | 34.5 – 36.5 | 26.17 | 19.7 – 20.5 |

**结论：齐平。** run-ahead 在两个中位数上都略微领先，但**差值远在 monolith 自身离散度之内，不能读作
"比 monolith 快"**。确实超出噪声的是稳定性：monolith 五次 ±10%，run-ahead ±4%。`mono1` 的 67.9 是
全场唯一离群值（装包后第一次运行）。

**相对它取代的 lockstep：p50 +69%、client CPU −40%、apply CPU −43%。** 负载越重越值钱，符合预期——
会合次数随 draw 数走，VD12 每帧约 900 次，VD32 约 3600 次。

两条臂**都是 client 线程打满**（约 94%），所以 client ms/帧 这一列是各自真正的瓶颈，可直接比。

## 什么没完成

- **BRIEF-P5E §3 的 item 4 与 item 5**：三条阴性对照、逐包 red-once 重跑。item 5 的证据形式在当时
  **不可执行**（每个候选在 revert 之前就已经是红的），ID-121 把它重排到车道变绿之后——之后尚未执行。
- `scripts/ci/split_negative_controls.sh` 的 E1 对照**自检就失败**，且它索要的 `Fatal{BarrierViolation}`
  在 `MOBILEGL_IPC_BATCH_WAITS` 默认值 1 下根本不可能触发（ID-122）。一个永远打不响的对照比没有对照更坏。
- **G1** 仍由 CI 断言；本阶段门禁只编 flavour，不做符号恒等（ID-123）。
- 契约 §8 修正 8（`GetBufferBindingPointCount` 的 sticky forward）标为 **UNLANDED**（ID-120）；
  §3.2 的 per-role stamp 存储同样未落地，正是它让上面第 3 类缺陷成立。
- **VD12 面板上限之上谁更快**没有回答：本机定不住更高频率。VD32 那一轮回答了一般性的问题。

## 下一个机会（不是缺陷）

VD32 下 apply 线程只有 **11-12 ms**，client 有 **15.7 ms**——**两侧不平衡**。把工作从 client 挪到
apply 会直接降低瓶颈。这在 lockstep 下毫无意义（两侧串行相加），**是 run-ahead 解锁出来的方向**，
留给后续阶段。
