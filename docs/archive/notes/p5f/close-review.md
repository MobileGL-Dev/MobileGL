# P5f 阶段末跨模型族审查

实现方为 GPT 系列；审查通过本机 Claude Code 只读 CLI 执行，限定 Read/Glob/Grep、无 MCP，
没有修改文件或运行 shell。审查头 `de78f47d`，基线 `e75e00cb`。CLI 成功返回；模型记录含
`claude-opus-5[1m]` 与 `claude-haiku-4-5`，与实现方错开模型族。原始结果保存在
`C:/Users/geekerwan/.codex/tmp/p5f-final-review-de78f47d/result.json`。

## 原始审查结果

## 审查发现

### P1 — 最终 dual-block“硬绿”门可在测试未完整执行时伪绿

- **位置**
  - `.github/workflows/test.yml:1387-1400`
  - `MobileGL/MG_IntegrationTest/Harness/split_log_paths.py:138-160`
  - `MobileGL/MG_IntegrationTest/CMakeLists.txt:2580-2601`
  - `MobileGL/MG_IntegrationTest/Scenarios/F1WireScenario.cpp:99-103`

- **触发条件**
  1. 当前 `dualblock-expected-fatals.txt` 为空；
  2. `ctest` 因空选择、提前中断或基础设施错误返回非零；或者新加的某个 `DirectGLES/DirectVulkan.Split.P5fRsp.*` 用例因环境标记或 split runtime 不满足而 skip/未出现在 JUnit 中。

- **失败路径**
  - workflow 用 `|| rc=$?` 吞掉 `ctest` 的非零状态；
  - 随后仅在“`rc == 0` 且预期表非空”时失败。当前预期表为空，因此任何非零 `rc` 都不会直接使步骤失败；
  - `expect_fatal()` 只遍历 JUnit 中实际出现的 testcase，不把 `dualblock-lane.json` 的发现集合与 JUnit 集合比较；skip 被直接计数后忽略，缺失用例也完全不可见；
  - 最终空观测集合与空预期集合相等，helper 返回成功。
  - 因而阶段出口要求的双后端逐帧 `rsp=0` 用例可以没有实际执行，步骤仍打印硬绿。

- **最小修复建议**
  1. 当前阶段预期表为空时，无条件要求 `rc == 0`；
  2. 在 `expect_fatal()` 中比较 JSON 发现集合与 JUnit 集合，拒绝 missing、notrun、disabled；硬绿模式还应拒绝所有 skip；
  3. 对 `integration-p5f-rsp` 增加独立核验：必须恰好执行 DirectGLES、DirectVulkan 两条，`2 PASS / 0 skip / 0 failed`。

### P2 — 三个反向回调没有投递给实际取得所有权的 `ServerSession`

- **位置**
  - `MobileGL/MG_Remote/Server/ServerSession.cpp:264-315`
  - 对照正确的错误回调：`MobileGL/MG_Remote/Server/ServerSession.cpp:332-340`
  - active owner 发布点：`MobileGL/MG_Remote/Server/ServerSession.cpp:567-573`

- **触发条件**
  - 首个成功 `Accept()` 的对象不是 `ServerSessionInstance()`，而是另一个合法的 `ServerSession` 实例；
  - 之后后端调用 `OnBufferWriteback`、`OnGpuWritten` 或 `OnSurfaceChanged`。

- **失败路径**
  - `Accept()` 将实际 owner 发布到 `g_active`，并把全局回调表安装为该 session 的生产者；
  - 但三个生产函数仍硬编码调用 `ServerSessionInstance()`，而不是 `ServerSession::Active()`；
  - 事件因此写向未接受连接的 singleton，而非拥有 SEG_EVENT 的 session，通常在无效 event producer 上落入 `Fatal{EventRingOverflow}`；即使 singleton 状态发生变化，也会投递到错误 session。
  - `ServerOnGlError()` 已正确通过 `Active()` 解析 owner，四个回调的所有权语义目前不一致。`g_sessionOwner` 对任意 `ServerSession*` 做 CAS，也说明所有权并未限定为 singleton。

- **最小修复建议**
  - 让三个生产函数与 `ServerOnGlError()` 一样解析 `ServerSession::Active()`；没有 active owner 时给出具名 `RoleViolation`；
  - 增加一个“非 singleton session 首先 Accept，四种回调均进入其 event ring”的测试。

## 审查局限

本次严格保持只读，仅使用 `Read/Glob/Grep`；未运行构建、测试或设备门。因此未评价当前 pending validation 的通过与否，也未将其视为代码缺陷。除此之外，在指定的跨角色访问、上下文/XFB/raw sampler 生命周期、控制帧保真及 Magma record/resource 路径中，未发现其他具有充分代码依据的 P0/P1/P2 问题。

## 处置（均已完成）

1. **P1，结果集不完整可伪绿**：`c42577a4` 给空表要求 ctest rc=0；对 JSON 发现全集与
   JUnit 做非空、完整、一一对应核验；拒绝 missing/extra/duplicate/notrun/disabled。
   对已执行的 green/skip 也扫描 marker，而非仅扫描 failed 条目。GLES 与 Magma 私有日志
   所有权同样校验。新增独立 `integration-p5f-rsp` 硬门，必须恰两后端 2 PASS / 0 skip。
   仅保留既有六个 skip，严格匹配名称和原因；不采用“所有 skip 都拒绝”，因为四个是专属
   计数车道的普通/SmallRing sibling，另两个是已有 Magma clip-feature 限制。其他 skip 均红。
   9 个结果核对回归测试全绿；旧 helper 对“缺一个 JUnit 条目”没有拒绝，控制真实失败为
   `ValueError not raised`，新 helper 通过。日志 `exit-review-gate-{red-once,green}.log`。
2. **P2，非 singleton callback owner**：`53699b5a` 让四种 producer 统一取 `Active()`，
   无 active owner 时均具名 RoleViolation。新增非 singleton 首先 Accept 后四事件落入其
   ring 的生产测试，以及 Close 后缓存的四 callback 不可再投递测试。旧版真实 abort 于
   `kEventGpuWritten` 的 `EventRingOverflow`，修复后 11 项反向通道测试全绿；原 readback
   fixture 与握手也通过。日志 `fv-review-*.log`。

两项均有实际红转绿证据，未把审查建议当作验证结果。修复后的最终 Android 库也重新部署，
六个 clean-boot 臂全部重新运行。没有留待 P6 的本次审查 P0/P1/P2 项。
