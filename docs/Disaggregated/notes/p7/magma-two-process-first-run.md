# P7 wave 0 包 L：Magma 两进程车道的首轮（2026-09-22）

> 计划见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §0 发现 1 / §1.3 出口门 1 / §3 wave 0 第 1 项。
> 基线 `feat/disaggregated@e8b2c4bd`，分支 `p7/lane`，主机 WSL Arch + lavapipe
> (`/usr/share/vulkan/icd.d/lvp_icd.json`，系统唯一 ICD)，`build-split` = Release / clang / ccache /
> `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON` `BUILD_INTEGRATION_TEST=ON`，
> `MOBILEGL_ITEST_TCP_ENDPOINT=tcp://127.0.0.1:40713`（本树专用端口，避免与兄弟树的 40613 撞车）。
>
> 本文记两轮：**第一轮**（提交 `e22561aa..a6d805cc`）铺了 tier 1 / tier 2；**第二轮**（集成者追加指令，
> 提交 `f28409be..`）按「出口门 1 = 整个集成二进制」的读法加了 **tier 3 全量普查**，并裁定了 ICD 与
> retrace 旋钮两处不对称。

## 0. 一句话结论

**仪器已经存在；把整个集成二进制铺到 Magma × 三条传输之后，红名单是 1 条。**

三档车道、1940 个新条目、每一条都实跑过：

- **tier 1 门控**（六条手铺 Magma 车道 × 三臂）：73 / 52 / 52，**0 红**。
- **tier 2 信息档**（split 臂跑的 ~100 个场景 × 三臂）：102 × 3，**0 红**。
- **tier 3 全量普查**（**整个集成二进制**，即 monolith `DirectVulkan.` 臂用的同一过滤器，× 三臂）：
  510 × 3，**split 0 红 / spawn 0 红 / tcp 1 红**。

那唯一的一条红是真东西，不是管道：
`IterationRPProgram203Scenario.FixedCompleteInputProducesFixedCompleteGoldenOutput` 在 **tcp（stream
数据面）上确定性地**（5/5）少算对了一个 texel——**只差第 0 号 texel，就是曝光值**——而 inproc、spawn（shm
数据面）与 monolith 各 5/5 全绿。详见 §5。

因此对计划 §1.3 的修正结论是：**主机 × lavapipe 口径下，P7 出口门 1 的集成半边只欠一条**，而且欠的不是
`@P7` 具名拒绝——**树上 15 处 `@P7` 拒绝没有一处被这 1530 条全量条目触发**。wave 2 的 A/B/C 三簇如果指望
从这条车道拿红名单，拿不到；它们的红名单必须来自 **wave 1 真机**、**retrace 全矩阵**和 **P3b/P4b 的场景
普查**。这条车道的职责应改为**回归门 + stream 数据面的显微镜**。

## 1. 交付物与文件

| 交付 | 文件 | 要点 |
|---|---|---|
| 后端维度 | `MobileGL/MG_IntegrationTest/CMakeLists.txt` | `mgl_itest_register_split_arms_for_backend(<backend> <filter> <tail> [stems])` + 同名薄包装保持 32 个调用点不变；`MGL_ITEST_VULKAN_{SPLIT,SPAWN,TCP}_ENVIRONMENT`；`mgl_itest_magma_arm_shape()` 供六条手铺车道复用同一套臂形状 |
| 三档标签 | 同上 | 见 §2。CMake 注释里写死了三档的定义、升档规则与标签拼写的正则理由 |
| ICD 裁定 | 同上 | 六条 Magma 车道从 `${MGL_ITEST_COMMON_ENV}` 改为 `${MGL_ITEST_VULKAN_ENV}`；NamedBlit / P5fRsp 两块按**后端**选，GLES 半边不拿 VK 钉 |
| 死亡通知前提 | `MobileGL/MG_Remote/Client/WireTables.cpp` | 去掉 `ActiveBackendType == DirectGLES`，保留 `Transport == Spawn` 与 `GetStateObjectDeathOps() == nullptr` 两条（P6.5 原意的两半） |
| 私有日志纪律 | `Harness/split_log_paths.py`、`Harness/SplitLogPaths.cmake.in` | `is_split` 前缀补 `DirectVulkan.Spawn.` / `DirectVulkan.Tcp.`；tier 3 走**自己的**列表，只发日志路径 + CtWire 的计数器开关 |
| 名集合门 | `scripts/ci/spawn_lane_parity.py` | 新增三档的三臂比对（逐档的 inproc-only 豁免） |
| CI | `.github/workflows/test.yml` | retrace-split 补**五个** Magma 旋钮；新增 magma-spawn/-tcp 门控步骤与 buffers 子标签的 `require_green` |

