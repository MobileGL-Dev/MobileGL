# P5f / f1 — 双块演练机制：落地报告

> 落地于 `feat/disaggregated`，代码提交 `cfcc12f7`（机制 + 测试 + 车道 + 脚本），
> 棘轮初版与本报告随文档提交。WSL 门树 `~/w7/p5f-f1`（branch `p5f/f1`），
> 对照基线树 `~/w7/p5f-base` @ `e75e00cb`。设计输入：`f0-dualblock-recon.md`。
> 旋钮 `MOBILEGL_IPC_ROLE_SPLIT_STATE` 默认关；关时两臂折成同一块，行为逐字节不变（G1 实测见下）。

---

## 1 落地内容

| 文件 | 内容 |
|---|---|
| `MobileGL/Config.h` | `IpcTable::RoleSplitState`（`MOBILEGL_IPC_ROLE_SPLIT_STATE`），只在 `MOBILEGL_BUILD_DISAGGREGATED` 闭包里 |
| `MobileGL/ConfigLoader.cpp` | `InitIpc()` 解析该旋钮；与 `MOBILEGL_PIPE_VERIFY` 互斥（强制关 + 一行 WARN，照 `RunAhead` 的形状）；进 IPC 汇总日志行 |
| `MobileGL/MG_Backend/MGPipe/PipeInputs.h` | `gPipeInputsClientBlock`（leak-at-exit，同 `gPipeInputs` 的论证）；`MGPipeRoleSplitActive()` / `MGPipeClientInputs()` / `MGPipeClientClearVerbBoundary()` / `MGPipeServerBlockNoteIdentity()` 声明；非 disagg 的 push 构建里 `MGPipeClientInputs()` 是折回 `gPipeInputs` 的 inline |
| `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp` | 四个函数的定义；**机关**：`CountBarrierPull` 在 `MGPipeRoleSplitActive()` 时无条件 `StrictBarrierPullFatal(field, verb, "MOBILEGL_IPC_ROLE_SPLIT_STATE=1")`——先于设障判定、先于 strict 旋钮、先于计数器；`MGPipeServerStampVerbBoundary` 末尾调 `MGPipeServerBlockNoteIdentity()` |
| `MobileGL/MG_Impl/Pipe/PipeFill.cpp` | 填侧 9 处拼写 `gPipeInputs` → `MGPipeClientInputs()`（:627/:629/:653/:703/:1217/:2205/:3016/:3027/:3348），两处 `MGPipeServerClearVerbBoundary()` → `MGPipeClientClearVerbBoundary()`（:2228/:3437）。`MGPipeVerifyReadHook` 的 `&self == &gPipeInputs` 判等（:663）**故意不动**：hook 钉在读侧块上，且 verify 与双块互斥 |
| `MobileGL/MG_Remote/Server/PipeApplier.cpp` | `Attach()` 调 `MGPipeServerBlockNoteIdentity()`（CONTRACT-P5E §3.2 的 "SetIdentity moves to ApplyOne"） |
| `MobileGL/MG_Test/Pipe/FieldOwnershipTest.cpp` | 6 个新用例（见 §3） |
| `MobileGL/MG_IntegrationTest/Scenarios/DualBlockScenario.cpp` + `Harness/DualBlockPeek.{h,cpp}` | 确定性 distinctness 对照用例（§6） |
| `MobileGL/MG_IntegrationTest/CMakeLists.txt` | `integration-dualblock-split` 车道注册（§5） |
| `MobileGL/MG_IntegrationTest/Harness/split_log_paths.py` | 新 `expect-fatal` 模式：Fatal 对的双向棘轮 + 每条红条目必须带具名 marker + 旋钮下一处 Admitted 即红 |
| `MobileGL/MG_IntegrationTest/Harness/dualblock-expected-fatals.txt` | 红清单棘轮初版（census 生成，非手写；§4） |
| `scripts/ci/dualblock_negative_control.sh` | 阴性对照脚本（`split_negative_controls.sh` 的合同：基线必须绿、红必须带被选条目自己的失败文本） |
| `.github/workflows/test.yml` | 两个新 CI 步：双块 census（预期红 + 棘轮核对 + 棘轮非空时全绿即红）与阴性对照 |
| `MobileGL/MG_Remote/CONTRACT-P5E.md` | §3.2 标 LANDED（落地形式写明）；ra2 修正段的 UNLANDED 注记同步关闭（§7） |

