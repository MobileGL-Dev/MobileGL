# P5f — 一切状态上 wire：跨进程前的最后一次归零

> 基线 `feat/disaggregated @ 7f1d2134`。路径在 `MobileGL/` 下。**未开工。**
> **P6 在 P5f 之前不开工**：P6 的前提"只是传输替换"今天不成立，§2 是不成立的清单。

---

## 0 目标与出口判据

**目标**：两个角色之间，除 `SEG_CMD` / `SEG_STAGE` / `SEG_REPLY` / `SEG_EVENT` / caps 快照 / 控制帧
之外零交换。不得有任何一方读写另一方的内存、也不得依赖"两者其实是同一个对象"。

P5c 立过同样的规则（规则 E），但它的判据是**审计**——人读 59 行清单。P5f 的判据是**机器**：

> **出口判据**：`inproc` 下给两个角色**各自一份 `PipeInputs`**（§1），以及各自一份所有
> per-context 的进程级静态，整条 `integration-split` 仍然全绿，且
> `MOBILEGL_IPC_STRICT_ERRORS=1` 下 `Harness/strict-expected-markers.txt` **为空**。

这条判据的价值在于它**不需要第二个进程**就能证伪跨进程的假设。今天两个角色共享一份
`gPipeInputs`，所以"server 读到了 client 填的值"和"server 读到了记录送来的值"在运行时完全
无法区分——这正是 P6 第一天会踩的坑。把块拆成两份，前者立刻变成 poison Fatal。

---

## 1 `gPipeInputs` 是什么，拆进程后它会怎样

### 1.1 它是什么

`MG_Backend/MGPipe/PipeInputs.h:835`：

```cpp
inline PipeInputs& gPipeInputs = *new PipeInputs();
```

一个**普通的进程级全局对象**，不在任何共享内存段里。它是“后端被推送的状态块”——
backend 不再每 draw 去读 `MG_State::pGLContext`，改为读这个块。每个字段带一个**归属类**
（CONTRACT-P5 表 2 / R-7，手维护的一半在 `MG_Pipe/FieldOwnership.def`）：

| 归属类 | 量 | 含义 |
|---|---|---|
| `RECORD_SUPPLIED` | **41 个字段**（P5c rv 之前 32）；**由推导得出，不列在表里** | 推送的记录供给**整个**字段，server 永远不需要 client |
| `APPLIER_DERIVED` | 4 行 | applier 从它已经应用的记录里写出它 |
| `BARRIER_PULLED` | **21 行 / 15 个字段** | **P5 的债**：server 读的是 client 的残余填充留在“那唯一一份共享的 `gPipeInputs`”里的值 |
| `FATAL` | 4 行 | 没有载体，缩减路径也不读；读到即 abort |

写它的是 client（`MG_Impl/Pipe/PipeFill.cpp`），读它的是 backend。每个字段带一个**逐 verb 的
世代戳**，读到比当前 verb 序号旧的戳就是
`Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}`（`MOBILEGL_PIPE_POISON`）。

### 1.2 为什么今天能工作

因为两个角色在**同一个进程、同一个 `gPipeInputs` 对象**上，而 verb barrier 保证同一时刻
{GL 线程, apply 线程} 至多一个可运行——所以这个块任何瞬间只有一个写者
（`PipeInputs.h:904-908` 的裁定）。client 填，server 读，中间不经过任何载体。

`BARRIER_PULLED` 这一类的定义就是："server 在 barrier 把两个线程隔开时，读 client 的残余填充
留在 `gPipeInputs` 里的值。**合法，并且被计数**（`PipeStats::CallClass::ResidualPulls`）。"

### 1.3 拆进程后它会怎样

**client 进程和 server 进程各有一个 `gPipeInputs`，是两个不同的对象。**

- `RECORD_SUPPLIED` / `APPLIER_DERIVED` 的 **45 个字段**没问题：它们的值本来就由记录或 server
  自身产生，server 侧的块会被正常填上。
- `BARRIER_PULLED` 的 15 个字段**没有填充者**。client 进程里那次 fill 写的是 client 自己的块；
  server 进程的块里那些字段的世代戳永远是旧的 → 每一次读都是
  `Fatal{UnmigratedPipeInput}`。
- `MG_Impl` 根本不在 server 进程里，所以连"让 server 自己去 fill"这条退路都没有。