## 2. 三档车道（本包的主结构）

**第一档，门控。** `integration-magma-split`（inproc，**含义不变**：同样 73 条、同样的名字、同样的标签、
同样的私有日志路径）＋ `integration-magma-spawn` / `integration-magma-tcp`。跑的是六条**手铺** Magma 车道
（Fm / Buffers / RunAhead / Caches / NamedBlit / P5fRsp），因为每条都带自己的 flavour（strict + dual-block、
adopt tier、run-ahead credit、cache pin），共享的场景过滤器表达不了。**一条标签只有在实测绿之后才准进门控
步骤**——两条都实测绿，所以进了（§7）。

**第二档，信息性。** `integration-magma-all-{split,spawn,tcp}`：DirectGLES split 臂跑的每一个场景，在
DirectVulkan 上，三条传输各一遍。由宏记录的调用列表**回放**生成（`MGL_SPLIT_ARM_CALLS`，32 次调用），所以
明天给 split 臂加一个场景，Magma 普查当天跟着长。

**第三档，全量普查（集成者追加）。** `integration-magma-full-{split,spawn,tcp}`：**整个集成二进制**，即
monolith `DirectVulkan.` 臂用的同一过滤器（`-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}`，
`CMakeLists.txt:770-778` 的形状），510 个用例 × 三臂。**出口门 1 说的是这个集合**，不是 tier 2 的 102 个。
tier 2 里没有一个场景能走到 `@P7` 拒绝；回读矩阵、同纹理 copy 形状、奇偶数偏移的 uniform range 这些能走到的，
只在这一档里。

三档**都包含 inproc 臂**，这是诊断半边：spawn/tcp 红而 inproc 绿 = 两进程缺陷；三条都红 = Magma 自己缺动词。
§5 的那条红正是靠这个判出来的。

**标签拼写是按 `ctest -L` 的正则选的**（ID-131 的规矩）：档位词放**中间**——`integration-magma-all-spawn` /
`integration-magma-full-spawn` 都不含 `integration-magma-spawn` 子串，而显然的 `…-spawn-all` 含，会把上千条
预期可红的条目悄悄拖进门控车道。九种拼写双向都查过。子标签同理：inproc 臂保留历史名
`integration-magma-buffers`（CI 那一步按名字 require_green 它的 19 条），其余臂把臂词放**前面**。

### 2.1 tier 3 的环境与 tier 1/2 差两处，两处都是实测出来的

1. **不设 `MGITEST_SPLIT_LANE=1`。** 该标记武装 `ScenarioFixture` 析构里的断言
   （`Harness/ScenarioFixture.h:85-100`）：**在武装的 split 车道里跑过的用例必须让客户端编码器的记录序号
   前进**，理由是「workload drew, cleared and read pixels, so records were due」。对一条**策展过的**车道这
   完全正确；对**整个二进制**的普查它是错的——普查里还有纯查询用例。**第一次带着这个标记跑，13 条红**：
   `AdvertisedLimitsScenario`（8）、`SpirvShaderBinaryScenario`（4）、
   `ProgramPipelineScenario.AReservedNameTakesStateBeforeItIsAProgramPipeline`（1），每一条都是
   `EmitSeq was 20 at SetUp and is 20 now`——limits 查询由客户端 CapsMirror 就地回答，shader-binary 的错误
   面根本到不了动词。**这 13 条在 DirectGLES 的全量普查上也会红**：它是普查的属性，不是 Magma 的属性。
   **放弃了什么（明写）**：这一档不证明自己的记录过了线，tcp 臂的「Dial=Connect 没有拉起本地 server 子进程」
   拓扑证明（`ScenarioFixture.h:160-171`，在同一个武装块里）在这里是哑的。tier 1/2 两样都保留，而它们才是
   门控的那两档。
