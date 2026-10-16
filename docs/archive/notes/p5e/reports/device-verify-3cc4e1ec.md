# P5e 设备验证（头 `3cc4e1ec`，Redmi `2f7cbe2e`）

**结论：PASS。两条传输臂都能正常跑游戏。** 上一轮设备验证发现的 monolith 启动即崩已修复并回归。

## 1. 被测物

- MobileGL `feat/disaggregated @ 3cc4e1ec`（= wave2 `44f91c74` + fix1 `66621767` + ID-110）
- FCL fordebug，`./gradlew :FCL:assembleFordebug -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON -Pmobilegl.pipePush=ON`，BUILD SUCCESSFUL
- Minecraft 26.3-rc-3，世界 `test`，VD12，DirectGLES
- **版本身份以符号探针确认**，不看游戏内 `GIT@` 戳记（增量构建下它是旧的）：APK 的
  `lib/arm64-v8a/libMobileGL.so` 内含 `texture-handle-arm`、
  `was handed a NULL frontend texture for handle`、`the storage sync needs the frontend`、
  `refusing to attach a renderbuffer with no storage` 四个 fix1 新增字符串。

## 2. 三臂结果（进世界后 60 s 采样）

| 臂 | 进世界 | fps | draws/帧 | 进程 | Fatal / crash buffer | 截图 |
|---|---|---|---|---|---|---|
| monolith | 31 s | **263.5** | 845 | 存活 | 无 | 世界正常（夜间地形、树、热键栏） |
| inproc | 31 s | **136.1** | 853 | 存活 | 无 | 同一视角同一世界 |
| inproc 复跑 | 32 s | **140.3** | 852 | 存活 | 无 | 同上 |

对照：修复前（`44f91c74`）同一臂 monolith 是 **65 s 后进程消失**，SIGSEGV 于
`Lightmap.<init>` → `clearColorTexture` → `glClear` → … → `SyncMipmapsToBackend(空 SharedPtr)`。

两点说明：

- monolith 这一次**未被 120 Hz vsync 封顶**（与 P5d 记录的 205.8 那次同类），所以 263.5 与
  inproc 的 136-140 **不可直接比大小**。本轮问的是"改完还能不能正常跑"，不是配对性能。
- inproc 的 136-140 与修复前记录的 140.0 / 144.9 同档。fix1 改的那行（臂选择本身）两臂都读，
  split 臂没有被拖慢。

## 3. 主机门（WSL `~/w7/p5e-int`，split flavour）

| 门 | `44f91c74` | `3cc4e1ec` |
|---|---|---|
| build | rc=0 | rc=0 |
| unit | 2250/2250 | **2250/2250** |
| `integration-split` | 113/113 | **116/116** |
| `integration-gpu`（两条运行时臂） | **1157/1294**（137 SEGFAULT / 38 scenario） | **1294/1294** |
| `integration-split-strict` | 7/116 | 7/116（预期红，逐数相同） |
| 生成器 / include 闭包 / dirty-surface / doc 引用 | 绿 | 绿（只余 `CONTRACT-P5.md:524` 既有歧义行） |

## 4. 两条裁定

- **ID-109**：fix1 给 monolith lane 挂 `integration-split` 标签的读法**接受**（该标签指"门禁要跑
  的集合"，不是"跑 inproc"）。并把 `integration-gpu` 升为**集成分支的常设门禁步骤**
  （`p5e_gate.sh gpu`）。漏的不是某个用例，是整条运行时臂。
- **ID-110**：fix1 报告 §3.5 留的那处"靠链条安全"的座（`texBufferByRecord`）改为自述式传输
  测试。新增 `MGB_TEXTURE_RECORD_ARM_SELECTED()` 把"记录臂被选中"只说一次。有传输时该合取项
  已被 `pushedStorage != nullptr` 蕴含，行为不变。

## 5. 阶段状态

P5e **未收官**：`kMGPipeP5eRunAheadReady` 与 `kMGPipeP5eClientWaitRuleLanded` 仍为 `false`。
翻转它们的门是 BRIEF-P5E §3（strict 车道转硬绿，只余 §7 白名单 —— 当前 7/116，是本阶段剩下的
主要工作）与 §4（设备出口：fps ≥ lockstep、client 线程 CPU ms/帧 < lockstep、SSIM 不变、
逐 op 等待清单无 `kWaitApplied` 行与逐 draw reply 行）。

已推送 `origin/feat/disaggregated @ 7c6f6886`（含本轮文档写回）。
