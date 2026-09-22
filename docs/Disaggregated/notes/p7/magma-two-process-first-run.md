# P7 wave 0 包 L：Magma 两进程车道的首轮（2026-09-22）

> 计划见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §0 发现 1 / §1.3 出口门 1 / §3 wave 0 第 1 项。
> 基线 `feat/disaggregated@78b7d6be`，分支 `p7/lane`，主机 WSL Arch + lavapipe
> (`/usr/share/vulkan/icd.d/lvp_icd.json`，系统唯一 ICD)，`build-split` = Release / clang / ccache /
> `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON` `BUILD_INTEGRATION_TEST=ON`，
> `MOBILEGL_ITEST_TCP_ENDPOINT=tcp://127.0.0.1:40713`（本树专用端口，避免与兄弟树的 40613 撞车）。

## 0. 一句话结论

**仪器已经存在，而首轮不是红的。** `mgl_itest_register_split_arms` 现在带后端维度，Magma 的六条手铺车道
各自长出 spawn / tcp 两条臂，另有一档信息性车道把 DirectGLES split 臂跑的**每一个**场景都在 DirectVulkan ×
{inproc, spawn, tcp} 上跑一遍。在 lavapipe 主机上，**这 410 条新条目 0 红**。

因此本包交付的不是「红名单 = P7 工作清单」，而是三件比它更硬的事实：

1. **出口门 1 的主机半边今天就是绿的**：Magma × 两进程，73 + 52 + 52 + 102 × 3 条目，零失败。计划 §1.3
   假设的红名单在主机口径下是**空集**；P7 剩下的 `@P7` 拒绝退役必须靠**真机**（wave 1）、**retrace 矩阵**
   与**更宽的场景普查**（P3b/P4b wave 2-D）去触发，而不是靠这条集成车道。这条判断本身就值一次裁定。
2. **两条 Magma 车道在结构上就跑不了两进程**，首轮如实红了 21 条并已按仪器纪律收回单臂（§3）。
3. **死亡通知的前提条件修好之后，Magma spawn 的 `object_death` 当场就通了**（§5）——比计划预期的
   「Magma 仍然 0」更进一步，且把剩余缺口精确定位到 **inproc 臂**。

## 1. 交付物与文件

| 交付 | 文件 | 要点 |
|---|---|---|
| 后端维度 | `MobileGL/MG_IntegrationTest/CMakeLists.txt` | `mgl_itest_register_split_arms_for_backend(<backend> <filter> <tail> [stems])` + 同名薄包装保持 32 个调用点不变；`MGL_ITEST_VULKAN_{SPLIT,SPAWN,TCP}_ENVIRONMENT`；`mgl_itest_magma_arm_shape()` 供六条手铺车道复用同一套臂形状 |
| 两档标签 | 同上 | 见 §2。CMake 注释里写死了两档的定义、升档规则与标签拼写的正则理由 |
| 死亡通知前提 | `MobileGL/MG_Remote/Client/WireTables.cpp` | 去掉 `ActiveBackendType == DirectGLES`，保留 `Transport == Spawn` 与 `GetStateObjectDeathOps() == nullptr` 两条（P6.5 原意的两半） |
| 私有日志纪律 | `MobileGL/MG_IntegrationTest/Harness/split_log_paths.py` | `is_split` 前缀补 `DirectVulkan.Spawn.` / `DirectVulkan.Tcp.` |
| 名集合门 | `scripts/ci/spawn_lane_parity.py` | 新增 Magma 两档的三臂比对 + 两条具名豁免 |
| CI | `.github/workflows/test.yml` | retrace-split 补三个 iterationrp 旋钮；新增 magma-spawn/-tcp 门控步骤与 buffers 子标签的 `require_green` |

## 2. 两档车道（这是本包的主结构）

**第一档，门控。** `integration-magma-split`（inproc，**含义不变**：同样 73 条、同样的名字、同样的标签、
同样的私有日志路径）＋本包新增的 `integration-magma-spawn` / `integration-magma-tcp`。跑的是六条**手铺**
Magma 车道（Fm / Buffers / RunAhead / Caches / NamedBlit / P5fRsp），因为每条都带自己的 flavour（strict +
dual-block、adopt tier、run-ahead credit、cache pin），共享的场景过滤器表达不了。**一条标签只有在实测绿之后
才准进门控步骤**——本包两条都实测绿，所以进了（§6）。