2. **`DualBlockScenario.*` 被过滤掉。** 它的绿**要求** `MOBILEGL_IPC_ROLE_SPLIT_STATE=1`，而普查绝不能开这个
   旋钮——开了之后每一次 barrier-pulled 读都 abort，普查的主题就换成了另一个实验。不开则它**按名字红**
   （`MGPipeRoleSplitRehearsalActive() is false`），这是它**写在案的负控行为**，不是发现。monolith
   `DirectVulkan.` 臂里同一条用例是 **SKIP**（那个运行时根本不是 split）。它自己的车道
   `integration-dualblock-split` 才是读它的地方。

## 3. 车道结果（逐档、逐臂）

统计口径：`ctest --output-junit` 的 testcase；tcp 车道的条目数含 `TcpServer.Start` / `TcpServer.Stop` 两条
fixture。门控三档按 CI 的 job 环境跑（`MOBILEGL_ITEST_REQUIRE_GPU=1` `MOBILEGL_IPC_STRICT_ERRORS=1`
`MOBILEGL_IPC_ROLE_SPLIT_STATE=1`，`-j 2`）；信息档与全量档按普通口径（`REQUIRE_GPU=1`，`-j 8`）。

| 车道 | 条目 | PASS | FAIL | SKIP | 与基线比 |
|---|---|---|---|---|---|
| `integration-magma-split` | 73 | 71 | **0** | 2 | 73，与基线**逐条相同** |
| `integration-magma-spawn` | 52 | 50 | **0** | 2 | 新增 |
| `integration-magma-tcp` | 54（52 + 2 fixture） | 52 | **0** | 2 | 新增 |
| `integration-magma-all-split` | 102 | 100 | **0** | 2 | 新增（信息性） |
| `integration-magma-all-spawn` | 102 | 100 | **0** | 2 | 新增（信息性） |
| `integration-magma-all-tcp` | 104（102 + 2 fixture） | 102 | **0** | 2 | 新增（信息性） |
| **`integration-magma-full-split`** | **510** | **453** | **0** | **57** | 新增（全量普查） |
| **`integration-magma-full-spawn`** | **510** | **453** | **0** | **57** | 新增（全量普查） |
| **`integration-magma-full-tcp`** | **512**（510 + 2 fixture） | **454** | **1** | **57** | 新增（全量普查） |
| `integration-split`（GLES） | 186 | 186 | 0 | 0 | 与基线相同 |
| `integration-spawn`（GLES） | 102 | 102 | 0 | 0 | 与基线相同 |
| `integration-tcp`（GLES） | 104 | 104 | 0 | 0 | 与基线相同 |
| `integration-magma-{buffers,runahead,caches}` | 19 / 14 / 7 | 全绿 | 0 | 0 | 与基线相同 |

**arming 证据**（不是「跑了没红」，是「确实跑了两个进程、确实是 Magma」）：DirectVulkan split 家族条目
**每条恰好一个**私有绝对日志路径（`SplitLogPaths.PrivateAndDistinct` 绿），落盘成对的
`.client.log` / `.server.log`；任取一对读到

```
client: Config: Active backend type set to DirectVulkan
client: Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream crosses a real ring to an apply thread in ANOTHER PROCESS
client: MG_Remote client: spawn ARMED - the server role runs in pid 130832, reached at "@mgl-130770-106887814518704"
server: MG_Remote server: pid=130832 transport=spawn role=server ready control=unix data=shm
server: MG_Remote server: pid=625725 transport=spawn role=server ready control=tcp data=stream   (tcp 臂)
```

## 4. 全量普查的 57 条 SKIP：全部具名，没有一条是「静默的」

三条臂的 57 条 SKIP 逐条相同，按理由归组（`integration-magma-full-split` 口径）：

