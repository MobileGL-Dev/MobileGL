# P2 specification, extracted from `docs/Disaggregated/` (read-only scout)

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated @ e7a6a72f`
(`git log --oneline -3` → `e7a6a72f [Docs] (Disaggregated): record the P1 landing and what its verify lane found`).

Documents opened in full:

| file | lines | size |
|---|---|---|
| `docs/Disaggregated/README.md` | 57 | 5,048 B |
| `docs/Disaggregated/ARCHITECTURE.md` | 601 | 90,120 B |
| `docs/Disaggregated/ROADMAP.md` | 90 | 21,462 B |
| `docs/Disaggregated/MEASUREMENTS.md` | 143 | 16,695 B |

All docs are in Chinese; requirement wording is quoted verbatim (Chinese) with a translation only where it aids the implementer. Every claim below carries the `file:line` I actually opened.

Status line, `README.md:3`:
> 状态：**P0、P0.5、P1 已落地**（`feat/disaggregated`，基线 `dev@50fb1343`）。下一步 P2，第 43 天 GO/NO-GO。见 `ROADMAP.md`。

(Note the baseline drift across docs: `README.md:3` says baseline `dev@50fb1343`; `ROADMAP.md:3` and `ARCHITECTURE.md:3` still say "P0 已落地（`feat/disaggregated@458ccde1`）"; `81b17c0b` is used only as the *performance* anchor, `ARCHITECTURE.md:504`, `ROADMAP.md:30`.)

---

## 1. The P2 row of the ROADMAP phase table, and every other P2 mention

### 1.1 P2 row — verbatim (`ROADMAP.md:18`)

Table header is `ROADMAP.md:13`: `| 阶段 | 天 | 落地什么 | 验收门 | 依赖 |`.

**阶段**: `**P2** 渲染状态 CSO + 第一片 Track H + 残余值块`
**天**: `18–26`

**落地什么 (deliverables), verbatim:**
> `MG_Impl/Pipe/Tracker`（dirty 位、5 个聚合世代、抑制器骨架）；`gen_pipe_dirty_surface.py` 首轮映射成门；`MGPipeRenderStateSpans` + G7 setter 一致性测试；`CsoCache`（64 项，键 = pipeline 子集）；`create/bind_render_state` + `set_dynamic_state`（Espryt `SyncRenderState` 一行不动；Magma `ComputePipelineStateHash`/`GetOrCreatePipeline`/`ApplyDynamicDrawStateTail` 改从 CSO 与动态 payload 取）；`set_pixel_pack_state`、`set_patch_state`、`set_vertex_attrib_defaults`；`set_residual_value_state` + `ResidualValueBlock` 绊线；**第一片 Track H**：Espryt 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`/`g_fbSlotCache`/GC）与 Magma 子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，删前端 VAO 里的后端裸指针）；`MOBILEGL_PIPE_LEGACY_MEMOS`；补 `FramebufferSrgb`/`DepthClamp` 存储

Itemised (11 deliverables):
1. `MG_Impl/Pipe/Tracker` — dirty bits, 5 aggregate generations, suppressor skeleton.
2. `gen_pipe_dirty_surface.py` first-round mapping **promoted to a gate**.
3. `MGPipeRenderStateSpans` + the G7 setter-consistency test.
4. `CsoCache` — 64 entries, key = the pipeline subset.
5. `create/bind_render_state` + `set_dynamic_state`; **Espryt `SyncRenderState` not one line changed**; Magma `ComputePipelineStateHash` / `GetOrCreatePipeline` / `ApplyDynamicDrawStateTail` re-sourced from the CSO and the dynamic payload.
6. `set_pixel_pack_state`
7. `set_patch_state`
8. `set_vertex_attrib_defaults`
9. `set_residual_value_state` + the `ResidualValueBlock` tripwire.
10. First Track H slice — Espryt 0b and Magma subsystem 4 (detail in §2.7 below).
11. `MOBILEGL_PIPE_LEGACY_MEMOS`; add real storage for `FramebufferSrgb` / `DepthClamp`.

**验收门 (acceptance gate), verbatim:**
> 集成 × 2 后端 × {pull, push} 逐名相同；40 trace push 下 SSIM ≥ 0.99 双后端；verify 零分歧；`HandleRecycleScenario` 绿且重键前红；G7 测试绿且拿掉一个字段能红；两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内；Blaze3D blend-toggle 微基准；CSO 内容寻址关闭的负面对照

Itemised (7 gate clauses):
1. integration × 2 backends × {pull, push} — **identical test-name-by-test-name**.
2. 40 traces under push: SSIM ≥ 0.99, both backends.
3. verify mode: zero divergence.
4. `HandleRecycleScenario` green **and red before the rekey**.
5. G7 test green **and removable-field negative control turns it red**.
6. Two devices, paired: per-thread CPU p50/p99 not worse **and** tracker absolute ns within the ceiling.
7. Blaze3D blend-toggle microbenchmark; negative control with CSO content addressing switched off.

**依赖**: `P1`.

### 1.2 Every other P2 mention

- `ROADMAP.md:9` — track membership: `两条跑道：**monolith 跑道** P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止且 monolith 严格好于起点；**IPC 跑道** P5 → P6 → P9 → P10 → P11 → P12。`
- `ROADMAP.md:17` (P1 row) — two rewrites were **deferred into P2**: `pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）`. Same deferral restated at `ARCHITECTURE.md:344` (Phase A proof column: `（空守卫/三元的重写推迟到 P2）`). Concretely, the P1 mechanical pass left ~7 empty guards (`空守卫`) and 3 ternaries (`三元`) among the 58 non-arrow conversions; P2 rewrites them.
- `ROADMAP.md:19` (P3a) — `依赖 | P2`.
- `ROADMAP.md:24` (P7) — `§5.5 其余 10 个子系统（子系统 1、4 已在 P2）`; dependencies `P0.5、P2；可与 P5/P6/P8 并行`. **Doc discrepancy:** `ARCHITECTURE.md:202` §5.5 is "sampler view 在 client 侧解析" and contains no 12-subsystem list — the "§5.5 subsystem" numbering is a dangling reference to the deleted `PLAN.md` (see Appendix A).
- `ROADMAP.md:32` — cumulative low-end day count: `P1 25 → P2 43 → P3a 61`. P2 therefore spans days 26–43 (low-end).
- `ROADMAP.md:39` — `- **第 43 天（P2 出口）：GO/NO-GO**。`
- `ROADMAP.md:57` — NO-GO branch: `**不回滚**：P0/P0.5/P1/P2 的产物（句柄基建与重键、两个头文件抽取、计数器、verify harness、渲染状态 CSO）全是自洽的 monolith 交付物，留在 `dev``.
- `ROADMAP.md:58` — `真正只为 MGPipe 押上的是 P1 + P2 ≈ 28–39 天`.
- `ROADMAP.md:74` (open question 1) — `P2 的头号数字，逐线程 CPU + 绝对 ns，两台设备。`
- `ARCHITECTURE.md:81` (G7 row of the generator table) — `G7 | `PipeSpanTable.inc` | render-state pipeline 子集的成员名表（24 个，取自 `ComputePipelineStateHash` 今天哈希的字段，`scripts/gen_pipe.py:67-92`）；带 `offsetof` 的 chunk 表与 setter 一致性测试在 P2`.
- `ARCHITECTURE.md:147` — the whole of §5 is scoped `## 5. 前端 state tracker（`MG_Impl/Pipe/Tracker`，P2 起）`.
- `ARCHITECTURE.md:187` — `MG_Pipe/MGPipeRenderStateSpans.{h,cpp}`（P2）.
- `ARCHITECTURE.md:359` — `所以 P2 发一个**显式临时**调用 `SetResidualValueState(MGPBlobRef)``, and `只在 P2..P13 存在`.
- `ARCHITECTURE.md:552` — build layout: `MobileGL/MG_Impl/Pipe/   Tracker、SlotAllocator、CsoCache、HostResolve、CompositeResolver  [P2+]`. (Only Tracker/SlotAllocator/CsoCache are P2; HostResolve is P8 per `ROADMAP.md:25`, CompositeResolver is P4a per `ROADMAP.md:20`.)
- `ARCHITECTURE.md:580` — switch table: `| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON（P2..P13） | 计划（编译期臂） |`.
- `ARCHITECTURE.md:367` — why that switch exists (quoted in §2.7.3 below).
- `MEASUREMENTS.md:115` — `其中 8 行是静态过近似（代码路径可达但通道未跑到），过近似会让那对 (类, 字段) 的 poison 永久失效，**P2 收紧填充表时先复查这 8 行**。` — **an explicit P2 work item not in the ROADMAP row**: when P2 tightens the fill table, first re-check those 8 statically over-approximated (verb-class, field) rows.
- `MEASUREMENTS.md:117` — push-on-mutation (`MGP_NOTE_MUTATION`, `MG_Pipe/PipeMutation.h`) `…"推送块在每次读取时都等于活上下文"这条不变式因此字面成立，也正是 P2 tracker 需要的形状。` — the P1 solution is stated to be exactly the shape the P2 tracker needs.