**第二档，信息性。** `integration-magma-all-{split,spawn,tcp}`：DirectGLES split 臂跑的每一个场景，在
DirectVulkan 上，三条传输各一遍。**不在任何 CI 门控步骤里**，它的职责就是**能红而 CI 仍绿**。它由宏记录的
调用列表**回放**生成（`MGL_SPLIT_ARM_CALLS`，32 次调用），所以明天给 split 臂加一个场景，Magma 普查当天
跟着长——这是 P6 `t6` 给 spawn 臂赢下的同一条论证，往后端方向再走一维。

三条臂里**包含 inproc**，这是它的诊断半边：spawn/tcp 红而 inproc 绿 = 两进程缺陷；三条都红 = Magma 自己缺
动词，归 wave 2 某个簇。没有 inproc 臂的话每一条红都要手工复跑一次才能分类。

**标签拼写是按 `ctest -L` 的正则选的**（ID-131 的规矩，本文件已经为它付过两次学费）：
`-L integration-magma-spawn` **不得**选中第二档，所以档位词放**中间**——`integration-magma-all-spawn` 不含
`integration-magma-spawn` 子串，而显然的 `integration-magma-spawn-all` 含，会把 ~100 条预期可红的条目悄悄
拖进门控车道。六种拼写双向都查过。子标签同理：inproc 臂保留历史名 `integration-magma-buffers`（CI 那一步
按名字 require_green 它的 19 条），其余臂把臂词放**前面**（`integration-magma-spawn-buffers`），两个选择
互相够不着。

## 3. 首轮结果（逐车道）

统计口径：`ctest --output-junit` 的 testcase，PASS / FAIL / SKIP 按 JUnit 判定；tcp 车道的条目数含
`TcpServer.Start` / `TcpServer.Stop` 两条 fixture（每条 tcp 条目都 `FIXTURES_REQUIRED mobilegl-tcp`，
ctest 会把它们一起列进标签）。门控三档按 CI 的 job 环境跑（`MOBILEGL_ITEST_REQUIRE_GPU=1`
`MOBILEGL_IPC_STRICT_ERRORS=1` `MOBILEGL_IPC_ROLE_SPLIT_STATE=1`，`-j 2`）；信息档与 GLES 档按普通口径
（`REQUIRE_GPU=1`，`-j 8` / `-j 4`）。

| 车道 | 条目 | PASS | FAIL | SKIP | 与基线比 |
|---|---|---|---|---|---|
| `integration-magma-split` | 73 | 71 | **0** | 2 | 73，与基线**逐条相同** |
| `integration-magma-spawn` | 52 | 50 | **0** | 2 | 新增 |
| `integration-magma-tcp` | 54（52 + 2 fixture） | 52 | **0** | 2 | 新增 |
| `integration-magma-all-split` | 102 | 100 | **0** | 2 | 新增（信息性） |
| `integration-magma-all-spawn` | 102 | 100 | **0** | 2 | 新增（信息性） |
| `integration-magma-all-tcp` | 104（102 + 2 fixture） | 102 | **0** | 2 | 新增（信息性） |
| `integration-split`（GLES） | 186 | 186 | 0 | 0 | 186 / 102 comparable，与基线相同 |
| `integration-spawn`（GLES） | 102 | 102 | 0 | 0 | 与基线相同 |
| `integration-tcp`（GLES） | 104 | 104 | 0 | 0 | 与基线相同 |

**六条 Magma 车道的 SKIP 全部是具名的、两种**：

- 门控三档的 2 条是 `ClipDistanceScenario.{ADisabledClipDistanceRemovesNothing,
  TheEnablesAreIndependentPerDistance}`，理由「lavapipe clips by a DISABLED gl_ClipDistance …」——inproc
  臂上 `split_log_paths.py` 的 `DUALBLOCK_ALLOWED_SKIPS` 已经收录了同两条的 Split 拼写（`:156-157`）。
- 信息档的 2 条是 `CtWireScenario.{Texture,Framebuffer}DeathCrossesAndTheRecycledSlotAnswersTheNewObject`，
  理由 `object_death is produced by the DirectGLES death-notice ops; DirectVulkan has none until P7`
  （`Scenarios/CtWireScenario.cpp:152-155` / `:208-211`）。**这两条就是 wave 2 包 C 的债，见 §5。**