| 条数 | 理由（截断） | 归属 |
|---|---|---|
| 21 | `dedicated Magma run-ahead lane only` / `dedicated Magma cache lane only` | 车道标记：这两组只在 tier 1 的 inproc 臂跑（§6 的债） |
| 5 | `GL_MAX_IMAGE_SAMPLES is 0, so the conformance case substitutes a plain 2D image` | lavapipe 能力 |
| 5 | `C-1's per-level respecify is consumed by DirectGLES only; backend is DirectVulkan` | Espryt 专属（P3b/P4b） |
| 4 | `DirectGLES only: this scenario is about Espryt's own reachability table` | Espryt 专属 |
| 3 | `three-channel widening is a DirectGLES substitution` | Espryt 专属 |
| 3 | `F-3 is a DirectGLES handle-arm seam` | Espryt 专属 |
| 2 | `the signed-normalized substitution is a DirectGLES fallback` | Espryt 专属 |
| 2 | `subgroup probe requires GL_SUBGROUP_SIZE_KHR in [16, 256]` | lavapipe 能力 |
| **2** | **`object_death is produced by the DirectGLES death-notice ops; DirectVulkan has none until P7`** | **wave 2 包 C 的债，见 §6** |
| 1 | `this driver cannot host separate DEPTH_COMPONENT24 and STENCIL_INDEX8 attachments` | lavapipe 能力（`DepthStencilReadbackMatrixScenario.SeparateDepthAndStencil…`） |
| 2 | `renderer Magma … clips by a DISABLED gl_ClipDistance` / `… regardless of the enables` | 已知 Magma 语义缺口（tier 1 同两条） |
| 2 | `known: a non-trailing unsized array overlaps…` / `two runtime arrays in one block overlap` | 已知（SSBO 声明形状） |
| 1 | `atomic-counter draws do not paint on DirectVulkan yet` | 已知 Magma 缺口 |
| 4 | `this case needs the <emulation/reroute/demotion> pinned ON for the whole process` ×3、`MOBILEGL_ASYNC_SHADER_COMPILE is unset` ×1 | 各自专属车道 |
| 5 | `not the strict-arming lane` / `not the counting lane` / `requires the dedicated per-frame stats lane` / `runs only in its own lane` / `optimistic-status quirk` | 各自专属车道 |

**注意 `DepthStencilReadbackMatrixScenario.SeparateDepthAndStencilAttachmentsAreBothReadable` 是 SKIP 而不是
红**：审计预测它会撞 `multisample-blit-aspect@P7`，实际上在 lavapipe 上它连 framebuffer 都组不起来，先一步
skip 了。这就是「主机拿不到那份红名单」的一个具体样本——它需要一台能做 separate D/S 附件的真设备。

## 5. 红名单：全量普查在 1530 条里只产出 1 条，而它是 stream 数据面的

| 组 | 条数 | 首阻塞 | 分类 |
|---|---|---|---|
| **S1 曝光 texel（tcp / stream 专属）** | 1 | `Scenarios/IterationRPProgram203Scenario.cpp:371: complete Program 203 output differs at 0,0: actual half bits=(0x357a, 0x4b65) golden half bits=(0x3a74, 0x4821); mismatched 1 of 262656 texels`，另有 `:377` 的第二条断言。**两个角色日志里都没有 `Fatal{` / `Refuse{`** | **错误像素**（不是 `@P7` 中止、不是超时、不是管道） |

**为什么判它是真缺陷而不是车道管道，四条证据**：

1. **确定性**：tcp 臂连跑 5 次，5 次全红，同一个 texel、同一组 bits。
2. **只在 tcp**：同一用例在 `DirectVulkan.Split.Full.`（inproc）、`DirectVulkan.Spawn.Full.`（spawn，**数据面
   是 shm**）与 monolith `DirectVulkan.` 各连跑 5 次，**各 5/5 全绿**。三臂唯一的差别是传输片段，
   而 spawn 与 tcp 的差别只有 `MOBILEGL_IPC_CONTROL=tcp://…` + `MOBILEGL_IPC_DATA=stream`。
   ⇒ **嫌疑落在 stream 数据面，不落在「两进程」本身。**
3. **GLES 的 tcp 车道是绿的**（`integration-tcp` 104/104），同一轮同一台机器 ⇒ 不是 fixture、不是端口、
   不是 supervisor。