后端与 applier 的 379 处 `MGB_CTX` 读点、`PipeApply.cpp` 的写点零改动——`gPipeInputs` 拼写保留为 server 块。

## 2 设计决策与对侦察报告的偏差

1. **`MGB_CTX_LIVE` 不会恒 false——侦察报告这里错了**。`PipeInputs::IsLive()`（PipeFill.cpp:2100）
   不读 `m_live`，而是 forward 到 `LiveContext()`（读 `MG_State::pGLContext`）。inproc 下它仍答对——
   但这是一次 inproc 结构性看不见的跨角色读（P6 时 MG_Impl 不在 server 进程里）。
   **真正恒 nullptr 的是 `m_contextIdentity`**，而 DirectGLES 的 fb-slot memo 缓存（DirectGLES.cpp:191）
   先比 identity 再决定是否走带检查的访问器：nullptr 对零初始化缓存是**命中**，会发空槽指针——
   无名崩溃，恰是演练要消灭的形态。所以 f1 落了 `MGPipeServerBlockNoteIdentity()`。
   （附带：`m_live` 目前没有任何读者，见 §8。）
2. **SetIdentity 不是"Attach 一次"而是随 stamp 按 `MGPipeApplierContextSerial()` 刷新**。契约 §3.2 原文是
   "moves to ApplyOne, once per session"；identity 的 consumers 是 per-context 的 memo key
   （fb-slot 缓存、以及 `GetTextureContextId` 已经用同一串行号回答），context 切换（applier reset）
   时它必须动。实现：stamp 里比较即存，knob 关时整个函数早退——零行为变化。
3. **车道形态用 env 车道而非整批重注册**。侦察 §6.2 建议 `gtest_discover_tests` 以
   `DirectGLES.Split.DualBlock.` 前缀重注册同一批 scenario。实际采用 strict 车道的同构形态
   （`MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1 ctest -L 'integration-split|integration-magma-split'`），
   只新注册 1 条对照条目。理由：重注册会让 179 条条目集合成为第二份需要手工保持同步的清单
   （各块的 TEST_FILTER 排除项各不相同，逐块复制必然漂移），而 env 车道的红条目集合与
   `integration-split` **恒等由构造保证**。发现数不翻倍（G14 只增不删仍满足：净增 3 条注册名，
   见 §5）。Magma 由 `integration-magma-split` 并入同一 census 覆盖（2 条 NamedBlit）。
4. **对照条目不把旋钮钉进自己的 ENVIRONMENT**——这是对 N-6 钉死惯例的一次有理由偏离：
   ctest 的 ENVIRONMENT 属性覆盖进程环境，钉死 `ROLE_SPLIT_STATE=1` 会让 knob=0 形态（即阴性对照本身）
   经 ctest 不可达。失败方向安全：不带旋钮裸跑 `-L integration-dualblock-split` 是**红**（具名断言），
   不是静默绿。条目钉了 `MOBILEGL_IPC_RUN_AHEAD=0`：串行分离断言要求每个 verb 边界都填，
   run-ahead 下未设障 verb 不填，断言会变成时序相关。
5. **`VertexAttribDefaultsOf` 的镜像自查改读 client 块**（PipeFill.cpp:3016）。knob 开时它退化为
   对同 verb 自家填充的恒真自检——applier 镜像比对本身是 lockstep 时代的跨角色读，正是双块要消的
   形状；knob 关时两块同物，语义逐字节不变。