---

## 2. What ARCHITECTURE.md says (the P2 design surface)

### 2.1 §5.1 Push happens at validate, before the verb — not in the GL setter (`ARCHITECTURE.md:149-155`)

`ARCHITECTURE.md:151` (the reason, verbatim):
> Blaze3D 每个 batch 用 `glEnable/glDisable(GL_BLEND)` 包住（Espryt 代码自己标它为最热路径），per-setter 推送会把每次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，严格慢于今天。正确形态是 gallium `st_validate_state`。

`ARCHITECTURE.md:153` — **eight validate entry points**, generated from the `kCtxVerb`/`kCtxObject` entries of `PipeCalls.def`:
`ValidateForDraw`（20 个 draw 入口）、`ValidateForDispatch`、`ValidateForClear`、`ValidateForBlitOrCopy`、`ValidateForTextureOp`（GenerateMipmap / CopyTex* / BindImageTexture）、`ValidateForReadback`、`ValidateForXfbSpan`、`ValidateForQuery`.
Why eight and not four: `MG_Impl` 用到的 70 个表项里只有约 22 个是 draw/dispatch，其余 ~48 个（clear、blit、copy、回读、barrier、XFB 跨度、query/sync）很多自己就读 `pGLContext`.

`ARCHITECTURE.md:155` (verbatim, a hard rule):
> **只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook。纹理 subdata 不在此列（§6）。

### 2.2 §5.2 Dirty bits: zero new bookkeeping for value class, five new aggregate generations for object class (`ARCHITECTURE.md:157-174`)

Table at `ARCHITECTURE.md:161-169` (dirty bit | class | shutter source):

| dirty 位 | 类 | 快门来源 | line |
|---|---|---|---|
| `NEW_RENDER_STATE` / `NEW_PIPELINE_STATE` | 值 | `m_version` / `m_pipelineStateVersion` | 161 |
| `NEW_PIXEL_PACK`、`NEW_PATCH_STATE`（`BitwiseEqual`，NaN 合法）、`NEW_VERTEX_ATTRIB_DEFAULTS`、`NEW_VERTEX_ELEMENTS`（VAO config version） | 值 | 既有计数器 | 162 |
| `NEW_SHADER`、`NEW_SHADER_BINDINGS`、`NEW_GLOBAL_CONSTANTS` | 值 | link/image-unit/backend-state/block-binding/uniform-write-set/UBO-content 版本 | 163 |
| `NEW_VERTEX_BUFFERS` | 对象 | **`VertexArrayState::m_anyVaoAttributeGeneration`**（新增）→ 命中后走 32 属性前缀 | 164 |
| `NEW_INDEX_BUFFER` | 对象 | 索引 slot 版本 + 绑定对象 `{slot,gen}` | 165 |
| `NEW_FRAMEBUFFER` | 对象 | **`FramebufferState::m_anyAttachmentGeneration`**（新增）+ 对象/slot 版本 → 重算 `ContentHash` | 166 |
| `NEW_SAMPLER_VIEWS`、`NEW_SAMPLERS`、`NEW_SHADER_IMAGES` | 对象 | **`TextureState::m_anyTextureContentGeneration` + `m_anyTextureParamsGeneration`**（新增）+ bind/sampling-resolution generation → 走 `GetMaxTouchedUnit()` 前缀、重算集合 hash | 167 |
| `NEW_CONST_BUFFERS` / `NEW_SHADER_BUFFERS` / `NEW_SO_TARGETS` | 对象 | **`BufferState::m_anyBufferChangeGeneration`**（新增）→ 走 `GetTouchedBindPointCount()` 前缀 | 168 |

**The five aggregate generations** are therefore: `VertexArrayState::m_anyVaoAttributeGeneration`, `FramebufferState::m_anyAttachmentGeneration`, `TextureState::m_anyTextureContentGeneration`, `TextureState::m_anyTextureParamsGeneration`, `BufferState::m_anyBufferChangeGeneration` (two of the five live in `TextureState`).

`ARCHITECTURE.md:170` (verbatim, the rationale + the cost claim):
> 五个聚合世代全部落在既有 bump 点上（约 20 行），把对象类组的快门从"每 validate 走查 192 单元 / 84×4 绑定点 / 32 属性 / 40 attachment"降成一次 `Uint64` 比较；对象类不能靠轮询逐对象版本（没有聚合能回答"有没有哪张已绑定纹理动了"，这正是 Magma 不得不用有损 `sampledContentSum` 的原因）。

`ARCHITECTURE.md:174` — wrap-around discipline:
> 三个回绕 `Uint16` 在 tracker 边界加宽（`m_lastPushed[]` 是 tracker 自己的字段，不改 `MG_State`）；回绕在 tracker 本地无害（多一次重推，永不漏推），且被集合 hash 抑制器吞掉。

### 2.3 §5.3 Render state design D-B1 (`ARCHITECTURE.md:176-190`)

Interface shape, verbatim (`ARCHITECTURE.md:178-182`):
```
create_render_state(cso, MGPBlobRef pipelineSubsetChunks)      // 只带 pipeline 子集
bind_render_state(cso, Uint16 version, Uint16 pipelineVersion)  // 稳态 12 B
set_dynamic_state(MGPBlobRef dynamicChunks, Uint16 version)     // 只带动态子集的变化 chunk
```