4. **差的是第 0 号 texel**，而这个位置正是该用例自己打印为 `actualExposureBits` 的**曝光值**
   （`IterationRPProgram203Scenario.cpp:363-370`）；其余 262655 个 texel 逐位相同 ⇒ 不是整幅画错，是
   **归约/曝光那一条链**上的一个值。

**归簇**：不属于 A（UniformManager 对齐）/ B（blit-copy-mip）/ C（顶点 / 死亡表 / resident / 反射）中的任何
一个——它是 **P6.5 stream 传输 × Magma 归约链**的交叉项。建议作为 **wave 2 的一条独立小项**派给
stream 数据面的 owner，或先在 wave 1 的设备窗口里确认它在真机 tcp 上是否复现。

**本包没有去修它**（超出「只修管道」的授权），也没有把它从普查里排除——它就该红在那里。

### 5.1 第一轮（tier 3 带着 `MGITEST_SPLIT_LANE=1`）的 14/15 条红，全部是车道管道，已修

留档，因为它们是「普查需要自己的环境」这条结论的证据：

| 组 | 条数（split/spawn/tcp） | 首阻塞 | 处置 |
|---|---|---|---|
| F1 fixture 记录序号断言 | 13 / 13 / 13 | `Harness/ScenarioFixture.h:90: this case ran in an armed DirectGLES.Split. lane and the client encoder's record ordinal did not move: EmitSeq was 20 at SetUp and is 20 now` | tier 3 不再设 `MGITEST_SPLIT_LANE=1`（§2.1-1） |
| F2 dual-block 负控 | 1 / 1 / 1 | `Scenarios/DualBlockScenario.cpp:63: before.roleSplitActive Actual: false Expected: true … MOBILEGL_IPC_ROLE_SPLIT_STATE is not armed` | tier 3 过滤掉 `DualBlockScenario.*`（§2.1-2） |
| F3 server 计数器未发布 | 0 / 1 / 0 | `Scenarios/CtWireScenario.cpp:131: counters.valid Actual: false Expected: true  server control-record telemetry missing` | tier 3 的 CtWire 条目补 `MOBILEGL_TEST_SERVER_COUNTERS=1`（遥测开关，不是断言旋钮） |

## 6. 死亡通知前提（交付 3）：修好了，而且当场就有效

**改动**：`WireTables.cpp` 的安装条件从
`Transport == Spawn && ActiveBackendType == DirectGLES && GetStateObjectDeathOps() == nullptr`
去掉中间一条。保留的两条正是 P6.5 原意的两半。

**R-16（安装点是承重的）**：把安装点临时短路掉（`if (false && …)`）重编，两条 **DirectGLES spawn** 死亡用例
立刻红：

```
CtWireScenario.cpp:187: Failure  Expected: (afterCounters.deaths) > (deathsBefore), actual: 0 vs 0
CtWireScenario.cpp:246: Failure  Expected: (afterCounters.deaths) > (deathsBefore), actual: 0 vs 0
0% tests passed, 2 tests failed out of 2
```

这正是计划 §1.3 (2) 描述的「静默 `ObjectDeaths=0`」的形状，而车道抓得住它。已还原。

**Magma 下实际观测到什么**：`CtWireScenario` 自己在非 DirectGLES 上 `GTEST_SKIP`（`:152-155` / `:208-211`），
所以三档里这两条都是 **SKIP**。做了一次**临时、未提交**的探针（去掉那两处 skip 守卫重编，跑完还原并重编）：

| 臂 | 结果 |
|---|---|
| `DirectVulkan.Spawn.…Death…`（spawn） | **两条 PASS**（`deaths > deathsBefore` 成立） |
| `DirectVulkan.Split.…Death…`（inproc） | **两条 FAIL** |

即：**本包的前提修复让 Magma 的 spawn / tcp 路径今天就能跨 `object_death`**；计划里「Magma 下计数可能仍然
是 0」的预期只在 **inproc 臂**成立（inproc 靠后端自己的 `StateObjectDeathOps`，Magma 还没有那张表 = wave 2
包 C）。