6. **`MGPipeClientClearVerbBoundary` 而非"删除 client 侧 clear"**（契约 §3.2 的字面是 DELETED）：
   调用点保留、重指到 client 块。knob 关时同一块同语义；knob 开时 client 块的 flag 从不举起，
   clear 是无操作——"client 不再能撤 server 的 stamp"由块分区结构性成立，不靠删代码。

## 3 单元测试（6 个新用例，FieldOwnershipTest）

- `RoleSplitOffFoldsTheFillSideOntoTheSharedBlock`：旋钮关 → `MGPipeClientInputs()` 就是 `gPipeInputs`。
- `RoleSplitUnderMonolithTransportIsStillOneBlock`：旋钮开 + monolith → 仍折成一块（monolith 折叠）。
- `RoleSplitGivesTheFillSideADistinctBlockTheStampNeverTouches`：两块是不同对象；server stamp 只动
  server 块（serial +1、flag 举起），client 块纹丝不动；`MGPipeClientClearVerbBoundary` 不撤 server 的
  stamp；stamp 后 server 块 identity 非空。
- `TheClientVerbLeaveWritesTheClientBlockAlone`：`MGPipeLeaveVerb` 只推 client 块 serial。
- `DualBlockMakesABarrierPulledReadANamedAbortWithoutStrict`（fork）：旋钮开、**strict 关**，
  server stamp 后读 `GetBoundVertexArray` → SIGABRT 且日志带
  `Fatal{UnmigratedPipeInput, "GetBoundVertexArray@DrawArrays"} [BARRIER-PULLED, MOBILEGL_IPC_ROLE_SPLIT_STATE=1, ...]`。
  这是机关的 R-16：O 类 SharedPtr 字段没有 null 检查，没有这一臂它就是无名段错误。
- `DualBlockDoesNotArmUnderMonolithTransport`（fork）：旋钮开 + monolith → 同一读被计数而非 Fatal。

## 4 红清单（census 实测，`~/w7/p5f-f1`，两次独立运行同一对集）

车道：`MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1 ctest -L 'integration-split|integration-magma-split'`。
181 条目：**155 红，红条目 155/155 带具名 Fatal，零无名崩溃**；22 绿（3 条 PushMonolithArm 单块臂 +
17 条路径上本就不读 BARRIER_PULLED 字段的 scenario + Ct 角色守卫死亡用例 + SplitLogPaths 元检查），
4 skip（两条 PersistentMapArm 专属、两条 StrictArming 专属，在自己的车道外 by design 跳过）。

**6 对首阻塞 `<字段>@<verb>`**（每条目死于自己的第一处 pull，退掉一对就推进到下一对——清单会随
fm/fs/fr/fv 缩短）：

| 对 | 条目数 | FieldOwnership.def 退役归属 | f1 包归属建议 |
|---|---|---|---|
| `GetFramebufferBindingSlot@ReadPixels` | 132 | P5e (Espryt unbarriered), P7 (Magma) | Espryt readback 路径的载体（最大一块，决定 fv/收尾工作量） |
| `GetTransformFeedbackProgram@DrawArrays` | 11 | P3b/P4b (Espryt), P7 (Magma) | XFB 程序随 draw 的载体 |
| `GetTextureObject@CopyImageSubData` | 6 | P7 | sticky forward 的 handle 化 |
| `GetFramebufferBindingSlot@Clear` | 2 | P5e (Espryt), P7 (Magma) | **fm 的前两行**（两条 DirectVulkan NamedBlit） |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | P5e (Espryt unbarriered), P7 (Magma) | 纹理单元指针的载体 |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | P9 | sticky forward |

注意与 strict 车道的分工：strict 管"设障 pull 有没有被承认"（值还在共享块里），双块管"值根本不在
对面"。同一笔债的两个探针，两份棘轮独立。旋钮下 `Admitted{` 实测为零（机关先于承认臂打响）。