头文件自己写着这件事（`PipeInputs.h:279`）：当前 verb 边界的清理"对 inproc 足够——两个角色共享
一个进程、一个 `gPipeInputs`——**而对 P6 是误导的，那里 `MG_Impl` 根本不在 server 里**"。
以及 `:902`：`rsp` 的值"**就是 P6/P7/P8 债务的大小**"。

**所以 P5f 对 `gPipeInputs` 的要求**：`BARRIER_PULLED` 归零。每个字段要么升成
`RECORD_SUPPLIED`（值随记录过线），要么升成 `APPLIER_DERIVED`（server 自己推），要么降成
`FATAL`（缩减路径不读它）。**块本身不需要跨进程共享，它需要的是每个字段都有一个不依赖对方
地址空间的来源。**

---

## 2 清单（在基线头上实测）

### 2.1 `BARRIER_PULLED` 的 15 个字段

| 字段 | 退役阶段（`FieldOwnership.def` 原文） |
|---|---|
| `GetBoundVertexArray` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetBufferBindingPoint` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetFramebufferBindingSlot` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetImageTextureBinding` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetProgramForDraw` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetProgramForDispatch` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetTextureUnitObject` | P5e (Espryt unbarriered), **P7 (Magma)** |
| `GetTextureObject` | P7 |
| `GetBufferBindingPointCount` | P7/P13 |
| `HasOpenTransformFeedbackSpan` | P7/P9 |
| `GetTransformFeedbackProgram` | P3b/P4b (Espryt), **P7 (Magma)** |
| `GetProgramObject` | P9 |
| `ValidateProgramName` | P9 |
| `RecordError` | P9 |
| `GetBufferBindingSlot` | P8 (indirect), P9 (readback), P13 (transfer) |

**八个字段 P5e 只为 Espryt 的未设障路径退役了，Magma 一条都没动。** 这是清单里最大的一块。

strict 车道当前实测活着 **10 对** `<field>@<verb>`（`Harness/strict-expected-markers.txt`），
其中两对是 escalation-barriered 的 client vertex array 条目（P8）。

### 2.2 Magma（DirectVulkan）

P7 原本是"全量迁移 10 个子系统"，里面混着与拆分无关的工作（内容寻址 CSO、内部 shader 烘焙、
定宽重写）。**P5f 只取其中会阻碍跨进程的部分**：§2.1 里带 "P7 (Magma)" 的那 9 个字段，以及
Magma 侧一切直接读前端对象 / 写 client 存储的站点。P5c 审计的 **T5** 是已知的一条：
Magma 生成 mip 直接写 client 的 level 存储（`DirectVulkan/Renderer/VulkanRenderer.cpp:1562-1590`）。

Magma 今天不发布 `kCapRunAheadApply`，所以它跑 lockstep——**lockstep 掩盖的正是这类缺陷**，
与 P5e 的 ID-132 是同一条教训。

### 2.3 EGL / surface 控制面（P5c 审计 G4）

十二个 `Server*` forwarder（`MG_Remote/Server/ServerLoop.cpp:802-1022`）经单槽邮箱传
**函数指针 + 栈上 `void*`**（`ServerLoop.cpp:533-568`、`:645`）。`protocol.fbs:166-205` 的
`SurfaceOp` / `SurfaceReply` / `WindowKind` schema 已定但**无人使用**。
跨进程后函数指针与栈地址都无意义。

### 2.4 进程级静态，语义却是 per-context

已知两个：`g_syncedRenderStateParameters`（`MG_Backend/DirectGLES/DirectGLES.cpp:4586`）与
`ScopedDefaultUnpackState::s_synced`（`CONTRACT-P5C.md:538-539` 记为 P6 的）。
**完整清单未知**——这类缺陷 `inproc` 结构性看不见，必须普查。

### 2.5 P5c 留下的具名豁免 scope

`MGPipeFrontendKeyedRegistryScope` 在 `DirectGLES.cpp` 还有约 **12 个活跃站点**
（`:2542`、`:4505`、`:5540`、`:5659`、`:5940`、`:6490`、`:6576`、`:7092`、`:7682`、`:8213`、`:11231` …），
背 P3b/P4b 的债：registry 仍按前端地址 / lifetime-id 键控。

### 2.6 反向通道（P9 的一部分）

XFB scatter 对 client shadow 读改写、`OnXfbScatterReady`、`OnTexturePullRequest` 与终止符、
`OnGlError` 有序化。其中**凡是直接触碰 client 内存的**归 P5f；纯粹的异步化 / 批处理 / 排序
（`SEG_REPLY` 异步槽池、PBO fire-and-forget、epoch 排序）仍归 P9。