→ **裁定项**：`CtWireScenario` 的 skip 守卫（`:152`/`:208`）现在**过宽**，可窄化为「非 DirectGLES **且**
transport 非 spawn/tcp 才 skip」，一行代价，收益是 Magma 两进程死亡路径立刻有门。本包**没有**改它：那是
wave 2 包 C 的文件，且改了会让 inproc 臂变红（需要同时决定 inproc 臂怎么处置）。

## 7. CI

**(a) retrace-split 的五个旋钮**（两次提交合起来）。该作业过去**一个 Magma 旋钮都不导出**。现已按与 monolith
`retrace` 作业（`test.yml:2180-2196`）**逐字相同**的守卫补齐：

- `MOBILEGL_MAGMA_R11G11B10F_FALLBACK`（全部 DirectVulkan 行）
- `MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE`（`improved-transparency-minecraft-26.3`）
- 三个 iterationRP 修复（`minecraft-1.21.4-fabric-iris-iterationrp-in-world`）

`split-matrix` 实测 154 格，含上述 case × DirectVulkan × {inproc, spawn}。`retrace-verify` 按裁定未动。

**(b) 门控标签**。两条新标签实测全绿，因此入门控：

- `Integration-magma-spawn and -tcp - the same cases in two processes (P7 L)`：
  `ctest -L '^integration-magma-spawn$'` 与 `'^integration-magma-tcp$'`，job 环境与既有 magma-split 一步相同。
- `Magma application-buffer wire cases must all execute on the spawn and tcp arms`：对
  `integration-magma-{spawn,tcp}-buffers` 按**名字** `require_green` 那 19 条（tcp 臂的必需名单里**显式包含**
  `TcpServer.Start` / `TcpServer.Stop`）。

两步都在本机**实跑过**：spawn `19 PASS, 0 skip, 0 failed`，tcp `21 PASS, 0 skip, 0 failed`。整个 workflow 的
**99 个 `run:` 块**全部 `bash -n` 通过。

**信息档与全量档共六条标签刻意不入任何门控步骤**；升档规则写在 CMake 注释里：一个用例随着**退役它的那个
提交**从下一档搬进门控档。

## 8. 车道自身的 red-once（交付 6）

把 spawn 臂的第二个进程拿掉（`mv build-split/libMobileGLServer.so ...redonce-bak`）：

```
# 门控档
DirectVulkan.Spawn.Fm.F1WireScenario.ClearBufferfiPixels ...***Failed
client log: MG_Remote spawn: the server image is not executable:
  "/home/swung/w7/p7-lane/build-split/libMobileGLServer.so" - refusing. Set MOBILEGL_IPC_SERVER_PATH,
  or place libMobileGLServer.so beside libMobileGL.so. There is NO monolith fallback from here.
client log: MG_Remote: the spawn session failed to start (rc=2); MobileGL will NOT fall back to
  monolith - a lane named split that ran monolith is the one failure this phase is built to make impossible

# 信息档
0% tests passed, 3 tests failed out of 3   (DirectVulkan.Spawn.TriangleScenario.*)

# 同一时刻，inproc 臂不受影响（破坏是传输特定的）
DirectVulkan.Split.Fm.F1WireScenario.ClearBufferfiPixels ... Passed
```

还原后即刻回绿。**失败是具名的**：ctest 的断言文本停在 `eglInitialize failed`（pre-flight 子进程），而库
自己的通道（私有角色日志）里是上面那条 `refusing` 行——这正是 `split_log_paths.py` 反复强调的「ctest 的
transcript 对库输出是 false zero」。

## 9. 名集合与 parity（交付 7）

```
$ python3 scripts/ci/spawn_lane_parity.py build-split
spawn-lane parity: split 105 (102 comparable), spawn 102          # 与基线逐字相同
integration-tcp parity: spawn 102, tcp 102                        # 与基线逐字相同
magma gated tier:          73 / 52 / 52  after normalisation（21 条 inproc-only 具名豁免）
magma informational tier: 102 / 102 / 102 after normalisation
magma full-suite tier:    510 / 510 / 510 after normalisation
RC=0
```