## 5 车道与标签（ID-131 的证据）

`ctest -N` 实测（build-split @ cfcc12f7）：

- `-L integration-split` = **179**（不含新条目——`integration-dualblock-split` 里隔着 `dualblock`，
  子串匹配不上）；
- `-L integration-dualblock-split` = **1**（`DirectGLES.Split.DualBlock.DualBlockScenario.TheTwoRolesHaveDistinctBlocks`）；
- `-L integration-magma-split` = 2（未变）；
- `-L integration-gpu` = **1359**（基线 1357 + 2 条 ambient 注册 `DirectGLES./DirectVulkan.DualBlockScenario.*`——
  与 F1WireScenario 的 19 条 ambient 条目同形态，运行时 monolith 臂 GTEST_SKIP）。
  G14：名集合只增不删（新增 6 个单测名 + 3 条 itest 注册名）。

`SplitLogPaths.PrivateAndDistinct` 通过：新条目恰好一条私有日志且不与他人重名。

## 6 阴性对照（P5F §6，两种形态都真跑过）

`scripts/ci/dualblock_negative_control.sh`（p5f_gate.sh 的 `negctl` 步）实测：

- **旋钮=1**：`DirectGLES.Split.DualBlock.DualBlockScenario.TheTwoRolesHaveDistinctBlocks` **Passed**
  （0.68s；三块断言全过：roleSplitActive、两块地址不同、client verb 边界推 client 块 serial 而
  server 块 serial 不动）。
- **旋钮=0**：同一条目 **Failed**，失败文本正是该用例自己的断言：
  `dual-block control: MGPipeRoleSplitActive() is false - MOBILEGL_IPC_ROLE_SPLIT_STATE is not armed ...`。
- 脚本同时坚持：基线绿（0 failed、0 skip——skip 不算绿，ID-62）、对照红且红带案文。

## 7 CONTRACT-P5E §3.2（ID-135）的核实结论

侦察的判断成立：双块落地后 §3.2 的语义**在旋钮开的臂上自动成立**——server 块就是 applier 私有存储，
stamp 写的 `m_filled/m_currentVerb/m_serverStampedVerb` 都在 server 块，client 的两处 clear 已重指到
自己块（无人读过它的 flag），`SetIdentity` 落到了 applier 侧（Attach + stamp 刷新）。ra2 的告诫
（"不配版本化就更坏"）由机关兑现：BARRIER_PULLED 值在 server 块里**不存在**，读它具名死，不存在
"静默旧值"。契约文本已从 UNLANDED 改为 LANDED 并写明落地形式。旋钮关的臂保持共享块旧语义，
是 A/B 与阴性对照所需，不是第二份契约。

## 8 附带发现（与本包无关，未修）

1. **`strict-expected-markers.txt` 有两行 stale**（`GetBoundVertexArray@DrawArrays`、
   `GetBufferBindingSlot@DrawArrays`）：strict 车道实测观测 8 对（7 admitted + 1 escalated），
   双向棘轮因这两行不再出现而红。**归因：基线树 `~/w7/p5f-base` @ e75e00cb 逐字复现同一失败**
   （同一观测集、同一 vanished 对），与 f1 无关——前者随 client-arrays 条目迁出
   `integration-split`（ID-134 的标签搬家）而失效，后者是 MultiDrawTier 车道不再产生它。
   修复是删掉这两行（属 strict 车道维护，留给下一轮车道养护，本包不顺手改别人的棘轮）。
   f1 的 strict 门按任务口径判读：179/179 绿、`Fatal{` 为零、观测 Admitted 恰为既有 8 对——满足。
2. **`check_doc_citations.py --strict` 在基线即红**：CONTRACT-P5.md:349（`Init.cpp:44`）与 :524
   （`Core.cpp:39`）两处 basename 歧义，base 树同 rc=1；CI 本就以 `|| true` 非 strict 跑它。
   f1 新增文档没有引入新歧义（见 §9 的对照数字）。