- **Whole blob on the wire** (`ARCHITECTURE.md:184`): `整块的理由：`RenderStateParameters` 是平凡可复制 POD，Espryt 自己 `static_assert` 并做 head/blend/tail 三段 memcmp，**字段顺序承重**（`ScissorBoxWrittenMask`、`ClipDistanceEnabledMask` 故意放在 tail 段）；拆成 blend/depth-stencil/rasterizer 三个 CSO 要手工维护 ~150 字段划分表且无绊线。`
- **CSO identity = the pipeline subset only** (`ARCHITECTURE.md:185`): `子集身份的理由：整块内容寻址会让 `glViewport`/`glScissor`/`glBlendColor`/`glClearColor` 每次铸造新 CSO、冲掉 server 的 pipeline memo——`RenderState.h` 记录的那次回归。`RenderState.cpp` 里 viewport/scissor/line-width 族只 `++m_version`，`SET_CAPABILITY` 与 pipeline 相关 setter 才 `BumpVersions()`。`
- **The dynamic subset** (`ARCHITECTURE.md:186`, verbatim, this is the enumeration to implement): `动态子集：viewport、scissor、depth range、blend color、line width、polygon offset、stencil ref/write mask、clear 值、sample coverage、hints、point-size 族。`
- **`MGPipeRenderStateSpans` — the split is written in exactly one place** (`ARCHITECTURE.md:187`, verbatim): `划分只写在一处：`MG_Pipe/MGPipeRenderStateSpans.{h,cpp}`（P2）的 chunk 表 + `MGPipeComputePipelineSubsetHash()`，从 Magma 的 `ComputePipelineStateHash` 搬来，client 与两个后端共用；G7 的 `MG_Test` 遍历每个 `RenderState` public setter，断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变`。`
  - The generated member-name table already exists: G7 → `PipeSpanTable.inc`, `render-state pipeline 子集的成员名表（24 个，取自 `ComputePipelineStateHash` 今天哈希的字段，`scripts/gen_pipe.py:67-92`）；带 `offsetof` 的 chunk 表与 setter 一致性测试在 P2` (`ARCHITECTURE.md:81`). So P2 adds the `offsetof`-carrying chunk table on top of the 24 names, plus the test.
- **Server side** (`ARCHITECTURE.md:188`, verbatim — contains the "not one line" requirement): `server 侧：每 context 一份 working `RenderStateParameters`（1168 B），`bind` 与 `set_dynamic_state` 各把自己的 chunk 散射进去。**Espryt 的 `SyncRenderState`（693 行）拿到的仍是 `const RenderStateParameters&`，单 `Uint16` 早退、三段 memcmp 一行不动**；Magma 的 pipeline memo 键是 `cso.slot`，动态尾巴仍走 `ApplyDynamicDrawStateTail`。Espryt 的 head/blend/tail 划分（驱动侧增量）与 pipeline/dynamic 划分（线上与身份）是两回事，并存、各有绊线。` (The per-context working copy is restated at `ARCHITECTURE.md:378`, §10.1.)
- **Client value-fetch order** (`ARCHITECTURE.md:189`, verbatim — this is the CsoCache lookup algorithm): `client 取值顺序：`m_pipelineStateVersion` 未变 → 复用上一个 CSO handle，零哈希；变了 → 对 pipeline 子集算 xxHash（~25-30 字，Magma 今天就在算）→ CSO map 探测 → 命中发 12 B bind，未命中发变化 chunk 的 create 再 bind；`m_version` 变而子集未变 → 只发 `set_dynamic_state`（~200 B）。`
- **`FramebufferSrgb` / `DepthClamp`** (`ARCHITECTURE.md:190`, verbatim): ``FramebufferSrgb` 与 `DepthClamp` 今天**没有存储**（`glEnable` 被静默吞掉且不报错，六个后端读点恒为 false）；chunk 表冻结前要补真存储并把 `FramebufferSrgb` 划进 pipeline 半边（它改变 attachment/blend 的解释）——待拍板，见 `ROADMAP.md`。`

Payload sizes (`ARCHITECTURE.md:70` = §4.1 row): `MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState` = `48 / **12** / 32`, "§5.3". Confirmed by the measured `static_assert` list, `MEASUREMENTS.md:86`: ``MGPBindRenderState` **12**` and `ResidualValueBlock` **1248**（其中 `RenderStateParameters` 1168）`.

Call-catalogue placement: `SetDynamicState`(B) is the first of the 17 `kCtxState` entries (`ARCHITECTURE.md:95`); `CreateRenderState`/`DeleteRenderState`/`BindRenderState` are among the 13 `kCtxCso` entries (`ARCHITECTURE.md:94`). Flag `B` = `kHasBlob` (`ARCHITECTURE.md:99`).

CSO cache capacities for all five classes (`ARCHITECTURE.md:63`, §2.3, verbatim):
> CSO 在 client 侧内容寻址（Mesa `cso_cache` 先例）：每类一张 `ska::flat_hash_map<xxHash, MGPipeHandle>`，容量上限 render-state 64 / vertex-elements 1024 / sampler 256 / sampler-view 4096 / shader 跟随 `ProgramObject` 生命周期，LRU 淘汰时发 `delete_*`。两个不同 program 设置了相同状态时 server 零状态转换。

CSO handle kinds and reserved handles: `ARCHITECTURE.md:38-39` (`RenderStateCso` is one of the 13 kinds; `{0,0}` = null; `{0,1}` of `Framebuffer` = default FB; top 1/16 of `ShaderCso` slot space reserved for program-pipeline composites, `MobileGL/MG_Pipe/MGPipeHandles.h:88-90`).

### 2.4 §5.4 Validation invariant, coalescing and the suppressors (`ARCHITECTURE.md:192-200`)

`ARCHITECTURE.md:194` — the normative invariant (D-B3), verbatim:
> 规范（D-B3）：**一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态惰性特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间没有顺序要求。** 推荐实现顺序（framebuffer → program → 纹理/sampler/image/buffer/global constants → render state/dynamic → vertex elements/buffers/index/attrib defaults → patch/XFB → verb）只是代码组织，不是契约。

`ARCHITECTURE.md:196` — `create_shader_state` 从编译池的终止 continuation 发出（不是从 draw）.

`ARCHITECTURE.md:198` — **the four coalescing rules**, verbatim (rule 4 is the suppressor P2 must scaffold):
> 四条合并规则：整块结构优于逐字段；高水位标记（`GetTouchedBindPointCount`、`GetMaxTouchedUnit`）留在 tracker 走查里，直接就是 `count` 实参；只发 program 解析过的集合（`uniformSamplerOrImageUnitIndex`）；**集合 hash 抑制器**——每条 `kVarTail` `set_*` 在 client 算已解析集合的 xxHash，未变不发。最后一条是从后端搬到 client 的 ~175 行去抖（`UnitBindingsSnapshot`/`PairingsIntact`/`g_fboTextureSyncList` 族）的载体：`GetTextureBindGeneration()` 在冗余重绑时也 bump（MC 26.2 每次纹理单元切换都重绑同一个 sampler），没有抑制器每个 batch 都会重发一条几百字节的变长记录并冲掉 server 的两个 memo。

Which calls the suppressor covers (`ARCHITECTURE.md:145`, §4.1 tail): `每条 `kVarTail` 的 `set_*`（`SetVertexBuffers`、`SetSamplerViews`、`BindSamplerStates`、`SetShaderImages`、`SetShaderBuffers`、`SetStreamOutputTargets`）都带 `ContentHash`——与 `MGPFramebufferState` 同一模式，hash 未变就不发（§5.4）。` The `MGPFramebufferState.ContentHash` doubles as server render-pass memo key and client emission suppressor (`ARCHITECTURE.md:134`).

`ARCHITECTURE.md:200` — 索引绑定范围在 validate 时刻实时解析（`glBindBufferBase` 之后再 `glBufferData` 是普通应用代码）.

Note: the P2 ROADMAP row asks only for the suppressor **skeleton** (`抑制器骨架`, `ROADMAP.md:18`); the full landing of the debounce移动 from backend to client is P3b/P4b (`ROADMAP.md:23`: `同时**在 Tracker 落地集合 hash 抑制器`).

### 2.5 `set_pixel_pack_state` / `set_patch_state` / `set_vertex_attrib_defaults`

- All three are `kCtxState` entries (`ARCHITECTURE.md:95`): `SetVertexAttribDefaults`(V)、`SetPixelPackState`、`SetPatchState`. `V` = `kVarTail` (`ARCHITECTURE.md:99`) — so `SetVertexAttribDefaults` carries a variable-length tail (and per `ARCHITECTURE.md:145` therefore also a `ContentHash` suppressor).
- `MGPPixelPackState` = 28 B, `只有 PACK 方向（D5）` (`ARCHITECTURE.md:138`); measured `static_assert` 28 (`MEASUREMENTS.md:86`).
- There is deliberately **no** `set_pixel_unpack_state` (`ARCHITECTURE.md:107`, verbatim): `set_pixel_unpack_state`（不存在：前端已在 `glTexImage` 时解析压缩格式、强制默认 unpack）.
- `MGPPatchState` = 40 B, `同时是 shader variant 输入` (`ARCHITECTURE.md:139`).
- Dirty bits for these three are value-class, driven by existing counters — `NEW_PIXEL_PACK`、`NEW_PATCH_STATE`（`BitwiseEqual`，NaN 合法）、`NEW_VERTEX_ATTRIB_DEFAULTS` (`ARCHITECTURE.md:162`). The `BitwiseEqual`/NaN-legal note is a requirement on the patch-state comparison.
- Vertex attribute default values are part of the `Track V` value class (`ARCHITECTURE.md:353`: `stencil/colormask/depthmask/scissor/patch/attrib 默认值`), and `PixelStoreParameters` + `VertexAttribute` types were already extracted to `MG_Pipe/MGPipeValueTypes.h` in P0.5 (`ARCHITECTURE.md:261`, `ROADMAP.md:16`).