**arming 证据**（不是「跑了没红」，是「确实跑了两个进程、确实是 Magma」）：483 条 DirectVulkan split 家族
条目**每条恰好一个**私有绝对日志路径（`SplitLogPaths.PrivateAndDistinct` 绿），落盘 306 对
`.client.log` / `.server.log`；任取一对读到

```
client: Config: Active backend type set to DirectVulkan
client: Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream crosses a real ring to an apply thread in ANOTHER PROCESS
client: MG_Remote client: spawn ARMED - the server role runs in pid 130832, reached at "@mgl-130770-106887814518704"
server: MG_Remote server: pid=130832 transport=spawn role=server ready control=unix data=shm
server: Config: Active backend type set to DirectVulkan
```

## 4. 红名单：首轮唯一的两组，都是**车道管道**，都已按仪器纪律处置

第一次把三条臂加给全部六条 Magma 车道时，spawn 与 tcp 各红 **21** 条，两臂**逐条相同**。按「Fatal 家族 +
消息 + 哪个角色日志」分组的首阻塞如下——**两组都不是 server 以 `@P7` 名字中止，不是错误像素，不是超时挂起**，
而是**场景本身只能在进程内跑**：

| 组 | 条数 | 首阻塞 | 角色日志 | 归类 |
|---|---|---|---|---|
| **P1 run-ahead hold** | 14（`MagmaRunAheadScenario.*` × credit 1/3） | `MagmaRunAheadScenario.cpp:134: Value of: WaitForSplitApplyHoldForTesting() Actual: false Expected: true` + `the clear waited for apply instead of returning while its server batch was held`，随后 `:170` `HoldNextBatch() ... generates new fatal failures`。**两个角色日志里都没有任何 `Fatal{` / `Refuse{`** | `magma-runahead-{spawn,tcp}-credit{1,3}-*.{client,server}.log` | 车道管道 |
| **P2 cache peek** | 7（`MagmaWireCacheScenario.*`） | `MagmaWireCacheScenario.cpp:53: Expected equality of these values: runtime.transportName Which is: "non-monolith" / "inproc"`。同样**零 Fatal** | `magma-caches-{spawn,tcp}-*.{client,server}.log` | 车道管道 |

**根因，逐行**：

- P1：`MagmaRunAheadScenario::HoldNextBatch` 经 `ArmSplitApplyHoldForTesting()` 把钩子装到
  `MG_Remote::Server::ServerLoopInstance().SetBeforeRetireHookForTesting(&HoldOneAppliedBatch)`
  （`Harness/SplitRuntimePeek.cpp:43-50`）——**本进程**的 server loop。spawn/tcp 下 server loop 在另一个
  进程里，钩子装在一个永不运行的实例上，等待必然超时。
- P2：`MagmaWireCacheScenario::SetUp` 直接 `ASSERT_EQ(runtime.transportName, "inproc")`
  （`MagmaWireCacheScenario.cpp:53`），整条车道读的是**本进程** server 的 view / attachment 身份。

**处置**：两条车道回到**单臂（inproc）注册**，理由写在 CMakeLists 的两段注释里，并在
`spawn_lane_parity.py` 的 `MAGMA_INPROC_ONLY` 里**具名豁免**（照抄 `MONOLITH_ONLY` 的纪律：宁可点名，
也不放宽容差）。这与本文件已有的四条 DirectGLES 单臂车道（DualBlock / StrictProgramArm /
PersistentMapArm / StrictArming）是同一条规矩：**一个因为「不是它存在的理由」而必然红的条目，比没有这个
条目更糟**（R-16 的镜像）。

**由此产生的债（不是本包能关的）**：两进程口径下的 run-ahead 与 cache 身份**今天没有覆盖**。要覆盖，需要
一个**跨进程**的 hold（server 侧旋钮，而不是函数指针）和一个跨进程的 cache peek。这两件都该由 wave 2-B/C
的 owner 决定要不要做——**本包不擅自做**，只把缺口写在这里。

## 5. 死亡通知前提（交付 3）：修好了，而且当场就有效

**改动**：`WireTables.cpp` 的安装条件从

```cpp
Transport == Spawn && ActiveBackendType == BackendType::DirectGLES && GetStateObjectDeathOps() == nullptr
```

去掉中间一条。保留的两条正是 P6.5 原意的两半：`Transport == Spawn` = 「这是没有本地孪生表的远端 client」；
`GetStateObjectDeathOps() == nullptr` = 「谁先装谁算，inproc 保留后端自己的派发器」。