### 2.7 class C

`MG_Remote/Client/EmitTables.cpp` 里 10 处具名拒绝。**不归 P5f**：拒绝是诚实的，
`Fatal{UnmigratedVerb}` 在两个进程下行为一致。它们限制的是 spawn 车道的覆盖面，不是它的正确性。

---

## 3 范围边界

| 取 | 不取 |
|---|---|
| §2.1 全部 15 个字段（含 Magma 的 9 个） | P7 的性能与架构项：内容寻址 CSO、内部 shader 烘焙、`DynamicBackendParameters` 定宽重写 |
| §2.2 Magma 侧一切跨角色读写 | Magma 的功能补齐（占位纹理原生化、具名 UBO host payload 等），除非它们本身是跨角色读写 |
| §2.3 控制面帧（原 P6 的） | 真窗口到达（P12） |
| §2.4 静态量按世代/角色分区 | — |
| §2.5 registry 按句柄重键 | P3b/P4b 的其余项（view 索引重映射、workaround 删除） |
| §2.6 反向通道里触碰 client 内存的部分 | P9 的异步化与排序 |
| — | class C（§2.7）、chunked readback（P9）、persistent map 分档（P11） |

**判断准则只有一条**：*它在另一个地址空间里还成立吗？* 不成立 → P5f。成立但慢/丑 → 留给原阶段。

---

## 4 中心机制：双块演练

P5f 的每一条都靠同一个机制证伪，而不是靠审计：

1. **`PipeInputs` 按角色分区**：`gPipeInputs` 变成"取当前角色的块"。client 的 fill 写 client 的块，
   backend 的读取 server 的块。任何今天依赖"两者是同一个对象"的路径，立刻变成
   `Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}`，带字段名和 verb 名。
2. **per-context 静态按角色 + 世代分区**，同理。
3. 两者都由一个旋钮武装（建议 `MOBILEGL_IPC_ROLE_SPLIT_STATE=1`），默认关，CI 开一条车道。

这等于在**不拆进程**的前提下把进程拆分的语义先跑一遍。代价是一个旋钮和一次分区；
回报是 P6 那天不会有任何一条"第一次执行"的路径。

---

## 5 包（草案，待 §2.4 普查后定）

```
f0  普查（只读）：§2.4 的完整静态清单 + §2.6 的逐站点清单 + 每个 BARRIER_PULLED 字段的载体判定
 └─ f1  双块机制 + 旋钮 + 车道（§4），此时预期大面积红，红就是清单
     ├─ fm  Magma 的 9 个字段 + T5 + Magma 侧跨角色读写
     ├─ fc  控制面帧（§2.3）
     ├─ fs  静态量分区（§2.4）
     ├─ fr  registry 按句柄重键（§2.5）
     └─ fv  反向通道触碰 client 内存的部分（§2.6）
 └─ 收口：strict 允许表清空，双块车道转硬绿
```

`f1` 先落是故意的：**让红先出现，再逐包消红**，而不是各包自称做完之后再找一个判据。

---

## 6 出口门

1. 双块车道（§4）`integration-split` 全绿。
2. `MOBILEGL_IPC_STRICT_ERRORS=1` 下 `strict-expected-markers.txt` **为空**，且两侧棘轮仍在
   （空表 + 出现任何一对即红）。
3. `FieldOwnership.def` 零 `BARRIER_PULLED` 行；`rsp` 计数器逐帧读零。
4. **Magma 与 Espryt 跑同一套判据**，且 Magma 发布 `kCapRunAheadApply`——或者明确说明它为什么
   不发布，并给出不发布也不掩盖缺陷的理由（ID-132）。
5. G1 0/0/0/0；G2 名集合不变；G14 测试名只增不删。
6. 逐包 red-once。
7. 设备：Redmi `2f7cbe2e`，双后端，reboot-clean 配对 A/B。性能只记录。

**阴性对照**：关掉双块（回到共享一块）必须让至少一条具名用例**由绿转红**——否则这个机制
什么也没证明。这是 ID-122 的教训：对照要证伪的是机制本身。

---

## 7 与 P6 的关系

P5f 收官后，P6 的"只是传输替换"才第一次成立，而且是**被机器证明过的**，不是被引用的。
P6 的 `a6` 只读审计随之缩小为一次核验，而不是一次发现。

`P6-SPAWN-PLAN.md` 与 `P6-CONTRACT-DRAFT.md` 里凡写"`PENDING a6`"的行，
其中属于 §2 的部分改由 P5f 回答。