### 2.6 §9.4 The residual value block and its tripwire (`ARCHITECTURE.md:357-359`)

`ARCHITECTURE.md:359`, verbatim (the whole spec):
> Track V 的 55% 不需要逐字段接口条目就能跑起来，所以 P2 发一个**显式临时**调用 `SetResidualValueState(MGPBlobRef)`，payload `ResidualValueBlock{RenderStateParameters, PixelStoreParameters, CapabilityBits, patch 三字段}`。三条纪律：退役是编译错误（`MGL_RESIDUAL_BLOCK_SIZE` 只降不升，`MobileGL/MG_Pipe/MGPipeTypes.h:535`，P13 变成 `static_assert(sizeof == 0)`）；布局逐成员 `offsetof` 断言且 split 下逐字段序列化（异质 POD 并集的 padding 差异 monolith verify 看不见）；只在 P2..P13 存在，`MOBILEGL_PIPE_STATS` 单独计一类字节（`ResidualValueBlock`，P0 已占位）。

Three disciplines, restated: (a) retirement is a **compile error** — `MGL_RESIDUAL_BLOCK_SIZE` may only ratchet **down**, never up, and becomes `static_assert(sizeof == 0)` at P13; (b) per-member `offsetof` asserts **and** field-by-field serialisation under split (monolith verify cannot see padding differences in a heterogeneous POD union); (c) it exists only during P2..P13, and `MOBILEGL_PIPE_STATS` counts its bytes as their own class.

Supporting facts:
- Catalogue entry: `迁移期临时的 `SetResidualValueState`(B)` — the 17th `kCtxState` call (`ARCHITECTURE.md:95`).
- Size: `ResidualValueBlock` = **1248** B (`ARCHITECTURE.md:140`, `MEASUREMENTS.md:86`; of which `RenderStateParameters` is 1168 — same 1168 as the server-side working copy, `ARCHITECTURE.md:188`).
- Counter slot already reserved in P0: `residual-value-block`（占位） (`ARCHITECTURE.md:601`).
- P13 removes it: `删 `set_residual_value_state`` and `static_assert(sizeof(ResidualValueBlock) == 0)` 编译通过 (`ROADMAP.md:30`).
- Gate part 5 lists it: ``ResidualValueBlock` 的 `offsetof` 断言与 P13 的 `sizeof == 0`` (`ARCHITECTURE.md:505`).

### 2.7 Track V and Track H, and the first Track H slice

#### 2.7.1 Definitions

- Terminology (`README.md:52`, verbatim): `**Track V / Track H**：值类读点的迁移（整块 POD 过线）/ 对象类读点的迁移（`SharedPtr<前端对象>` → 句柄）。`
- `ARCHITECTURE.md:353` (§9.3), verbatim: `Track V（值类型：`GetRenderStateParameters`、`GetPixelStoreParameters`、capability 位、stencil/colormask/depthmask/scissor/patch/attrib 默认值、Magma ~22 个标量 getter……约 B 类读点的 55%）：机械。`
- `ARCHITECTURE.md:354`: `Track H（对象类型：167 个 `SharedPtr<MG_State…>` 点）：真活。` ("real work")
- `ARCHITECTURE.md:355` — read-point census: `A 探测变化 ~35（12%）、B 翻译输入 ~216（74%）、C 瞬时参数 ~4、D 身份/缓存键 ~48（与 B 重叠）、E 数据字节 3、写 8。74% 是 B 类——"bump 一个版本让 server 自己拉"行不通，值本身必须过去。`
- The three-stage strangler (`ARCHITECTURE.md:343-346`): A 别名 (done, P1) / B 推送 / C 句柄化. Stage C's proof column is `全套门（§13）`.

#### 2.7.2 The first Track H slice — Espryt 0b and Magma subsystem 4