Magma 档的归一化**只脱臂段**（`<Backend>.<Arm>.<其余>` → `<Backend>.<其余>`），**不脱**子车道尾巴：
`RunAhead.Credit1.` 与 `RunAhead.Credit3.` 是同一个用例的两条条目。inproc-only 豁免是**逐档**的：门控档里
`MagmaRunAheadScenario` / `MagmaWireCacheScenario` 只注册在 inproc 臂（§10 的债），全量档里它们三臂都在
（只是各自 SKIP），所以在全量档豁免它们反而会造出假的 `extra`。

**G14（名字只增不减）**，对 `~/w7/p7-before/ctest-names-split.txt`：

```
before=4155  after=6095   removed=0   added=1940
added by prefix:  DirectVulkan.Split. 612   DirectVulkan.Spawn. 664   DirectVulkan.Tcp. 664
```

1940 = (52+102+510) spawn + (52+102+510) tcp + (102+510) split。**removed = 0。**

## 10. 明写的例外与债

**`MagmaRunAheadScenario`（14）与 `MagmaWireCacheScenario`（7）在门控档里只注册 inproc 臂。** 第一次把三臂
加给全部六条 Magma 车道时，spawn 与 tcp 各红 21 条，两臂逐条相同，**两个角色日志里都没有任何 `Fatal{`**：

- `MagmaRunAheadScenario.cpp:134: Value of: WaitForSplitApplyHoldForTesting() Actual: false Expected: true`
  ——`HoldNextBatch` 经 `ArmSplitApplyHoldForTesting()` 把钩子装到
  `ServerLoopInstance().SetBeforeRetireHookForTesting`（`Harness/SplitRuntimePeek.cpp:43-50`），**本进程**的
  server loop；spawn/tcp 下它在另一个进程里。
- `MagmaWireCacheScenario.cpp:53: runtime.transportName Which is: "non-monolith" / "inproc"`——`SetUp` 直接
  断言 inproc。

处置：回到单臂注册，理由写进 CMakeLists，并在 `spawn_lane_parity.py` 的 `MAGMA_INPROC_ONLY` 里**具名豁免**
（照抄 `MONOLITH_ONLY` 的纪律）。**债**：两进程口径下的 run-ahead 与 cache 身份**今天没有覆盖**，需要一个
**跨进程**的 hold（server 侧旋钮）和跨进程 cache peek——归 wave 2-B/C 的 owner 裁定。

## 11. 相邻门（本机实跑，证明没有回归）

| 门 | 结果 |
|---|---|
| `SplitLogPaths.PrivateAndDistinct` / `.ResultAccountingControls` | 绿 |
| dual-block 普查步骤（`-L 'integration-split\|integration-magma-split'` + `expect-fatal`） | `259 entrie(s) - 257 green, 2 skipped, 0 red (255 private logs scanned)`，`ratchet OK - 0 fatal pair(s)` |
| `integration-p5f-rsp` 的 `require_green` | `2 PASS, 0 skip, 0 failed` |
| `scripts/ci/magma_cache_checks.py --execute`（exact-seven + 环境不变式） | 7/7 `verdict=GREEN` |
| `scripts/ci/magma_runahead_checks.py negative --execute` | `run_ahead_0: EXPECTED_RED` → `restored_run_ahead_1: GREEN` |
| `integration-magma-{buffers,runahead,caches}` 锚定标签 | 19 / 14 / 7，全绿，与基线相同 |
| 六条 Magma 车道的 ICD 钉（裁定后） | 73/73 条带 `VK_ICD_FILENAMES` 与三个 iterationRP 变量；`DirectGLES.Split.NamedBlit.` 两条**不带** |

## 12. 没有验证的（明写）

- **真机**：一次都没有跑（全部在 lavapipe 主机）。出口门 3 的真机 ssim 归 wave 1。**§4 的若干 SKIP（分离
  D/S 附件、subgroup 探针、`GL_MAX_IMAGE_SAMPLES=0`）只有真机才能变成真结果。**
- **retrace**：没有跑 retrace-split 矩阵，五个旋钮只验证了 YAML + shell 语法与 matrix 里确实有那些格，
  **没有**验证加上旋钮后 `iterationrp × DirectVulkan × {inproc,spawn}` 会变绿或变红。