**R-16（安装点是承重的）**：把安装点临时短路掉（`if (false && …)`）重编，两条 **DirectGLES spawn** 死亡用例
立刻红：

```
CtWireScenario.cpp:187: Failure  Expected: (afterCounters.deaths) > (deathsBefore), actual: 0 vs 0
CtWireScenario.cpp:246: Failure  Expected: (afterCounters.deaths) > (deathsBefore), actual: 0 vs 0
0% tests passed, 2 tests failed out of 2
```

这正是计划 §1.3 (2) 描述的「静默 `ObjectDeaths=0`」的形状，而车道抓得住它。已还原。

**Magma 下实际观测到什么**（计划要求「记录你观察到的」）：`CtWireScenario` 自己在非 DirectGLES 上
`GTEST_SKIP`（`:152-155` / `:208-211`），所以门控与信息档里这两条是 **SKIP**，不是 0 也不是绿。
为了给集成者一个真数，做了一次**临时、未提交**的探针（把那两处 skip 守卫去掉重编，跑完还原并重编）：

| 臂 | 结果 |
|---|---|
| `DirectVulkan.Spawn.Ct.CtWireScenario.{Texture,Framebuffer}Death…` | **两条 PASS**（`deaths > deathsBefore` 成立） |
| `DirectVulkan.Split.Ct.…`（inproc） | **两条 FAIL** |

也就是说：**本包的前提修复让 Magma 的 spawn / tcp 路径今天就能跨 `object_death`**（对象作为 wire 对象已经
过线，`MGPipeSlots().FindByLifetimeId` 解析得到句柄），计划里「Magma 下计数可能仍然是 0」的预期只在
**inproc 臂**成立——inproc 靠的是后端自己的 `StateObjectDeathOps`，而 Magma 还没有那张表（wave 2 包 C）。

→ **给集成者的一条裁定项（ID 待编）**：`CtWireScenario` 的 skip 守卫（`BackendName() != "DirectGLES"`）现在
**过宽**，可以窄化为「非 DirectGLES **且** transport 非 spawn/tcp 才 skip」，代价是一行，收益是 Magma 两进程
死亡路径立刻有门。本包**没有**改它：那是 wave 2 包 C 的文件，且改了会让信息档的 inproc 臂变红（需要同时
决定 inproc 臂怎么处置）。

## 6. CI（交付 4）

**(a) retrace-split 的三个旋钮**。`test.yml` 的 `retrace-split` 作业「Retrace and validate under
MOBILEGL_TRANSPORT」一步过去**一个 Magma 旋钮都不导出**，而 monolith `retrace` 作业（`:2180-2196`）对
`minecraft-1.21.4-fabric-iris-iterationrp-in-world × DirectVulkan` 导出三个 iterationRP 修复。已按**逐字
相同**的 backend+case 守卫补上三个旋钮。`split-matrix` 实测 154 格，含该 case × DirectVulkan × {inproc,
spawn} 两格——也就是这一步过去是唯一**无法**与 monolith 对照的两格。

**(b) 门控标签**。两条新标签首轮全绿，因此按交付要求入门控：

- 新步骤 `Integration-magma-spawn and -tcp - the same cases in two processes (P7 L)`：
  `ctest -L '^integration-magma-spawn$'` 与 `'^integration-magma-tcp$'`，job 环境与既有 magma-split 一步相同。
- 新步骤 `Magma application-buffer wire cases must all execute on the spawn and tcp arms`：对
  `integration-magma-{spawn,tcp}-buffers` 按**名字** `require_green` 那 19 条（tcp 臂的必需名单里**显式包含**
  `TcpServer.Start` / `TcpServer.Stop`，因为 ctest 会把 fixture 一起列进标签，而 supervisor 没起来的车道对
  那 19 条无话可说）。

两步都在本机**实跑过**（不只是 `bash -n`）：spawn 19/19 + `require-green: 19 PASS, 0 skip, 0 failed`，
tcp 19/19 + `require-green: 21 PASS, 0 skip, 0 failed`。整个 workflow 的 **99 个 `run:` 块**全部 `bash -n` 通过。

**信息档三条标签刻意不入任何门控步骤**，CMake 注释与上面的 CI 注释都写明了升档规则：一个用例随着**退役它的
那个提交**从第二档搬进第一档。

## 7. 车道自身的 red-once（交付 6）

把 spawn 臂的第二个进程拿掉（`mv build-split/libMobileGLServer.so ...redonce-bak`），三件事同时发生：

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