`ROADMAP.md:18`, verbatim: `**第一片 Track H**：Espryt 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`/`g_fbSlotCache`/GC）与 Magma 子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，删前端 VAO 里的后端裸指针）`

Espryt 0b components:
1. `SlotAllocator` (lives in `MG_Impl/Pipe/`, `ARCHITECTURE.md:552`).
2. 6 registries → slot arrays.
3. Delete `TwinLookupMemo` ×3.
4. Delete `OwnerEquals`.
5. Delete `g_fbSlotCache`.
6. Delete the GC (`ARCHITECTURE.md:363` calls it `GC ×6` alongside the same-address `weak_ptr`; the deleted PLAN says `2 个 GC 扫描`, Appendix A).

Magma subsystem 4 components: rekey `VertexInputStateFactory` and `VaoDrawMemo`; **delete the backend raw pointers stored in the frontend VAO**.

Backing detail from §9.5 (`ARCHITECTURE.md:363`, verbatim — the 21-memo rekey census that Espryt 0b/Magma 4 draw from):
> 统一事实：每个进入 memo 键的版本计数器要么是回绕 `Uint16`，要么根本不会被它害怕的那个 mutation bump；身份比较是堵回绕洞的补丁。`{slot, gen}` + 显式 destroy 让 **11 条直接删除**（registry 的同址 `weak_ptr` + GC ×6、`TwinLookupMemo` ×3 + `OwnerEquals`、`UnitSamplerLookupMemo` 的 `WeakPtr` 测试、`SetBackendStateMemo`、`VkTextureManager::TextureIdentity` 存活探测、`ConvertedVertexStreamKey` 的 `sourcePin`……），**2 条** server 删除但去抖搬到 client（§5.4），**7 条重键**成更便宜的比较（`StampSyncedFBO` 四元组 → `ContentHash` + server 私有 `attachmentRemintEpoch`；`ResolvedTextureBindingMemo` 9 键 → `(shaderCso.slot, viewSetSerial)`；`SetupDrawSnapshot` 的 ~14 探测字段与两个有损求和 → 三个 handle + 两个 server 纪元 + dirty mask；`VertexInputStateFactory::ComputeHash` 里的 lifetimeId → `gen` **混进** server 侧每个 content hash），**1 条**（D18）原样不动。

So the Magma-4 rekey rule is precisely: `VertexInputStateFactory::ComputeHash` stops using `lifetimeId` and instead mixes the handle `gen` into every server-side content hash. The corresponding frontend-VAO raw pointer is `SetBackendStateMemo` — `ARCHITECTURE.md:284` (§8.1 tail): `**`SetBackendStateMemo`（前端 VAO 里存后端堆裸指针）直接删除**` (verbatim: ``SetBackendStateMemo`（前端 VAO 里存后端堆裸指针）直接删除`); also `SetBackendResource` 删除（server 拥有资源表）and `SetBackendHashMemo/AuxMemo` → server 侧 per-slot 字段.

Handle/slot rules the SlotAllocator must obey (`ARCHITECTURE.md:35-40`, §2.1):
- 8-byte POD, passed by value in a register pair; **client mints, server never returns a handle** → zero create round trips (deviation D1) (`:35`).
- `slot 稠密、**按 kind 分配**（free list + 高水位），server 对象表是数组而非哈希表。与 `IndexGenerator` 无关——后者的 LIFO 名字复用正是句柄要关掉的问题。` (`:36`)
- ``gen` 只在 slot 复用时 ++，不在 respecify 时 ++；同一 slot 复用 2³² 次才回绕（1000 fps 逐帧复用约 50 天），debug 分配器断言回绕。` (`:37`)
- 13 kinds (`:38`); reserved handles (`:39`); `GL name 只以 `GlNameForDiag` 出现…永不做身份、永不进 memo 键或 content hash；`GetLifetimeId()` 留在 client 作 tracker 自己的身份，client 维护 `lifetimeId → slot`。` (`:40`)
- Two generation spaces stay strictly separate (`ARCHITECTURE.md:44-49`): client `MGPipeHandle::Gen` crosses the wire, server `MGGen` **never** does.
- Lifetime ordering constraints expressed by payload (`ARCHITECTURE.md:208`): view destroyed before its storage owner (`ViewOf` + server keep-alive), FBO attachment pins the texture, buffer texture pins the buffer.

#### 2.7.3 `MOBILEGL_PIPE_LEGACY_MEMOS` (§9.6, `ARCHITECTURE.md:365-369`)

`ARCHITECTURE.md:367`, verbatim (why it must be a compile-time arm):
> `MOBILEGL_PIPE_PUSH` 子系统位图（含一位关闭 CSO 内容寻址，负面对照）在阶段 B 是真正的旧-vs-新 A/B；阶段 C 之后不是——位清零时 `SnapshotFromGLContext()` 仍要合成句柄，后端仍跑重键后的 memo 代码，一个重键 bug 两臂都在。对策：**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）在 P3a/P4a 期间保留 registry / `TwinLookupMemo` 实现活在同一个 `PipeInputs` 接口之下，随 pull 路径在 P13 退役（各阶段 +1 天维护）。

Switch table: `| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON（P2..P13） | 计划（编译期臂） |` (`ARCHITECTURE.md:580`) and the runtime row `| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON | 三态读取，只有显式 falsy 才关 |` (`ARCHITECTURE.md:591`). Maintenance cost is explicitly budgeted at +1 day per phase.

#### 2.7.4 The `HandleRecycleScenario` gate

`ARCHITECTURE.md:501` (purity gate block, verbatim tail): `外加 debug 断言"每个后端 memo 键都是 `{slot, gen}`，永不是裸前端指针"，由 `HandleRecycleScenario` 支撑（重键前必须在至少一个后端上是红的）。`
`ROADMAP.md:18` restates: ``HandleRecycleScenario` 绿且重键前红`.

### 2.8 The dirty-surface gate `gen_pipe_dirty_surface.py` — and which phase owns it

**The docs assign the gate promotion to P1, and the first-round mapping to P2.** Both statements exist; they are consistent only if read as: the script becomes a CI gate at P1, and P2 lands the first real mapping table behind it.

- `ARCHITECTURE.md:172` (§5.2 tail), verbatim: `完整性由 `scripts/gen_pipe_dirty_surface.py` 保证：枚举 `MG_Impl/GLImpl` 里每个 mutator → 必须 bump 的聚合世代，CI 重生成 + `git diff --exit-code`，未映射即失败（**P1 起成为门**）。**实测规模**：926 次 mutator 调用落在 73 个不同 mutator 上，其中 92 次（7 个 mutator，绝大多数 `RecordError`）位于同函数内也会到达后端的"即时发布点"，其余 834 次由紧随其后的 verb 发布——映射表是 73 条目的问题。`
- `ARCHITECTURE.md:568` (§16 CI bullet): ``gen_pipe_dirty_surface.py --summary`（信息性，**P1 成门**）` — i.e. informational in P0, a gate from P1.
- `ROADMAP.md:15` (P0 row): the script itself is listed as already landed (✅) in P0.
- `ROADMAP.md:18` (P2 row): ``gen_pipe_dirty_surface.py` 首轮映射成门` — **P2 delivers the first-round mapping, promoted to a gate.**
- `ARCHITECTURE.md:505` (gate part 5): ``gen_pipe_dirty_surface.py` 重生成 0 未映射 mutator`.
- Measured surface (`MEASUREMENTS.md:81`, verbatim): `**dirty-surface 面**（`python3 scripts/gen_pipe_dirty_surface.py --summary`，本树）：`MG_Impl/GLImpl` 41 个文件，926 次 mutator 调用，73 个不同 mutator（`RecordError` 一项就占 836 次）；92 次（36 个即时发布点、7 个 mutator，绝大多数是 `RecordError`）位于同函数内也到达后端的入口，其余 834 次由紧随的 verb 发布。映射表是 73 条目的问题。`

(Numeric discrepancy to be aware of: `ARCHITECTURE.md:172` attributes "绝大多数 `RecordError`" to the 92 immediate-publish sites; `MEASUREMENTS.md:81` says `RecordError` alone accounts for 836 of the 926 calls and names 36 immediate-publish points across 7 mutators.)

### 2.9 Other P2-relevant architecture the row does not spell out

- Sampler-view resolution stays on the client with its own memo, ~40 lines moved (`ARCHITECTURE.md:204`, §5.5) — that is P4a work but constrains the tracker shape.
- Composite program pipeline (`ARCHITECTURE.md:210`): tracker pushes **one** handle, slot from the reserved high segment of `ShaderCso`, `gen++` and `delete_shader_state` on pipeline-cache eviction.
- Emulation ownership rule (`ARCHITECTURE.md:214`, verbatim): `**驱动表达不了的变换在 tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering。**`
- The `PipeInputs`/`MGB_CTX` scaffolding P2 builds on: `ARCHITECTURE.md:325-340` (code block), fill points per verb class (`ARCHITECTURE.md:348`), per-verb generation poison and its `Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}` shape (`ARCHITECTURE.md:349`), and `纯度门 grep 的是 `pGLContext` 不是 `pGLContext->`` (same line).
- Push-on-mutation, already landed in P1 and named as the shape P2's tracker needs (`MEASUREMENTS.md:117-124`): `MGP_NOTE_MUTATION(Field)` in `MG_Pipe/PipeMutation.h`, hooked on **three counters**, not on forty write sites — `GetSamplingResolutionGeneration` ← `TextureState::BumpSamplingResolutionGeneration()`; `GetTextureBindGeneration` ← `TextureState::BumpTextureBindGeneration()` and the `bindingChanged` branch of `NoteUnitTouched()`; `GetMaxTouchedTextureUnit` ← the high-water branch of `NoteUnitTouched()` (`MEASUREMENTS.md:121-123`).
- P1 scale numbers P2 inherits (`MEASUREMENTS.md:105-112`): 277 arrow sites (Espryt 113 / Magma 164), 58 non-arrow lines (Espryt 9 / Magma 49), 63 `PipeInputs` fields, 62 distinct accessors (Espryt 32 / Magma 56), **83 `MGP_FILL` fill points covering 69 verbs across 9 classes** (`MG_Pipe/FillPoints.def`), 20 `SyncPersistentMappedRange` / 6 `SyncGpuWrites`.

---

## 3. The day-43 GO/NO-GO checklist, in full (`ROADMAP.md:42-58`)

Heading `ROADMAP.md:42`: `## 第 43 天 GO/NO-GO 清单`; `ROADMAP.md:44`: `手上必须有：` ("you must have in hand").

Verbatim checklist (`ROADMAP.md:46-52`):
- [ ] P1 交付的逐 draw 逐字段语义等价证明（40 trace + 全部集成测试零分歧）
- [ ] 两个后端上都已推送的渲染状态，`SyncRenderState` 693 行一行未动
- [ ] 两片 Track H 的实测单位成本（Espryt 0b、Magma 子系统 4）
- [ ] 两台设备（Adreno 830 `35d0befa`、Mali `3B159D009VZ00000`）reboot-clean 配对的逐线程 CPU 时间增量，p50 与 p99
- [ ] tracker 每 draw 的**绝对 ns**（上限从设备基线定：稳态每 draw 6.5–9.3 次 accessor + memo 探测，见 `MEASUREMENTS.md`）
- [ ] Blaze3D blend-toggle 微基准（enable/draw/disable/disable，MC batch 速率）  ← *doc reads* `（enable/draw/disable/draw，MC batch 速率）`
- [ ] 负面对照：关掉 CSO 内容寻址重跑，把"推送更慢"与"CSO 设计更慢"分开

(Item 6 verbatim is `Blaze3D blend-toggle 微基准（enable/draw/disable/draw，MC batch 速率）`, `ROADMAP.md:51`.)