- **§5 那条红的根因**：只做到「确定性、tcp/stream 专属、曝光 texel」，没有下钻到具体的记录或回读。
- **pull 构建 / G1**：本包改的是 CMake 注册、`MG_Remote` 下的一处 client 文件（pull 构建不编译它）与
  CI/脚本；**没有**重跑 pull `.text` 恒等。合并前请由集成者跑一次 G1。
- **`MOBILEGL_ITEST_TCP_DEVICE_ENDPOINT`**：本树为空，`TcpDevice` 臂未配置，Magma **不给** TcpDevice 臂
  （一台手机、串行）——明写的决定，不是遗漏。
- **CI runner 上的耗时**：本机 tier 3 三条合计约 2 分 6 秒（`-j 8`，其中 tcp 臂 91 s，因 fixture 串行），
  CI runner 未测。

## 13. 给集成者的发现（按重要性）

1. **出口门 1 的主机半边只欠一条，而且不是 `@P7`。** 全量普查 1530 条里 **0 条**触发树上 15 处 `@P7` 具名
   拒绝；唯一的红是 §5 的 stream 曝光 texel。计划把 wave 2 的 A/B/C 三簇建立在「首轮红名单」之上——这份
   名单在主机 × lavapipe 口径下是空的。**建议**：把红名单来源改挂到 wave 1 真机 + retrace 全矩阵 +
   P3b/P4b 场景普查；本车道的职责改为**回归门**，并把 §5 作为 stream 数据面的独立小项派出去。
2. **§5 的 stream 曝光 texel**：确定性、tcp 专属、与 A/B/C 三簇都不同族。需要一位 owner。
3. **`CtWireScenario` 的 skip 守卫过宽**（`:152`/`:208`）：Magma **spawn/tcp** 的死亡路径今天实测可通（§6），
   只有 inproc 臂还等 wave 2 包 C 的 `StateObjectDeathOps`。一行窄化即可让两进程死亡路径有门。
4. **两进程 run-ahead / cache 身份今天无覆盖**（§10 的债）。
5. **tier 3 放弃了两条自证**（记录过线证明、tcp 拓扑证明，§2.1-1）。若将来要把某条全量用例升进门控档，
   必须连同 `MGITEST_SPLIT_LANE=1` 一起升——门控档的价值有一半在这两条自证上。
6. **`DUALBLOCK_ALLOWED_SKIPS` 只收录 Split 拼写**（`split_log_paths.py:156-157`）。如果将来把
   magma-spawn/-tcp 加进 dual-block 普查的选择式，需要同时补 `DirectVulkan.{Spawn,Tcp}.Fm.ClipDistance…`
   两条，否则那一步会以「unexpected skip」红。

## 14. 复现

```bash
cd ~/w7/p7-lane
cmake -S . -B build-split -DMOBILEGL_BUILD_INTEGRATION_TEST=ON \
  -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json \
  -DMOBILEGL_ITEST_TCP_ENDPOINT=tcp://127.0.0.1:40713
ninja -C build-split -j 20
python3 scripts/ci/spawn_lane_parity.py build-split
cd build-split
# 门控档（CI 口径）
MOBILEGL_ITEST_REQUIRE_GPU=1 MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_ROLE_SPLIT_STATE=1 \
  ctest -L '^integration-magma-spawn$' --no-tests=error -j 2 --output-on-failure
# 全量普查（一条一条跑，-j 8）
MOBILEGL_ITEST_REQUIRE_GPU=1 ctest -L '^integration-magma-full-split$' --no-tests=error -j 8 --output-on-failure
MOBILEGL_ITEST_REQUIRE_GPU=1 ctest -L '^integration-magma-full-spawn$' --no-tests=error -j 8 --output-on-failure
MOBILEGL_ITEST_REQUIRE_GPU=1 ctest -L '^integration-magma-full-tcp$'   --no-tests=error -j 8 --output-on-failure
# §5 的那一条
MOBILEGL_ITEST_REQUIRE_GPU=1 ctest -V -R '^DirectVulkan\.Tcp\.Full\.IterationRPProgram203Scenario\.'
```
