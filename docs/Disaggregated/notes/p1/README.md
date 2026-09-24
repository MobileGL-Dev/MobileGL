# P1 — `PipeInputs` 替换与 verify harness

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：`MG_Backend/MGPipe/PipeInputs.h`（63 字段）；277 处 `pGLContext->` 箭头 + 58 行非箭头逐条换成 `MGB_CTX`；逐 verb 类填充点（83 条 `MGP_FILL`）与逐 verb 世代 poison；G4 影子比对器 + verify CI 模式；push-on-mutation 三字段。
- 门：pull `nm` 不变；单元 1485×3；`integration-gpu` 878；`integration-verify` 818 零 `Fatal{`；79 例 retrace verify 79/79 armed 零分歧。verify 通道找出 9 处缺填充行与 3 个"verb 内后端改前端"字段。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P1** `PipeInputs` 替换与 verify harness
- **状态**：✅
- **落地什么 / 范围**：`MG_Backend/MGPipe/PipeInputs.h`（63 字段）；277 处箭头 + 58 行非箭头逐条转换；逐 verb 类填充点；逐 verb 世代 poison；G4 影子比对器 + verify CI 模式；push-on-mutation 三字段
- **验收门 / 证据**：pull `nm` 不变；单元 1485×3；`integration-gpu` 878；`integration-verify` 818 零 Fatal；79 retrace verify 79/79 armed 零分歧。§2

## 实测：P1（lavapipe / llvmpipe）（原 `MEASUREMENTS.md` §2）

### 2.1 规模

| 量 | 值 |
|---|---|
| 后端 `pGLContext->` 箭头站点 | 277（Espryt 113、Magma 164） |
| 非箭头行 | 58（Espryt 9、Magma 49，其中 43 条是 Magma 的逐 verb `MOBILEGL_ASSERT`） |
| `PipeInputs` 字段 / 不同访问器 | 63 / 62（Espryt 32、Magma 56） |
| 填充点 | 83 条 `MGP_FILL`，覆盖 69 个 verb、9 个类 |
| `SyncPersistentMappedRange` / `SyncGpuWrites` | **21**（Espryt 9 + Magma 12；P5 b1 复核，原记 20 漏了共享 helper `ResolveIndirectCommandBytes`）/ 6 |

### 2.2 verify 通道发现的两类真问题

**缺填充行（9 处）**：`kReadback` 缺 `IsTransformFeedback{Active,Paused}`、`kTextureOp` 与 `kDispatch` 缺 `IsCapabilityEnabled`、`kBlitOrCopy`/`kTextureOp` 缺着色器 blit 用到的 viewport 与顶点 / 缓冲绑定。其中 8 行是静态过近似（代码路径可达但通道未跑到），有意保留——能退役它们的证据只能是动态的（完整 CTS caselist + `MOBILEGL_PIPE_POISON_OMIT`，未做）。

**verb 内后端改前端（3 个字段）**：Magma 在 draw 里写前端对象（合成回退纹理、材质化排队清除、覆写 sampler filter），8 条 DirectVulkan 用例与 2 条 trace 报 `Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@Draw*"}`。解法 **push-on-mutation**：前端计数器移动时用 `MGP_NOTE_MUTATION(Field)` 刷新推送块里那一个字段——钩子挂在三个计数器上（`BumpSamplingResolutionGeneration`、`BumpTextureBindGeneration` / `NoteUnitTouched`、高水位分支），不挂在四十个写入点上。

### 2.3 验收

| 门 | 结果 |
|---|---|
| pull 构建符号与 `.text` | 0 增 / 0 删 / 0 改尺寸 / 0 重命名，`.text` 不变 |
| `MG_Backend` 里的 `pGLContext` | 0 |
| 单元（pull / push / verify） | 1485 × 3 |
| `integration-gpu`（pull） / `integration-verify` | 878 / 818 零 `Fatal{` |
| 79 例 retrace（`MOBILEGL_PIPE_VERIFY=1`） | 79/79，79/79 armed，零 `Fatal{` |
| 两个阴性对照 | 篡改字段变红、抽掉一个填充戳记在那条 verb 上变红 |
| 测试名 | 0 删除，+29 |

verify 构建的代价：`integration-verify` 与 `integration-gpu` 同量级；79 例 retrace 在 4 路下约 20 分钟。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P1.md`](BRIEF-P1.md) | P1 implementation brief — PipeInputs strangler, per-verb fill points, poison generations, MOBILEGL_PIPE_VERIFY comparator and its CI mode |
| [`P1-LANE-FINDINGS.md`](P1-LANE-FINDINGS.md) | P1 verify-lane findings on the integrated tree (`feat/disaggregated` @ 97b997d5, WSL `~/w7/pipe`) |
| [`p1-results/`](p1-results/) | 19 个文件：各包（core / espryt / magma / verify / fix-*）的实现与评审稿 |

捞回的独有材料：[`../recovered/wf4/`](../recovered/wf4/)（pull-site 目录分部、BRIEF-P1 第一部分、三份 scout）。