# 同一时刻，inproc 臂不受影响（破坏是传输特定的，不是「整个 Magma 挂了」）
DirectVulkan.Split.Fm.F1WireScenario.ClearBufferfiPixels ... Passed
```

还原后两条臂即刻回绿。**注意失败是具名的**：断言文本停在 `eglInitialize failed`（pre-flight 子进程），但
库自己的通道（私有角色日志）里是上面那条 `refusing` 行——这正是 `split_log_paths.py` 反复强调的
「ctest 的 transcript 对库输出是 false zero」。

## 8. 名集合与 parity（交付 7）

```
$ python3 scripts/ci/spawn_lane_parity.py build-split
spawn-lane parity: split 105 (102 comparable), spawn 102          # 与基线逐字相同
integration-tcp parity: spawn 102, tcp 102                        # 与基线逐字相同
magma gated tier: integration-magma-split has 73 case entrie(s), 73 after normalisation
magma gated tier: integration-magma-spawn has 52 case entrie(s), 52 after normalisation
magma gated tier: integration-magma-tcp  has 52 case entrie(s), 52 after normalisation
magma gated tier: 21 inproc-only entrie(s) excluded from the comparison by name
                  (MagmaRunAheadScenario., MagmaWireCacheScenario.)
magma informational tier: integration-magma-all-{split,spawn,tcp} 102 / 102 / 102 after normalisation
RC=0
```

Magma 档的归一化**只脱臂段**（`<Backend>.<Arm>.<其余>` → `<Backend>.<其余>`），**不脱**子车道尾巴：
`RunAhead.Credit1.` 与 `RunAhead.Credit3.` 是同一个用例的两条条目，脱掉尾巴会把它们折成一条，然后报一个
「数目相等」的假绿。GLES 档保持原样（脱 `F1.` / `Ct.`），数字与基线逐字相同。

**G14（名字只增不减）**，对 `~/w7/p7-before/ctest-names-split.txt`：

```
before=4155  after=4565   removed=0   added=410
added by prefix:  DirectVulkan.Spawn. 154   DirectVulkan.Tcp. 154   DirectVulkan.Split. 102
```

410 = (52 门控 spawn + 102 信息 spawn) + (52 门控 tcp + 102 信息 tcp) + 102 信息 split。**removed = 0。**

## 9. 相邻门（本机实跑，证明没有回归）

| 门 | 结果 |
|---|---|
| `SplitLogPaths.PrivateAndDistinct` / `.ResultAccountingControls` | 绿 |
| dual-block 普查步骤（`-L 'integration-split\|integration-magma-split'` + `expect-fatal`） | `259 entrie(s) - 257 green, 2 skipped, 0 red (255 private logs scanned)`，`ratchet OK - 0 fatal pair(s)` |
| `integration-p5f-rsp` 的 `require_green` | `2 PASS, 0 skip, 0 failed` |
| `scripts/ci/magma_cache_checks.py --execute`（exact-seven + 环境不变式） | 7/7 `verdict=GREEN` |
| `scripts/ci/magma_runahead_checks.py negative --execute` | `run_ahead_0: EXPECTED_RED` → `restored_run_ahead_1: GREEN` |
| `integration-magma-{buffers,runahead,caches}` 锚定标签计数 | 19 / 14 / 7，与基线相同 |

## 10. 没有验证的（明写）

- **真机**：一次都没有跑（本包全部在 lavapipe 主机）。出口门 3 的真机 ssim 归 wave 1。
- **retrace**：没有跑 retrace-split 矩阵，(a) 的三个旋钮只验证了 YAML + shell 语法与 matrix 里确实有那两格，
  **没有**验证加上旋钮后 `iterationrp × DirectVulkan × {inproc,spawn}` 会变绿或变红。
- **pull 构建 / G1**：本包只改 CMake 注册、一处 `MOBILEGL_BUILD_DISAGGREGATED` 之外的 client 文件
  （`WireTables.cpp` 在 `MG_Remote` 下，pull 构建不编译它）与 CI/脚本；**没有**重跑 pull `.text` 恒等。
  合并前请由集成者跑一次 G1。
- **`MOBILEGL_ITEST_TCP_DEVICE_ENDPOINT`**：本树为空，`TcpDevice` 臂未配置，Magma **不给** TcpDevice 臂
  （一台手机、串行，主机臂都还没绿之前翻倍没有收益）——这是一个明写的决定，不是遗漏。
- **信息档在 CI runner 上的耗时**：本机三条信息车道合计 24.5 s（`-j 8`），CI runner 未测。

## 11. 给集成者的发现（按重要性）

1. **出口门 1 的主机半边已经满足，红名单是空的。** 计划把 wave 2 的 A/B/C 三簇建立在「首轮红名单」之上；
   在主机 × lavapipe 口径下没有这样一份名单。`@P7` 的 15 处具名拒绝**没有一处**被这 410 条新条目触发。
   建议：把「红名单来源」改挂到 wave 1 的真机实验 + retrace 全矩阵 + P3b/P4b 的 ~20 个候选场景普查上，
   并据此重估 wave 2 三簇的规模；本车道的职责改为**回归门**（退役一条，把它从第二档搬进第一档）。
2. **`CtWireScenario` 的 skip 守卫过宽**（`:152`/`:208`）：Magma **spawn/tcp** 的死亡路径今天实测可通
   （§5），只有 inproc 臂还等 wave 2 包 C 的 `StateObjectDeathOps`。一行窄化即可让两进程死亡路径有门。
3. **两进程 run-ahead / cache 身份今天无覆盖**（§4 的债）：需要跨进程 hold 与跨进程 cache peek，属
   wave 2-B/C owner 的裁定。
4. **既有 Magma 车道不带 ICD 钉**（相邻缺陷，未修）：`MGL_ITEST_MAGMA_*_ENVIRONMENT` 家族拼的是
   `${MGL_ITEST_COMMON_ENV}`，**不是** `${MGL_ITEST_VULKAN_ENV}`，所以 monolith `DirectVulkan.` 臂有
   `VK_ICD_FILENAMES` 与 lavapipe 的三个 iterationRP 修复，而 `integration-magma-split` 的 73 条**没有**
   （`CMakeLists.txt` 内 Fm / Buffers / RunAhead / Caches / NamedBlit / P5fRsp 六处）。本机只装了 lavapipe
   一个 ICD，所以驱动是同一个，但**三个修复旋钮的取值不同**。本包**没有**改动它们（那会改变 73 条既有条目的
   环境，超出「含义不变」的承诺）；新的 `MGL_ITEST_VULKAN_{SPLIT,SPAWN,TCP}_ENVIRONMENT`（信息档用）按交付
   要求用了 `${MGL_ITEST_VULKAN_ENV}`。**因此两档之间存在一处已知的环境不对称**，请裁定是把六条车道对齐到
   `VULKAN_ENV`，还是把信息档降回 `COMMON_ENV`。
5. **`retrace-split` 还缺另外两个 monolith / verify 都有的旋钮**（相邻缺陷，未修）：
   `MOBILEGL_MAGMA_R11G11B10F_FALLBACK`（monolith `:2180`、verify `:2383` 都有）与
   `MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE`（`:2195` / `:2387`）。交付只点名了 iterationrp 的三个，
   故只补了三个；这两个同样让 split 与 monolith 不可比。
6. **`DUALBLOCK_ALLOWED_SKIPS` 只收录 Split 拼写**（`split_log_paths.py:156-157`）。如果将来把
   magma-spawn/-tcp 加进 dual-block 普查的选择式，需要同时补 `DirectVulkan.{Spawn,Tcp}.Fm.ClipDistance…`
   两条，否则那一步会以「unexpected skip」红。

## 12. 复现

```bash
cd ~/w7/p7-lane
cmake -S . -B build-split -DMOBILEGL_BUILD_INTEGRATION_TEST=ON \
  -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json \
  -DMOBILEGL_ITEST_TCP_ENDPOINT=tcp://127.0.0.1:40713
ninja -C build-split -j 20
python3 scripts/ci/spawn_lane_parity.py build-split
cd build-split
MOBILEGL_ITEST_REQUIRE_GPU=1 MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_ROLE_SPLIT_STATE=1 \
  ctest -L '^integration-magma-spawn$' --no-tests=error -j 2 --output-on-failure
MOBILEGL_ITEST_REQUIRE_GPU=1 MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_ROLE_SPLIT_STATE=1 \
  ctest -L '^integration-magma-tcp$' --no-tests=error -j 2 --output-on-failure
MOBILEGL_ITEST_REQUIRE_GPU=1 ctest -L '^integration-magma-all-spawn$' --no-tests=error -j 8 --output-on-failure
```