**Which items P2 must produce** (item 1 is P1's; items 2–7 are all P2 deliverables):
| # | item | produced by |
|---|---|---|
| 1 | per-draw per-field semantic equivalence proof, 40 traces + all integration tests, zero divergence | **P1** (already landed — `MEASUREMENTS.md:126-135`: 79/79 retrace verify, 818 integration-verify, 1485 unit ×3, zero `Fatal{`) |
| 2 | pushed render state on both backends, `SyncRenderState` 693 lines untouched | **P2** (deliverable 5) |
| 3 | measured unit cost of the two Track H slices | **P2** (deliverable 10) |
| 4 | two-device reboot-clean paired per-thread CPU delta, p50 and p99 | **P2** (gate clause 6) |
| 5 | tracker absolute ns per draw, ceiling derived from the device baseline of 6.5–9.3 accessors + memo probes per draw | **P2** (gate clause 6) |
| 6 | Blaze3D blend-toggle microbenchmark | **P2** (gate clause 7) |
| 7 | negative control with CSO content addressing off | **P2** (gate clause 7) |

Verdict criteria and exits (`ROADMAP.md:54-58`), verbatim:
> 判据与出口：
> - **继续**：两台设备 p50 与 p99 逐线程 CPU 增量都不为负；tracker 绝对 ns 在上限内；Track H 单位成本不超出估计的 50%。按两条跑道推进。
> - **收缩为 headless 工装用途或重新评估**：任一判据落空。**不回滚**：P0/P0.5/P1/P2 的产物（句柄基建与重键、两个头文件抽取、计数器、verify harness、渲染状态 CSO）全是自洽的 monolith 交付物，留在 `dev`；MGPipe 收缩为 `MG_Test` mock 后端 → MGPipe recorder（给 trace_replay 一种记录已解析状态的录制格式）+ `inproc` 渲染线程实验；IPC 跑道搁置到出现新判据。
> - 沉没成本：P0 与 P0.5 无论走哪条路都要花（后者本身是 monolith 净收益）；真正只为 MGPipe 押上的是 P1 + P2 ≈ 28–39 天，NO-GO 分支下仍留下上述产物。

Note the GO criterion wording `两台设备 p50 与 p99 逐线程 CPU 增量都不为负` — the *delta must not be negative*, i.e. push must not cost more than pull, on both devices, at both percentiles.

Related milestone lines (`ROADMAP.md:38-40`):
> - **第 25 天（P1 出口）**：verify harness 逐 draw 逐字段证明"推送等价于拉取"。零产品风险，**不是** GO/NO-GO。
> - **第 43 天（P2 出口）：GO/NO-GO**。

Where the absolute-ns ceiling comes from (`MEASUREMENTS.md:64`, verbatim):
> **真机稳态动态 accessor 成本是每 draw 6.5–9.3 次**（预测区间 10–25 的下沿；llvmpipe 的 15.5/20.7 是 memo 冷的）。推送要打败的是 ~8 次 accessor + memo 探测，不是 124/169 的静态调用点数。GO/NO-GO 的 tracker 绝对 ns 上限从这里定。

Per-device, per-backend accessor baselines to compare against (`MEASUREMENTS.md:50-58`, acc/draw column): MC 1.21.4 in-world Espryt **9.28** / Magma **8.56** (91.6 draws/frame); Iris BSL (memo cold) Espryt 21.04 / Magma 11.26 (23.2 draws/frame); improved-transparency 26.3 Espryt **8.44** / Magma **6.53** (1320 draws/frame). `accessor-calls` 是约 10 个热入口的静态计数，是每 draw accessor 数的**下界** (`MEASUREMENTS.md:60`). Memo-gate hit/miss abbreviations at `MEASUREMENTS.md:60`: `ers = EsprytRenderState、etl = EsprytTextureSyncList、eub = EsprytUnitBindingsEpoch、mfp = MagmaDrawFastPath、mpm = MagmaPipelineMemo、mdt = MagmaDynamicTail`（`MobileGL/MG_Util/Metrics/PipeStats.h:46-122`）.

Devices (`MEASUREMENTS.md:3`): `35d0befa` = Xiaomi 24129PN74C, Adreno 830, Android 16; `3B159D009VZ00000` = Oppo PLG110, Mali, Android 16 (ColorOS).

Gate part 4 of the five-part validation gate defines *how* to measure (`ARCHITECTURE.md:504`, verbatim):
> 4. **monolith 性能不回归**：两台设备 reboot-clean、同热窗口、配对 A/B，`tools/bench.sh` + trace replay `--benchmark` 逐帧 JSON；**指标是逐线程 CPU 时间**，p50 与 p99；**绝对阈值**——tracker 每 draw 的 ns 公布并设上限（真实拉取基线只有每 draw 6.5–9.3 次 accessor，相对噪声阈值会平凡通过）；Blaze3D blend-toggle 微基准单列；关掉 CSO 内容寻址的负面对照。

And gate part 3 defines the name-for-name baseline (`ARCHITECTURE.md:503`): `逐名功能基线是"P1 出口的重构后 monolith"（P1 出口先用 verify 证明等价于 `81b17c0b`）；`81b17c0b` 只作性能锚点。` SSIM default threshold 0.99; CTS conformance within 0.5 pp, `行 = GL 版本/扩展，列 = 状态计数，rate = Pass/(Pass+Fail)，NS 不进分母`.

Re-baseline checkpoints table (`ROADMAP.md:60-68`) — none fires inside P2, but the first (`P3a > 27 天`) is the immediate successor; `任一触发，先跑 `inproc` 的证伪数字再决定是否继续。` (`ROADMAP.md:68`).

---

## 4. Open questions that touch P2 (`ROADMAP.md:70-89`)

Preamble `ROADMAP.md:72`: `P0 已回答的不再列出（spike A 的域、spike B 的分档、`posix_spawn` 不可用、OOM 探测惯用法、`GetInteger64i_v`/`GetProgramiv` 退役、D21 与 `RenderbufferObject` lifetime id、动态 accessor 基线）。`

**Q1 — the headline P2 number** (`ROADMAP.md:74`, verbatim):
> 1. **client 侧 dirty 走查的真实每 draw CPU 代价。** 拉取基线已实测为每 draw 6.5–9.3 次 accessor + memo 探测；推送要在这个数字下净减少。P2 的头号数字，逐线程 CPU + 绝对 ns，两台设备。

**Q4 — CSO LRU capacity and `set_dynamic_state` chunk granularity** (`ROADMAP.md:77`, verbatim):
> 4. **渲染状态的 wire 粒度。** chunk 划分定下来后，CSO LRU 容量（暂定 64）与 `set_dynamic_state` 的 chunk 粒度由计数器定。

Reading: 64 is provisional (`暂定`); both the LRU capacity and the dynamic-chunk granularity are to be **decided by the boundary counters**, after the chunk split is settled. The P2 row nevertheless specifies `CsoCache`（64 项）(`ROADMAP.md:18`) and §2.3 fixes 64 for render state (`ARCHITECTURE.md:63`) — so build to 64 and let the counters retune it. P13 re-tunes surviving cache capacities generally with counters live (`ROADMAP.md:30`), and open question 14 says `**推送模型改变哪些按拉取模式调过的缓存命中率。** 幸存者容量在 P13 重调。` (`ROADMAP.md:87`).

**Q5 — `FramebufferSrgb` / `DepthClamp` decision** (`ROADMAP.md:78`, verbatim — note the freeze prohibition):
> 5. **`FramebufferSrgb` / `DepthClamp` 的拍板。** 事实已清（无存储、`glEnable` 静默吞掉、六个读点恒 false、41 个 fixture 无一开启）；建议在 chunk 表冻结前补真存储并把 `FramebufferSrgb` 划进 pipeline 半边。由计划所有者拍板，**拍板前不冻结 chunk 表**。

Supporting measurement (`MEASUREMENTS.md:84`, verbatim):
> - **`FramebufferSrgb` / `DepthClamp`**：`FramebufferSrgb` 的六个后端读点全部消费一个编译期常量 `false`，`DepthClamp` 零读点；两者的 `glEnable` 落到 `RenderState.cpp` 的 `default:` 分支既不存储也不报 `GL_INVALID_ENUM`；41 个 fixture 无一开启任一项（补真存储不会改动任何既有 fixture 的输出）。

And the design-side statement at `ARCHITECTURE.md:190` (quoted in §2.3): add real storage before freezing the chunk table and put `FramebufferSrgb` on the **pipeline** half, because it changes the interpretation of attachments/blend. `DepthClamp` has no stated half — it has zero read points, so it is a pure storage addition; the pipeline/dynamic assignment for it is not decided in the docs.

**Other open questions with P2 relevance (secondary):**
- Q2 (`ROADMAP.md:75`) — texture-remint pull rate; touches `MOBILEGL_PIPE_TEXEL_RETAIN_MB` default 0, P8/P9-era but the `ImageBindableHint` prevention is set at resource-create time.
- Q9 (`ROADMAP.md:82`) — `**viewport-array 回放能否塞进一次 `draw_vbo`**：`EndViewportRoutingPasses` 会 `InvalidateSyncedRenderState`，各遍之间观察到的状态是否与今天一致未验证。` — this is a render-state-adjacent unknown that P2's CSO/dynamic split can perturb.
- Q14 (`ROADMAP.md:87`) — cache hit rates changed by the push model; retuned at P13.
- Q17 (`ROADMAP.md:89`) — `create-indirect` fixture fails on Adreno 830 **on the `dev@81b17c0b` baseline itself**, must be fixed on `dev` first; corroborated at `MEASUREMENTS.md:96` (Adreno 830: Espryt black frame after ~4.5 min, Magma `VK_ERROR_DEVICE_LOST` during texture upload submit; Mali SSIM 0.85 / 0.45). Relevant because it is one of the 40 traces the P2 SSIM gate runs over.

---

## 5. Commit / CI discipline

### 5.1 Per-commit discipline (`ROADMAP.md:5-9`)

Heading `ROADMAP.md:5`: `## 通用纪律（每个 commit）`. Verbatim (`ROADMAP.md:7`):
> 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议；每阶段出口跑一次五部分门；每阶段性能判据是**逐线程 CPU 时间**。

Seven rules: (1) the default ALL target must build completely; (2) no hot-path instrumentation may be committed (CI grep gate); (3) **every gate must be able to go red for the reason it exists**; (4) the Windows machine is not a correctness gate; (5) device comparisons are reboot-clean + same-thermal-window paired A/B with CPU frequency pinned per project protocol; (6) run the five-part gate once at each phase exit; (7) the per-phase performance criterion is per-thread CPU time.

Track rule (`ROADMAP.md:9`): every monolith-track segment must be independently deliverable, abortable at any time, and leave the monolith **strictly better than the starting point**.

CTS turnaround is priced separately (`ROADMAP.md:34`, verbatim):
> **CTS 周转单独计价**：`gl44to46` 约 56,271 例。逐阶段只跑该阶段可能影响的具名块（P4a `packed_pixels`、P3b/P4b `texture_*`/`shader_image_*`、P9 `transform_feedback*`）；完整 caselist 只在五个架构边界（P0.5、P3a、P4a、P3b/P4b、P13）与每次合并 `dev` 之前跑，放 CI 不放关键路径。若周转仍主导排期，加宽估时而不是削弱门。
**P2 is not one of the five architecture boundaries** requiring a full caselist, and no named CTS block is assigned to P2.

### 5.2 CI jobs (`ARCHITECTURE.md:568`, verbatim)

> CI（`.github/workflows/test.yml:809` `pipe-gates`，P0 已落地）：`gen_pipe.py` 重生成 + diff；`MG_Backend`/`MG_State` 下禁止 stdio 插桩的 grep 门；`gen_pipe_dirty_surface.py --summary`（信息性，P1 成门）；`check_doc_citations.py`（警告级，文档定稿后 `--strict`）。独立 job `flatc-check`。后续：`include-graph-check`（P0.5）、`monolith-symbol-report`。

Generated artifacts are committed into the tree and CI regenerates + `git diff --exit-code` (`ARCHITECTURE.md:69`): `七个生成器（`scripts/gen_pipe.py`，产物提交进树，CI `pipe-gates` 重生成并 `git diff --exit-code`）`. `PipeCalls.def` opcodes are file positions, so `新调用只能**追加**到文件末尾、退役的调用保留槽位` (`ARCHITECTURE.md:75`); `MGP_CALL_LIST_DOCUMENTED_COUNT = 71` is pinned by `MG_Test/Pipe/PipeCatalogueTest.cpp` (`ARCHITECTURE.md:75`, citing `MobileGL/MG_Pipe/PipeCalls.def:69`).

Doc-citation lint: `scripts/check_doc_citations.py` lints `file:line` citations in this doc directory (`README.md:39`) — every `file:line` a P2 doc edit adds must be true.

### 5.3 The five-part validation gate to run at P2 exit (`ARCHITECTURE.md:497-507`)

Heading `ARCHITECTURE.md:497`: `### 13.2 五部分验证门（取代 monolith 的字节一致门）`; `ARCHITECTURE.md:499` explains the byte-identity gate dies by construction.

1. **Interface purity, three gates** (non-verify builds only) (`ARCHITECTURE.md:501`): A = include graph (remove `MG_State/GLState` from the include search path when compiling `MG_Backend` in the disaggregated config; depends on P0.5); B = symbols (`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` empty); C = undeclared (`grep -c 'pGLContext' MG_Backend/` == 0). Plus the `{slot, gen}` memo-key debug assertion backed by `HandleRecycleScenario`.
2. **Semantic shadow comparison `MOBILEGL_PIPE_VERIFY=1`** (`ARCHITECTURE.md:502`) — the decisive one; per-field, per-draw; catches fields the tracker forgot to push and **dirty bits that fire too rarely（危险方向）**; third CI mode, 40 traces + all integration tests, ~5–10× slower, never shipped; per-field not `memcmp` (padding false-DIFFERs); **保留模式** for consume-and-clear groups (texture dirty rects) compares the emitted `(UnionBox, RegionCount, Regions[])`; survives P13.
3. **Behavioural A/B** (`ARCHITECTURE.md:503`) — 40 traces under `{monolith-pull, monolith-push, split}` SSIM ≥ 0.99; `ctest -L integration-gpu` name-for-name identical between `DirectGLES.` and `DirectGLES.Pipe.`/`DirectGLES.Split.` (same for DirectVulkan); unit tests all green; CTS per-backend conformance within 0.5 pp; `TextureUploadShapeScenario` golden shape comparison.
4. **monolith performance non-regression** — quoted in §3 above.
5. **Coverage + poison + handle discipline** (`ARCHITECTURE.md:505`): G6 regenerates with 0 UNMAPPED; `gen_pipe_dirty_surface.py` regenerates with 0 unmapped mutators; per-verb generation poison; G7 setter-consistency test; `ResidualValueBlock` `offsetof` asserts and the P13 `sizeof == 0`.

Two surviving byte-level equalities (`ARCHITECTURE.md:507`): with `MOBILEGL_BUILD_DISAGGREGATED=OFF`, `nm --defined-only libMobileGL.so | grep MG_Remote` is empty and the link line gains no library; `nm -D libMobileGL.so | grep mobilegl_server_main` hits in RelWithDebInfo. Symbol/`.text` drift is published per phase as an informational metric.

### 5.4 Harness traps that will bite P2's measurement runs (`MEASUREMENTS.md:98-104`)

1. `MEASUREMENTS.md:98` — the trace app never reaches `MobileGL::DestroyImpl`, so `MOBILEGL_PIPE_STATS_FILE` JSON is never written on device; only the periodic summary lines in `mobilegl.log`; traces shorter than one period produce nothing. Set `MOBILEGL_PIPE_STATS_PERIOD` small enough.
2. `MEASUREMENTS.md:99` — `run_android_retrace_local.py` shares one `.trace-work/android-retrace-result` root per tree and `rmtree`s it per invocation, so **two devices must be run serially from one tree**.
3. `MEASUREMENTS.md:100` — `/data/...` inside `--env` values gets MSYS path-converted; run with `MSYS_NO_PATHCONV=1`.
4. `MEASUREMENTS.md:101` — `coherent_as_flush` goes through `--ez coherent_as_flush true`, independent of `--env`.
5. `MEASUREMENTS.md:102` — `minecraft-1.21.1-neoforge-create-indirect-in-world` fails on both devices, reproducible on the `dev@81b17c0b` baseline APK; not caused by this branch.

Reproduction command (`MEASUREMENTS.md:71-75`):
```sh
ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 \
python3 tools/trace_replay/run_android_retrace_local.py \
  --case minecraft-1.21.4-in-world --backend DirectGLES \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
# 数字在结果目录的 mobilegl.log 里，grep 'MGPipe stats:'，取最后一个完整窗口
```

ColorOS install trap (`MEASUREMENTS.md:18`): first `adb install` of a not-yet-installed package hangs on `com.oplus.appdetail InstallGuideActivity` until "继续安装" is tapped (`input tap 353 2349` on a 1272×2772 panel); same-signature reinstalls pass silently.

### 5.5 Switches P2 touches (`ARCHITECTURE.md:576-597`)

CMake (`ARCHITECTURE.md:578-583`): `MOBILEGL_PIPE_VERIFY` OFF (build-time, compiles in `SnapshotFromGLContext()` + the G4 comparator, retained past P13); `MOBILEGL_PIPE_LEGACY_MEMOS` ON (P2..P13, compile-time arm).
Runtime, landed in P0 (`MobileGL/Config.h:319-358`, `MobileGL/ConfigLoader.cpp:245-256`, `ARCHITECTURE.md:586`): `MOBILEGL_PIPE_PUSH` default 0 — `子系统位图（0 = 全 pull），含一位关闭 CSO 内容寻址；十进制或 `0x`` (`ARCHITECTURE.md:588`) — **that bit is the P2 negative control**; `MOBILEGL_PIPE_VERIFY`, `MOBILEGL_PIPE_STATS`, `MOBILEGL_PIPE_LEGACY_MEMOS`（三态读取，只有显式 falsy 才关）, `MOBILEGL_PIPE_TEXEL_RETAIN_MB` 0, `MOBILEGL_PIPE_INDEX_MIRROR_MB` 64, `MOBILEGL_PIPE_STATS_PERIOD` 120, `MOBILEGL_PIPE_STATS_FILE`.
Counters available for the P2 numbers (`ARCHITECTURE.md:601`, §附 B): byte classes incl. `residual-value-block`（占位）; call classes `draws`、`accessor-calls`、`texture-upload-emissions/box/rect/jobs`; six memo gates each hit/miss (`SyncRenderState` 早退、`SyncNeccessaryTextures` 键比较、`CurrentUnitBindingsEpoch` 快门、`TrySetupDrawFastPath`、pipeline memo、`ApplyDynamicDrawStateTail`); a 24-bucket per-draw payload histogram already implemented, waiting for the first emitter; `站点清单——哪些路径**没有**接线——写在 `MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`，那份清单是契约。`

### 5.6 Commit message convention (project standing rule, not in these docs)

Single-line `[Type] (Scope): description`; never add `Co-Authored-By` or other attribution lines. (User memory rule, applies to every commit on this branch; the branch's own history matches, e.g. `e7a6a72f [Docs] (Disaggregated): record the P1 landing and what its verify lane found`.)

---

## Appendix A — supplementary P2 detail from the deleted `PLAN.md` (git history, **not** current doctrine)

`README.md:57` says the 328 KB `PLAN.md` and 135 KB `REVIEW.md` were deleted and live only in git history (`8b31de2f`、`1794ac94`、`8349babe`、`87ee17c6`). `ROADMAP.md:24`'s "§5.5 subsystem" numbering and the names "Espryt 0b" / "Magma 子系统 4" originate there. `git show 87ee17c6:docs/Disaggregated/PLAN.md` yields detail the current ROADMAP compresses away — useful to the implementer, but treat the current four docs as authoritative where they differ:

- P2 day breakdown (PLAN §, line 2261 of that blob): `P2 | 18-26 | 43 | Espryt 1(3-5) + Magma 1(3-4) + Espryt 0b(5-7) + Magma 4(2-3) + tracker/CSO/G7(4-6) + 聚合世代(1)`.
- Espryt subsystem 0b row (line 1170): `handle 基建；6 个 registry → slot 数组；删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / **2 个 GC 扫描**`, estimated `5-7` days, risk `低`.
- Render-state deliverable (line ~2145): the Espryt side keeps `RenderStateImpl`'s 693-line body, the single-`Uint16` early-out, the three-segment memcmp, `g_syncedColorMaskAlphaWidenMask` and the dual-source decline **unchanged** (eliminating 4 read points); the Magma side re-sources `ComputePipelineStateHash` / `GetOrCreatePipeline` / `ApplyDynamicDrawStateTail` from the CSO and the dynamic payload (**eliminating ~55 read points**); **both version numbers cross the wire**.
- Chunk table provenance: `从 `VulkanRenderer.cpp:4826-4906` 原样搬来` (line ~2143).
- `CsoCache`: `64 项 LRU，键是 **pipeline 子集**的 xxHash（**不是整块**，D-B1 v2）`.
- Additional named integration scenarios the PLAN's P2 acceptance called out beyond the ROADMAP row: `ClipDistance`、`SampleMaskScope`、`SampleVariables`、`DualSourceBlend`、`ViewportArray`、`PrimitiveRestart` — worth running, since they are exactly the render-state-sensitive ones.
- The PLAN's P2 acceptance also states the paired-device criterion as `monolith-push 在 p50 与 p99 逐线程 CPU 上落在 monolith-pull 噪声内或更好，**并且 tracker 每 draw 的绝对 ns 落在预设上限内**（相对阈值不够）`.

---

## Appendix B — discrepancies / cautions for the implementers

1. **`ROADMAP.md:24`'s "§5.5 其余 10 个子系统" is a dangling cross-reference.** `ARCHITECTURE.md:202` §5.5 is "sampler view 在 client 侧解析". The 12-subsystem Magma decomposition (from which "subsystem 1" = pipeline+dynamic state and "subsystem 4" = VertexInput/VaoDrawMemo come) exists only in the deleted `PLAN.md`.
2. **Baseline commit differs across docs**: `README.md:3` `dev@50fb1343`; `ROADMAP.md:3` and `ARCHITECTURE.md:3` both still say `feat/disaggregated@458ccde1`; `81b17c0b` is the performance anchor only.
3. **`PipeInputs` field count**: `ARCHITECTURE.md:79` (G5 row) says 61; `MEASUREMENTS.md:107` says the landed count is **63** (`计划写 61；GetBoundTransformFeedbackLifetimeId、HasOpenTransformFeedbackSpan 是 D21 之后新增的读点`). Same for accessors: plan said Espryt 32 / Magma 55 (`ARCHITECTURE.md:328`), landed is Espryt 32 / Magma 56 (`ROADMAP.md:17`, `MEASUREMENTS.md:108`, which also notes `GetBoundTransformFeedbackName` is a dead annotated line).
4. **Fill points**: `ROADMAP.md:17` says `~93 个边界站点` (also `ARCHITECTURE.md:348`); the landed measurement is **83 `MGP_FILL`, 69 verbs, 9 classes** in `MG_Pipe/FillPoints.def` (`MEASUREMENTS.md:110`).
5. **The dirty-surface gate's phase**: promoted to a gate at P1 per `ARCHITECTURE.md:172` and `:568`, while the *first-round mapping* is a P2 deliverable per `ROADMAP.md:18`. Build P2 assuming the CI job already fails on unmapped mutators and that P2 supplies the 73-entry table.
6. **The 8 over-approximated poison rows** (`MEASUREMENTS.md:115`) are a P2 obligation the ROADMAP row does not mention: a (verb class, field) pair that is statically over-approximated has its poison permanently disabled, so re-check those 8 before tightening the fill table.
7. `ROADMAP.md:18` says the Espryt 0b deletion list includes "GC" (singular); `ARCHITECTURE.md:363` counts `GC ×6` together with the same-address `weak_ptr`; the deleted PLAN says `2 个 GC 扫描`. Resolve against the code.