3. **`m_live` 无读者**：`PipeInputs::IsLive()` forward 到 `LiveContext()`，从不读 `m_live`；
   `SetIdentity` 写它纯属记录。inproc 下 `MGB_CTX_LIVE` 经此跨角色读到 client 的 `pGLContext`——
   答得对但正是 P6 要消灭的形状。建议 fm/fs 或收尾包把 server 块的 liveness 改读自己的 `m_live`
   （f1 已会在 stamp/Attach 时置它），并决定 context destroy 时谁把它打回 false
   （侦察 §8.2 的开放问题，候选：applier_reset 控制记录）。
4. **census 车道与 strict/gpu 车道共享私有日志文件，绝不能并发跑**。本报告撰写期间我曾在后台并发
   发起 `dualblock` 与 `gpu` 两步，gpu 车道（knob 关）覆写了 census 正在读的 18 份日志，表现为
   "红而无 marker + 对子计数漂移"。单独重跑后两套数字稳定一致。p5f_gate.sh 的步序是串行的；
   教训写在这里防后来者重蹈。

## 9 逐门数字汇总

| 门 | 结果 |
|---|---|
| build（split flavour） | rc=0，984 TU |
| unit（旋钮关） | **2262/2262**（基线 2256 + 新增 6） |
| integration-split（旋钮关） | **179/179**，零回归 |
| integration-split+magma（旋钮开） | 181 条目，155 红**全部具名**、零无名崩溃；6 对首阻塞落盘为 `dualblock-expected-fatals.txt` 初版 |
| flavours | pull rc=0（`MOBILEGL_PIPE_PUSH=1` absent ✓）、push rc=0（=1 ✓），compile_commands.json 断言过 |
| **G1** | pull flavour 对基线 e75e00cb：`.text` 10806051 → 10806051（+0），defined symbols 27815 → 27815，**0 added / 0 removed / 0 resized / 0 renamed**——零认定 |
| integration-gpu（旋钮关） | **1359/1359**（1357 + 2 条 ambient DualBlock skip 条目） |
| 生成器 --check/--self-test、include 闭包、dirty-surface | 全绿（gen_pipe 12 控制、field-ownership 13 控制、dirty-surface 27 控制均打响） |
| doc 引用 --strict | 基线既有 2 处 ambiguous（CONTRACT-P5.md:349/:524），f1 新增 0 处 |
| strict（旋钮关） | 179/179 绿、`Fatal{`=0、观测 8 对 admitted（=任务口径的既有八行）；棘轮文件的 2 行 stale 见 §8.1 |
| 阴性对照 | 旋钮=1 绿 / 旋钮=0 红且带案文，两种形态各跑过（§6） |

## 10 复现

```bash
# WSL 侧（树 ~/w7/p5f-f1，branch p5f/f1）
bash ~/w7/notes/tools/p5f_gate.sh f1 build      # configure + build-split
bash ~/w7/notes/tools/p5f_gate.sh f1 unit
bash ~/w7/notes/tools/p5f_gate.sh f1 isplit
bash ~/w7/notes/tools/p5f_gate.sh f1 strict
bash ~/w7/notes/tools/p5f_gate.sh f1 dualblock  # census + 棘轮核对
bash ~/w7/notes/tools/p5f_gate.sh f1 negctl     # 阴性对照两种形态
bash ~/w7/notes/tools/p5f_gate.sh f1 gpu
bash ~/w7/notes/tools/p5f_gate.sh f1 flavours
bash ~/w7/notes/tools/p5f_gate.sh f1 gens
# G1：python3 scripts/symbol_report.py --before ~/w7/p5f-base/build-pull/libMobileGL.so \
#      --after build-pull/libMobileGL.so --threshold 0
```
