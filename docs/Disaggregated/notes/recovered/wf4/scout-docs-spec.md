# P1 specification, extracted from the MGPipe design docs

Scout report. Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg` (branch `feat/disaggregated`). Read-only; nothing was edited.

Docs read in full: `docs/Disaggregated/README.md` (57 lines), `ARCHITECTURE.md` (601 lines), `ROADMAP.md` (90 lines), `MEASUREMENTS.md` (96 lines). Every code claim below carries a path + line that I actually opened in this tree.

Quoting convention: `>` blocks are **verbatim** from the doc (Chinese as written). Everything else is my summary or my own measurement.

---

## 0. Where P1 sits

- `docs/Disaggregated/README.md:3`: `> 状态：**P0、P0.5 已落地**（`feat/disaggregated@5d99ee43`，基线 `dev@50fb1343`）。下一步 P1 → P2，第 43 天 GO/NO-GO。见 `ROADMAP.md`。`
- Track: monolith lane. `ROADMAP.md:9` — `> 两条跑道：**monolith 跑道** P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止且 monolith 严格好于起点；**IPC 跑道** P5 → P6 → P9 → P10 → P11 → P12。`
- Milestone: `ROADMAP.md:38` — `> - **第 25 天（P1 出口）**：verify harness 逐 draw 逐字段证明"推送等价于拉取"。零产品风险，**不是** GO/NO-GO。`
- Cumulative day count: `ROADMAP.md:32` — P0 9 → P0.5 15 → **P1 25** → P2 43 …
- Sunk-cost framing: `ROADMAP.md:58` — `> 真正只为 MGPipe 押上的是 P1 + P2 ≈ 28–39 天，NO-GO 分支下仍留下上述产物。`
- Under NO-GO, P1's output is explicitly **not rolled back** (`ROADMAP.md:57`): `> **不回滚**：P0/P0.5/P1/P2 的产物（句柄基建与重键、两个头文件抽取、计数器、verify harness、渲染状态 CSO）全是自洽的 monolith 交付物，留在 `dev``.

---

## 1. The P1 row of the phase table (verbatim), `ROADMAP.md:17`

Columns are: 阶段 | 天 | 落地什么 | 验收门 | 依赖.

**阶段**: `**P1** `PipeInputs` 替换与 verify harness`
**天**: `10–13`

**落地什么 (deliverables), verbatim:**

> `MG_Backend/MGPipe/PipeInputs.h`（Espryt 32 / Magma 55 访问器）；`sed` 293 处 + 58 行非箭头清单逐条转换（显式交付物）；逐 verb 类填充点（G5 表，~93 个边界站点）；逐 verb 世代 poison；G4 影子比对器 + 第三种 CI 模式；20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表

Broken out — six deliverables:
1. `MG_Backend/MGPipe/PipeInputs.h`, Espryt 32 accessors / Magma 55 accessors.
2. The mechanical `sed` of 293 arrow sites **plus** a per-line conversion of the 58-line non-arrow list — the doc calls this an **explicit deliverable** (i.e. the list itself is a produced artifact, not a side effect).
3. Per-verb-class fill points: the G5 table, ~93 boundary sites.
4. Per-verb generation poison.
5. G4 shadow comparator + **a third CI mode**.
6. A per-site ownership table for the 20 `SyncPersistentMappedRange` and 6 `SyncGpuWrites` sites.

**验收门 (acceptance gate), verbatim:**

> pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）；40 trace + 全部集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；故意损坏一个快照字段能让 verify 变红；故意在 `glGenerateMipmap` 的填充表漏一个字段能在**那条 verb** 上触发 poison Fatal

Four gate clauses:
1. On the **pull** build, `nm --defined-only` unchanged; `.text` differences attributable line by line. Empty-guard / ternary rewrites are **deferred to P2** — do not do them in P1, they would break this gate's attributability.
2. 40 traces + **all** integration tests, run under `MOBILEGL_PIPE_VERIFY=1`, **zero divergence**.
3. Negative control A: deliberately corrupting one snapshot field must turn verify red.
4. Negative control B: deliberately omitting one field from `glGenerateMipmap`'s fill table must raise the poison Fatal **on that verb** (not on a later draw). This is what makes the poison a per-verb *generation* rather than a bitmap.

**依赖**: `P0.5`. And from the P0.5 row (`ROADMAP.md:16`, last cell): `> P0；**P1 与 P7 的硬前置**` — P0.5 is a hard prerequisite for P1 and P7.

### Every other P1 mention in the docs

| Where | Verbatim / content |
|---|---|
| `ARCHITECTURE.md:172` | `> 完整性由 `scripts/gen_pipe_dirty_surface.py` 保证：枚举 `MG_Impl/GLImpl` 里每个 mutator → 必须 bump 的聚合世代，CI 重生成 + `git diff --exit-code`，未映射即失败（P1 起成为门）。` Also gives the measured surface: 926 mutator calls over 73 distinct mutators; 92 of them (7 mutators, mostly `RecordError`) sit at "即时发布点"; the other 834 are published by the following verb. `> 映射表是 73 条目的问题。` |
| `ARCHITECTURE.md:323` | Section title: `### 9.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充 + poison 世代（P1）` — §2 below. |
| `ARCHITECTURE.md:503` (gate 13.2-③) | `> 逐名功能基线是"P1 出口的重构后 monolith"（P1 出口先用 verify 证明等价于 `81b17c0b`）；`81b17c0b` 只作性能锚点。` **P1's exit becomes the by-name functional baseline for every later phase.** |
| `ARCHITECTURE.md:553` | Build layout: `MobileGL/MG_Backend/MGPipe/    PipeInputs.h + MGPipeImpl_DirectGLES/DirectVulkan.cpp   [P1+]` — i.e. P1 creates that directory with **three** files: the header plus one impl TU per backend. (The directory does not exist yet: `MobileGL/MG_Backend/` currently holds only `BackendObject.cpp`, `BackendObject.h`, `BackendObjects.h`, `Init.cpp`, `DirectGLES/`, `DirectVulkan/`.) |
| `ARCHITECTURE.md:568` | CI: `> `gen_pipe_dirty_surface.py --summary`（信息性，P1 成门）` |
| `ARCHITECTURE.md:583` (附 A) | `> `MOBILEGL_PIPE_VERIFY` \| OFF \| 计划（P1；构建期开关，编译进 `SnapshotFromGLContext()` 与 G4 比对器，P13 后保留）` — note **CMake-level** switch, distinct from the runtime env var of the same name. |
| `ROADMAP.md:46` (GO/NO-GO) | `> - [ ] P1 交付的逐 draw 逐字段语义等价证明（40 trace + 全部集成测试零分歧）` |
| `ROADMAP.md:18` | P2's 依赖 cell is `P1`. |

In-tree code comments that name P1 (these are contracts the P1 implementer inherits):

- `MobileGL/MG_Pipe/generated/PipeFilled.inc:26` / `scripts/gen_pipe.py:462`: `// P0 is the skeleton: the enum, the tables and the assertion helper exist, PipeInputs // itself lands in P1.`
- `MobileGL/MG_Pipe/generated/PipeFilled.inc:161` / `scripts/gen_pipe.py:479`: `// true has to be argued for in P1 when the fillers land: a sticky field is a field // the poison cannot protect.`
- `MobileGL/MG_Pipe/PipeFields.def:16`: `// here makes the comparator blind to it; that gap closes in P1, when the verify harness // goes live and the comparator's coverage is itself asserted.`
- `scripts/gen_pipe.py:165` and `MobileGL/MG_Pipe/generated/PipeVerify.inc:236`: the memcmp fallback types (`RenderStateParameters`, `PixelStoreParameters`, `DynamicBackendParameters`, `MGHostSpan`, listed at `scripts/gen_pipe.py:167-172`) `// P1 gives them field lists of their own, at which point this branch stops being reachable from any payload.`
- `MobileGL/MG_Pipe/MGPipeTypes.h:32`: `// needs fixed-width members before it can move (a type change, not a move): P1/P7.` — about `DynamicBackendParameters` inside `MGPCaps`, which is what still keeps purity gate A off `MGPipeTypes.h`.
- `MobileGL/MG_Pipe/Coverage.def:36-40`: `GetBufferBindingSlot` is polymorphic over `BufferTarget`; `/* inventory carries the target argument (P1) */` — its rows must split across `set_vertex_buffers` / `set_index_buffer` / `set_indirect_buffers` / `set_shader_buffers` in P1.
- `scripts/gen_pipe_dirty_surface.py:21-22`: `P0 is the skeleton: it reports. P1 adds the mapping file and CI regenerates it with `git diff --exit-code` and zero unmapped mutators, the same shape as gen_pipe.py's G6.`
- `scripts/gen_pipe_dirty_surface.py:193-194`: `dirty-surface: every mutator above is UNMAPPED - the aggregate-generation mapping file lands in P1, and this report is what it has to cover.`
- `scripts/check_include_closure.py:15` and `:86` describe the P0.5 closure gate that P1 must not regress.

---

## 2. The strangler scaffold (ARCHITECTURE §9.2, lines 323–351)

### 2.1 `PipeInputs` + `MGB_CTX` — verbatim code block (`ARCHITECTURE.md:325-340`)

> ```cpp
> // MG_Backend/MGPipe/PipeInputs.h —— 按 memo 键组织，不按读点组织（~20 KB，字段集全迁移期稳定）
> struct PipeInputs {
>     const RenderStateParameters& GetRenderStateParameters() const;   // 阶段 A：类型与后端今天读到的完全一致
>     // … 每个后端真正用到的 GLContext 方法一个访问器（Espryt 32 / Magma 55）
> #if MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED
>     Uint64 m_filledGen[kFieldCount];   // 逐字段"上次填充的 verb 序号"
>     Uint64 m_currentVerbSerial;
> #endif
> };
> #if MOBILEGL_PIPE_PUSH
> #  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
> #else
> #  define MGB_CTX (::MG_State::pGLContext)
> #endif
> ```

Load-bearing details in that block:
- **Organized by memo key, not by read point.** That is why the field set is small (61) and stable across the whole migration. `MobileGL/MG_Pipe/generated/PipeFilled.inc:15-17` repeats this: `// PipeInputs is organized by MEMO KEY, not by read point, which is why the field set is // small and stable across the whole migration).`
- Size budget: `~20 KB`.
- **Phase A rule**: the accessor return types are *exactly* what the backends read today (`const RenderStateParameters&`, etc.), so the `sed` is type-neutral.
- The poison members are compiled only under `MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED`.
- `MGB_CTX` is a **compile-time** macro switched on `MOBILEGL_PIPE_PUSH`. Note the asymmetry with the runtime knob: `MOBILEGL_PIPE_PUSH` is *also* a runtime per-subsystem bitmask (`MobileGL/Config.h:320-326`, `Uint64 PipePush = 0;` at `Config.h:326`, parsed at `MobileGL/ConfigLoader.cpp:245`). The docs use the same name for both; the P1 implementer must decide/record how the macro arm and the runtime bitmask relate (see §7, Risk R3).
- `gPipeInputs` is a **single global instance** in `MobileGL::MG_Pipe` (matching `gMGPipeScreen`/`gMGPipeContext` at `MobileGL/MG_Pipe/MGPipe.h:72-73`).

### 2.2 The three-phase table (`ARCHITECTURE.md:342-347`) — verbatim

> | 阶段 | 改什么 | 证明 |
> |---|---|---|
> | A 别名 | 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（293 处）+ 手工转换 58 行非箭头用法（~34 处 `MOBILEGL_ASSERT` 删除、7 处空守卫、3 处三元、`.get()` 裸指针捕获与 `decltype` 别名、14 处 `!= nullptr`、1 处注释）；逐 verb 类填充点填 `gPipeInputs` | `nm --defined-only` 不变；`.text` 差异可逐行归因（空守卫/三元的重写推迟到 P2） |
> | B 推送 | tracker 填 `gPipeInputs`，填充器按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位 | `MOBILEGL_PIPE_VERIFY=1`：tracker 再填一份快照版，G4 比对器逐字段每 draw 比一次 |
> | C 句柄化 | `SharedPtr<前端对象>` 字段 → `MGPipeHandle` + POD 描述符；memo 重键；写回变回调 | 全套门（§13） |

**P1 is phase A plus the scaffolding for B** (the ROADMAP P1 row's `verify harness`). Phase B's per-field yielding by the `MOBILEGL_PIPE_PUSH` bitmap and phase C's handle-ification belong to P2+ (P2 lands the tracker + the first push subsystems; P3a/P4a land Track H).

The 58 non-arrow lines break down (per the doc) into: ~34 `MOBILEGL_ASSERT` deletions, 7 empty guards, 3 ternaries, `.get()` raw-pointer captures + `decltype` aliases, 14 `!= nullptr`, 1 comment. **The empty-guard and ternary rewrites are explicitly deferred to P2** so the `.text` diff stays attributable.

### 2.3 Fill points: the G5 table and the ~93 boundary sites (`ARCHITECTURE.md:348`) — verbatim

> - 填充点逐 verb 类，不只 `PrepareForDraw`/`SetupDraw` 两处：G5 从 `PipeCalls.def` 生成"每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些字段"的表，在 `MG_Impl` 的 ~93 个边界站点生成 validate/fill 调用。

Cross-references:
- G2 (`ARCHITECTURE.md:157`): `> monolith 直调 thunk `MGP_<Name>()`，`MG_Impl` 的约 93 个 `gBackendFunctionsTable.GL.*` 站点逐名改到它上面` — the same ~93 sites are where the fill calls go.
- G5 (`ARCHITECTURE.md:160`): `> `PipeFilled.inc` \| `PipeInputs` 字段 id（61 个）与逐 verb 世代 poison`
- The eight validate entry points (`ARCHITECTURE.md:153`, §5.1): `> 八个 validate 入口，由 `PipeCalls.def` 的 `kCtxVerb`/`kCtxObject` 条目生成：`ValidateForDraw`（20 个 draw 入口）、`ValidateForDispatch`、`ValidateForClear`、`ValidateForBlitOrCopy`、`ValidateForTextureOp`（GenerateMipmap / CopyTex* / BindImageTexture）、`ValidateForReadback`、`ValidateForXfbSpan`、`ValidateForQuery`。八个而不是四个，因为 `MG_Impl` 用到的 70 个表项里只有约 22 个是 draw/dispatch，其余 ~48 个（clear、blit、copy、回读、barrier、XFB 跨度、query/sync）很多自己就读 `pGLContext`。`
  - **P1 relevance**: the tracker itself lands in P2, but the *fill points* land in P1 at these same eight shapes. `glGenerateMipmap` — the P1 negative-control verb — is under `ValidateForTextureOp`.
- Class counts (`ARCHITECTURE.md:171-172` region, §3.2 table at `ARCHITECTURE.md:165-172`): `kCtxObject` = 9 calls, `kCtxVerb` = 13 calls. Those 22 rows are the ones G5 has to enumerate fields for.
- `ARCHITECTURE.md:155` (§5.1, still binding on where a push happens): `> **只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook。纹理 subdata 不在此列（§6）。`

**Measured in this tree** (my count, for the mechanical work estimate):
- `grep -rc "gBackendFunctionsTable" MobileGL/MG_Impl/` → 91 lines across 8 files: `GLImpl/Drawing/GL_Drawing.cpp` 37, `GLImpl/Framebuffer/GL_Framebuffer.cpp` 11, `GLImpl/Getter/GL_Getter.cpp` 3, `GLImpl/Program/GL_Program.cpp` 1, `GLImpl/Program/ProgramInterface.h` 1, `GLImpl/Query/GL_Query.cpp` 22, `GLImpl/Sync/GL_Sync.cpp` 6, `GLImpl/Texture/GL_Texture.cpp` 10. (Doc says "~93"; 91 lines here — a line may hold more than one site.)

### 2.4 Per-verb generation poison (`ARCHITECTURE.md:349`) — verbatim

> - poison 是**逐 verb 世代**不是位图：每次 verb 递增 `m_currentVerbSerial`，字段被填时记下序号，读取时断言相等（跨 verb 有效的字段显式标 sticky）。位图看不见"上一个 draw 填过、紧随的 `glTexSubImage` 读到陈旧值"。debug 与 disaggregated 构建里读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}`。纯度门 grep 的是 `pGLContext` 不是 `pGLContext->`。

So, precisely:
- **What is poisoned**: every one of the 61 `PipeInputs` fields, individually.
- **When**: `m_currentVerbSerial` is bumped once per verb; a fill stamps the field with the current serial; a read asserts `stamp == currentVerbSerial` unless the field is marked *sticky*.
- **Where it is live**: `MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED` builds only.
- **The Fatal**: `Fatal{UnmigratedPipeInput, "<FieldName>@<Verb>"}` — the message names the field *and* the verb.
- **Last sentence is a separate rule**: purity gate C greps for `pGLContext`, not `pGLContext->` — so an `MGB_CTX` alias that still spells `pGLContext` in the `#else` arm inside `MG_Backend` will trip gate C. (Gate C: `ARCHITECTURE.md:502` — `> **C 门未声明**——`grep -c 'pGLContext' MG_Backend/` == 0`.) The `MGB_CTX` macro definition therefore cannot live under `MG_Backend/` at P13; in P1 gate C is not yet green anyway (it turns green with the pull path's retirement, `ARCHITECTURE.md:369`).

**The skeleton already in the tree** — P1 fills it in, it does not invent it. `MobileGL/MG_Pipe/generated/PipeFilled.inc` (308 lines, generated by `scripts/gen_pipe.py` from `Coverage.def` + `PipeCalls.def`, included from `MGPipe.h:86`):

- `PipeFilled.inc:19-26` states the poison contract verbatim in code: `// The poison is a per-verb GENERATION, not a bit. A bitmap cannot see the dangerous case: // a field filled by the previous DRAW and then read by the glTexSubImage that follows is // stale, and its bit is already set. So every verb bumps CurrentVerbSerial, filling a // field stamps it with that serial, and reading a non-sticky field whose stamp is older is // Fatal{UnmigratedPipeInput} (section 6.2.2).`
- `PipeFilled.inc:28-91`: `enum class MGPipeInputField : Uint16 { … kFieldCount }` — the 61 field ids, in this order:
  `GetActiveTextureUnit, GetBlendColor, GetBlendEquationIndexed, GetBlendFuncIndexed, GetBoundTransformFeedbackName, GetBoundVertexArray, GetBufferBindingSlot, GetBufferBindingPoint, GetBufferBindingPointCount, GetTouchedBufferBindingPointCount, GetClampReadColor, GetClearColor, GetClearDepth, GetClearStencil, GetColorMaskIndexed, GetCullFaceMode, GetCurrentVertexAttribute, GetDepthFunc, GetDepthMask, GetDepthRangeIndexed, GetFramebufferBindingSlot, GetImageTextureBinding, GetLineWidth, GetLogicOp, GetMaxTouchedTextureUnit, GetMinSampleShadingValue, GetPatchDefaultInnerLevel, GetPatchDefaultOuterLevel, GetPatchVertices, GetPipelineStateVersion, GetPixelStoreParameters, GetPolygonModeFront, GetPolygonOffsetFactor, GetPolygonOffsetUnits, GetPrimitiveRestartIndex, GetProgramForDispatch, GetProgramForDraw, GetProgramObject, GetProvokingVertexMode, GetRenderStateParameters, GetRenderStateParametersVersion, GetSamplingResolutionGeneration, GetScissorBox, GetStencilState, GetTextureBindGeneration, GetTextureContextId, GetTextureObject, GetTextureUnitObject, GetTransformFeedbackCapturedVertices, GetTransformFeedbackGeneration, GetTransformFeedbackPausedPrimitiveCounter, GetTransformFeedbackProgram, GetViewport, GetViewportIndexed, IsCapabilityEnabled, IsCapabilityEnabledIndexed, IsTransformFeedbackActive, IsTransformFeedbackPaused, InvalidateCompileEnv, ValidateProgramName, RecordError`
- `PipeFilled.inc:93-94`: `static_assert(kMGPipeInputFieldCount == 61, "the PipeInputs field set moved");`
- `PipeFilled.inc:96-155`: `kMGPipeInputFieldNames[]`.
- `PipeFilled.inc:158-225`: `kMGPipeInputFieldSticky[]` — **every entry is currently `false`**, and each `true` P1 introduces has to be argued for (`PipeFilled.inc:160-162`).
- `PipeFilled.inc:227-291`: `kMGPipeInputFieldFilledBy[]` — the call that must have filled each field by the time a verb reads it; names come from `Coverage.def` so `PipeCoverage.inc` and `PipeFilled.inc` cannot disagree (`PipeFilled.inc:227-228`). The last three entries are pseudo-calls: `"kClientResolved"` (×2: `InvalidateCompileEnv`, `ValidateProgramName`) and `"kReverseChannel"` (`RecordError`).
- `PipeFilled.inc:293-296`: `struct MGPipeFilledState { Uint64 CurrentVerbSerial; Uint64 FilledGen[kMGPipeInputFieldCount]; };`
- `PipeFilled.inc:298-302`: `[[noreturn]] inline void MGPipeInputPoisonFatal(MGPipeInputField field, const char* verb)` → `MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"}", …); std::abort();`
- `PipeFilled.inc:304-308`: `MGPipeInputFieldIsFresh(state, field)` → sticky fields pass when `FilledGen != 0`; non-sticky require `FilledGen == CurrentVerbSerial`.

So P1's poison work is: instantiate `MGPipeFilledState` inside `PipeInputs` (the doc's `m_filledGen[kFieldCount]` / `m_currentVerbSerial`), stamp it from the G5-generated fill points, check it in each of the 61 accessors, and argue each `sticky = true`.

The source of truth behind the field list is `MobileGL/MG_Pipe/Coverage.def:29-97` (`MGP_COVERAGE_ACCESSOR_LIST`) — one `X(Accessor, PipeCall)` row per field, which is what generates both `kMGPipeInputFieldFilledBy` and the coverage table.

### 2.5 G4 shadow comparator / `MOBILEGL_PIPE_VERIFY`

The definitive statement is gate 13.2-②, `ARCHITECTURE.md:501` — verbatim:

> 2. **语义影子比对 `MOBILEGL_PIPE_VERIFY=1`**——决定性的一条：两套状态模型活在同一地址空间，tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 比对器逐字段、每 draw 比对，打印第一个分歧字段与 draw 序号。抓 tracker 忘推的字段、**dirty 位触发得太少**（危险方向）、两条路径变换不一致的值。第三种 CI 模式，40 个 trace + 全部集成测试，~5–10× 慢，永不出货。逐字段而非 `memcmp`（padding 会 false-DIFFER）。**保留模式**：消费即清的组（纹理 dirty rect）发射后无法重算，verify 时 tracker 保留清除前的集合并比对发射出去的 `(UnionBox, RegionCount, Regions[])`。**活过 P13**。

Decomposed:
- **What is compared**: field by field, the pushed `PipeInputs` against a second `PipeInputs` filled by `SnapshotFromGLContext()` — both models live in the same address space.
- **When**: once per draw.
- **Output on failure**: the *first* differing field name + the draw serial.
- **What it is designed to catch**: (a) fields the tracker forgot to push; (b) **a dirty bit that fires too rarely** — the dangerous direction, which no purity gate can see; (c) values that the two paths transform inconsistently.
- **"Zero divergence"** means: across the 40 trace fixtures and the entire integration suite, the comparator never reports a differing field. That is exactly the ROADMAP gate and exactly GO/NO-GO item 1.
- **"The third CI mode"**: a build/run configuration alongside the existing ones, running the 40 traces + all integration tests, ~5–10× slower, **never shipped**.
- **Field-wise, never memcmp** — padding would false-DIFFER.
- **Retention mode (保留模式)**: for consume-and-clear groups (texture dirty rects) that cannot be recomputed after emission, the tracker under verify keeps the pre-clear set and compares the emitted `(UnionBox, RegionCount, Regions[])`. (P1 only needs the mechanism to the extent texture ops are in scope; the shape gold-standard `TextureUploadShapeScenario` is a P3b/P4b gate, `ROADMAP.md:23`.)
- **Survives P13**: `ARCHITECTURE.md:369` — `> P13：删 `SnapshotFromGLContext()` 的非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**保留 `MOBILEGL_PIPE_VERIFY` 连同它需要的 `SnapshotFromGLContext()` 与 `MG_State` include**（D-B5，verify 构建永不出货）；三道纯度门在非 verify 构建上转绿。` So P1 must build `SnapshotFromGLContext()` as a *permanent* verify-only facility, not a throwaway.
- **CMake vs env**: `ARCHITECTURE.md:583` lists `MOBILEGL_PIPE_VERIFY` as a CMake option (`OFF`, `计划（P1；构建期开关，编译进 SnapshotFromGLContext() 与 G4 比对器，P13 后保留）`); `ARCHITECTURE.md:589` lists the runtime env `MOBILEGL_PIPE_VERIFY` default `0`, `逐 draw 逐字段影子比对`. Both exist; P1 must wire both. Runtime side already landed: `MobileGL/Config.h:327-332` (`Bool PipeVerify = false;` at `Config.h:332`) and `MobileGL/ConfigLoader.cpp:246`.
- **G4 in the catalogue table** (`ARCHITECTURE.md:159`): `> G4 \| `PipeVerify.inc` \| `MOBILEGL_PIPE_VERIFY` 的逐字段比对器（字段表来自 `PipeFields.def`；浮点按位比较，NaN patch level 不会误报）`

**Already in the tree**: `MobileGL/MG_Pipe/generated/PipeVerify.inc` (573 lines, included at `MGPipe.h:83`).
- `PipeVerify.inc:9-23` restates the contract, including `// Floating-point fields are compared by BITS, so a NaN patch level - which // glPatchParameterfv accepts and ComputePipelineStateHash already hashes bitwise - equals // itself instead of tripping every draw.`
- `PipeVerify.inc:30-…` declares one `MGPipeVerify(const X&, const X&, const char** outField)` per payload (≈63 payloads; `MEASUREMENTS.md:82` says `63 个 verify payload`).
- `PipeVerify.inc:222-239`: the dispatch template — float → bitwise memcmp; scalar/enum → `==`; else `==` if available; **else the memcmp fallback**, reachable today only from `RenderStateParameters`, `PixelStoreParameters`, `DynamicBackendParameters` and `MGHostSpan`. `PipeVerify.inc:233-237`: `// P0.5 moved the first two into MGPipeValueTypes.h; P1 gives them field lists of their own, at which point this branch stops being reachable from any payload.` → **P1 deliverable (implied but explicit in code): add `MGP_FIELDS_*` lists for those value structs to `MobileGL/MG_Pipe/PipeFields.def`** (238 lines, format shown at `PipeFields.def:21-39`) and remove them from `MEMCMP_FALLBACK_TYPES` (`scripts/gen_pipe.py:167-172`).
- `PipeFields.def:9-17` states the zero-false-positive requirement: `// MOBILEGL_PIPE_VERIFY has to have ZERO false positives and a padding byte is exactly what makes a memcmp of RenderStateParameters false-DIFFER`.

### 2.6 Track V / Track H (`ARCHITECTURE.md:353-357`, §9.3) — verbatim

> - Track V（值类型：`GetRenderStateParameters`、`GetPixelStoreParameters`、capability 位、stencil/colormask/depthmask/scissor/patch/attrib 默认值、Magma ~22 个标量 getter……约 B 类读点的 55%）：机械。
> - Track H（对象类型：167 个 `SharedPtr<MG_State…>` 点）：真活。
> - 读点分类实测（静态）：A 探测变化 ~35（12%）、B 翻译输入 ~216（74%）、C 瞬时参数 ~4、D 身份/缓存键 ~48（与 B 重叠）、E 数据字节 3、写 8。74% 是 B 类——"bump 一个版本让 server 自己拉"行不通，值本身必须过去。

Glossary (`README.md:52`): `> **Track V / Track H**：值类读点的迁移（整块 POD 过线）/ 对象类读点的迁移（`SharedPtr<前端对象>` → 句柄）。`

P1 does neither migration — P1 makes both *addressable*: every read point, V or H, goes through a `PipeInputs` accessor. Track V's bulk moves in P2 via the residual value block; Track H's first slice moves in P2 (Espryt 0b + Magma subsystem 4) and the waves in P3a/P4a.

### 2.7 The residual value block (`ARCHITECTURE.md:359-360`, §9.4) — verbatim

> Track V 的 55% 不需要逐字段接口条目就能跑起来，所以 P2 发一个**显式临时**调用 `SetResidualValueState(MGPBlobRef)`，payload `ResidualValueBlock{RenderStateParameters, PixelStoreParameters, CapabilityBits, patch 三字段}`。三条纪律：退役是编译错误（`MGL_RESIDUAL_BLOCK_SIZE` 只降不升，`MobileGL/MG_Pipe/MGPipeTypes.h:535`，P13 变成 `static_assert(sizeof == 0)`）；布局逐成员 `offsetof` 断言且 split 下逐字段序列化（异质 POD 并集的 padding 差异 monolith verify 看不见）；只在 P2..P13 存在，`MOBILEGL_PIPE_STATS` 单独计一类字节（`ResidualValueBlock`，P0 已占位）。

**It is a P2 deliverable, not P1** — but P1 constrains it: the `PipeInputs` accessors for `GetRenderStateParameters` / `GetPixelStoreParameters` / capability bits / the three patch fields are the ones the residual block will feed in P2, so their P1 signatures must be the whole-struct-by-const-ref shape the block can satisfy.
Verified in tree: `MobileGL/MG_Pipe/MGPipeTypes.h:534` `#define MGL_RESIDUAL_BLOCK_SIZE 1248` with the ratchet comment at `MGPipeTypes.h:525-533` (`// This number only ever goes DOWN … P13 replaces it with static_assert(sizeof(ResidualValueBlock) == 0)`), the `static_assert` at `MGPipeTypes.h:535-537`, `static_assert(std::is_trivially_copyable_v<ResidualValueBlock>)` at `MGPipeTypes.h:525`(region), and `struct MGPResidualValueState { Uint32 Version; Uint32 Pad0; MGPBlobRef Blob; }` at `MGPipeTypes.h:539-543`. Size 1248 (of which `RenderStateParameters` is 1168) is confirmed at `MEASUREMENTS.md:86`.

### 2.8 `SnapshotFromGLContext()`

Mentions (there is no other definition in the docs; the function does not yet exist in the tree — `grep -rn SnapshotFromGLContext` over the source returns nothing outside `docs/`):
- `ARCHITECTURE.md:501` (gate 13.2-②): the tracker uses it to fill a second `PipeInputs` for the comparator.
- `ARCHITECTURE.md:367` (§9.6): `> `MOBILEGL_PIPE_PUSH` 子系统位图（含一位关闭 CSO 内容寻址，负面对照）在阶段 B 是真正的旧-vs-新 A/B；阶段 C 之后不是——位清零时 `SnapshotFromGLContext()` 仍要合成句柄，后端仍跑重键后的 memo 代码，一个重键 bug 两臂都在。`
- `ARCHITECTURE.md:369` (P13): delete its non-verify branch, keep the verify branch **plus the `MG_State` include it needs**.
- `ROADMAP.md:30` (P13 row): `删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；保留 `MOBILEGL_PIPE_VERIFY``.

So `SnapshotFromGLContext()` has **two branches from birth**: a non-verify branch (the pull path — what phase A's `MGB_CTX` `#else` arm effectively is) and a verify branch. P1 authors both.

### 2.9 The `SyncPersistentMappedRange` / `SyncGpuWrites` per-site ownership table

The rule the table has to encode is the stale-index discipline, `ARCHITECTURE.md:233-238` (§5.7) — verbatim:

> **陈旧索引纪律是逐站点表，不是一条笼统规则**（client 侧扫描/解析之前要做的 reconcile 必须逐字复现 monolith 的集合）：
>
> | client 侧动作 | 必须做的 reconcile |
> |---|---|
> | client 顶点数组范围计算 + 暂存 | 无（应用内存，无 GPU 写者） |
> | 最大索引扫描（EBO 源） | `SyncPersistentMappedRange()` **+** `SyncGpuWrites()` |
> | 最大索引扫描（client 指针源） | 无 |
> | `*IndirectCount` 计数解析 | **只** `SyncPersistentMappedRange()`，不加 `SyncGpuWrites()`——monolith 今天就只做这一个，加了会给 Create/Flywheel 的每 batch 平白加一次 publish-and-wait |
> | server 侧 restart 重写 / multi-draw 展平 | server 从镜像读；GPU 写者可见性由 `OnGpuWritten` 收窄集在 server 本地判定 |

And `ARCHITECTURE.md:294` (§8.1, last sentence): `> 20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 按 §5.7 逐站点归属，其中至少一处消费者搬不走：Magma 的 `ResolveUniformBufferPayload` 把具名 UBO 打进自己的 UBO ring → `SetShaderBuffers` 的 host payload（D-B8）。`

Also relevant to the same table: `ARCHITECTURE.md:239` — `> 前两条 client reconcile 的形态：publish → 等 `appliedSeq` → 排空事件 → 再碰 shadow。门：`ClientArrayAfterComputeWriteScenario`（去掉等待必须看到几何缺失）；`create-indirect` fixture 上 `roundtrips-per-frame` 必须读零（P8）。`

**The 20 + 6 sites, enumerated from this tree** (I grepped `MobileGL/MG_Backend/`; comment-only hits excluded and listed separately). The doc's counts match exactly.

`SyncPersistentMappedRange()` — 20 call sites:

| # | Site | Backend |
|---|---|---|
| 1 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:263` `drawBuffer->` | Espryt |
| 2 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4468` `indexBuffer->` | Espryt |
| 3 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4722` `drawBuffer->` | Espryt |
| 4 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4723` `parameterBuffer->` | Espryt |
| 5 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4824` `drawBuffer->` | Espryt |
| 6 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4825` `parameterBuffer->` | Espryt |
| 7 | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:1569` `bufferObject->` | Espryt |
| 8 | `MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp:510` `indexBuffer->` | Espryt |
| 9 | `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:280` `drawBuffer->` | Magma |
| 10 | `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:471` `parameterBuffer->` | Magma |
| 11 | `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:795` `indexBufferShared->` | Magma |
| 12 | `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:2023` `bufferObject->` | Magma (**the immovable consumer**: `ResolveUniformBufferPayload` / D-B8) |
| 13 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp:628` `bufferObject->` | Magma |
| 14 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp:679` `bufferObject->` | Magma |
| 15 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:3434` `indexBufferShared->` | Magma |
| 16 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:3513` `bufferObject->` | Magma |
| 17 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:3828` `sourceBufferShared->` | Magma |
| 18 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:7137` `indirectBuffer->` | Magma |
| 19 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:12132` `drawBuffer->` | Magma |
| 20 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:12133` `parameterBuffer->` | Magma |

Comment-only mentions to keep in view while writing the table (they carry the reasoning): `DirectGLES/Managers.cpp:1461` (`// A live non-zero-copy map may owe a per-draw SyncPersistentMappedRange push`), `DirectGLES/Managers.cpp:2681`, `DirectVulkan/Renderer/VulkanRenderer.h:1214` (`// SyncPersistentMappedRange is the push-down. A map taken AFTER the record …`).

`SyncGpuWrites()` — 6 call sites:

| # | Site | Backend |
|---|---|---|
| 1 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4469` `indexBuffer->` (pairs with #2 above) | Espryt |
| 2 | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:2643` `bufferObject->` | Espryt |
| 3 | `MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp:511` `indexBuffer->` (pairs with #8 above) | Espryt |
| 4 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:3433` `indexBufferShared->` (pairs with #15) | Magma |
| 5 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:3827` `sourceBufferShared->` (pairs with #17) | Magma |
| 6 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:4161` `indexBufferShared->` | Magma |

Comment-only: `DirectGLES/DirectGLES.cpp:459`, `DirectGLES/DirectGLES.cpp:509`.

Note the pattern the table has to preserve: the EBO/max-index sites pair the two calls (rows 1/3/4/5 of the `SyncGpuWrites` list sit immediately after a `SyncPersistentMappedRange` on the same object), whereas the `*IndirectCount` parameter-buffer sites (`DirectGLES.cpp:4722-4723`, `:4824-4825`, `DirectVulkan.cpp:471`, `VulkanRenderer.cpp:12132-12133`) deliberately do **not** call `SyncGpuWrites` — exactly the row `> `*IndirectCount` 计数解析 \| **只** `SyncPersistentMappedRange()`` in the §5.7 table, and the subject of open question 15 (§6 below).

### 2.10 The five-part verification gates (ARCHITECTURE §13.2, `ARCHITECTURE.md:498-506`)

Preamble, `ARCHITECTURE.md:499`: `> "改前改后 `nm --defined-only` 与 `.text` size 完全相等"的门在本方案里按构造死亡（不存在能让旧字节回来的配置）；替换是：`

**① Interface purity, three gates** (`ARCHITECTURE.md:500`) — verbatim:

> 1. **接口纯度三道门**（只跑非 verify 构建）：**A 门 include 图**——disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除（`nm --undefined-only` 对"只 include 不调用"是瞎的，而 `RenderState.h → FramebufferObject.h → TextureObject.h` 正是这种耦合），依赖 P0.5；**B 门符号**——`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**C 门未声明**——`grep -c 'pGLContext' MG_Backend/` == 0。外加 debug 断言"每个后端 memo 键都是 `{slot, gen}`，永不是裸前端指针"，由 `HandleRecycleScenario` 支撑（重键前必须在至少一个后端上是红的）。

All three turn green only in P13 (`ARCHITECTURE.md:369`). P1 must not make them *further* from green: in particular gate C greps the bare token `pGLContext`, so the `MGB_CTX` `#else` arm must be defined outside `MG_Backend/`.

**②** — the verify comparator, quoted in full in §2.5 above (`ARCHITECTURE.md:501`). **This is P1's gate.**

**③ Behavioural A/B** (`ARCHITECTURE.md:503`) — verbatim:

> 3. **行为 A/B**：40 个 trace 在 `{monolith-pull, monolith-push, split}` 下 SSIM ≥ 0.99（默认阈值）；`ctest -L integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Pipe.`/`DirectGLES.Split.`（DirectVulkan 同）之间逐名相同；单元测试全绿；CTS 逐后端 conformance 在 0.5 pp 内（行 = GL 版本/扩展，列 = 状态计数，rate = Pass/(Pass+Fail)，NS 不进分母）。`TextureUploadShapeScenario` 把逐纹理逐帧的上传形状（box vs N region、作业数）录金标比对——+6 ms 悬崖由形状相等把关，SSIM 对它完全不敏感。逐名功能基线是"P1 出口的重构后 monolith"（P1 出口先用 verify 证明等价于 `81b17c0b`）；`81b17c0b` 只作性能锚点。

**④ Monolith performance non-regression** (`ARCHITECTURE.md:504`) — verbatim:

> 4. **monolith 性能不回归**：两台设备 reboot-clean、同热窗口、配对 A/B，`tools/bench.sh` + trace replay `--benchmark` 逐帧 JSON；**指标是逐线程 CPU 时间**，p50 与 p99；**绝对阈值**——tracker 每 draw 的 ns 公布并设上限（真实拉取基线只有每 draw 6.5–9.3 次 accessor，相对噪声阈值会平凡通过）；Blaze3D blend-toggle 微基准单列；关掉 CSO 内容寻址的负面对照。

**⑤ Coverage + poison + handle discipline** (`ARCHITECTURE.md:505`) — verbatim:

> 5. **覆盖 + poison + 句柄纪律**：G6 重生成 0 UNMAPPED；`gen_pipe_dirty_surface.py` 重生成 0 未映射 mutator；逐 verb 世代 poison；G7 setter 一致性测试；`ResidualValueBlock` 的 `offsetof` 断言与 P13 的 `sizeof == 0`。

Of ⑤, P1 owns: the poison, and the `gen_pipe_dirty_surface.py` mapping-with-zero-unmapped gate (`ARCHITECTURE.md:172`, `ARCHITECTURE.md:568`, `scripts/gen_pipe_dirty_surface.py:21-22`) — see risk R2.

Two surviving byte-level equalities (`ARCHITECTURE.md:506`) — verbatim: `> `MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空且链接行不增加库；`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中。符号与 `.text` 漂移每阶段作为信息性指标发布。`

---

## 3. What P2 expects P1 to have left in place

P2 row, `ROADMAP.md:18`, deliverables verbatim:

> `MG_Impl/Pipe/Tracker`（dirty 位、5 个聚合世代、抑制器骨架）；`gen_pipe_dirty_surface.py` 首轮映射成门；`MGPipeRenderStateSpans` + G7 setter 一致性测试；`CsoCache`（64 项，键 = pipeline 子集）；`create/bind_render_state` + `set_dynamic_state`（Espryt `SyncRenderState` 一行不动；Magma `ComputePipelineStateHash`/`GetOrCreatePipeline`/`ApplyDynamicDrawStateTail` 改从 CSO 与动态 payload 取）；`set_pixel_pack_state`、`set_patch_state`、`set_vertex_attrib_defaults`；`set_residual_value_state` + `ResidualValueBlock` 绊线；**第一片 Track H**：Espryt 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`/`g_fbSlotCache`/GC）与 Magma 子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，删前端 VAO 里的后端裸指针）；`MOBILEGL_PIPE_LEGACY_MEMOS`；补 `FramebufferSrgb`/`DepthClamp` 存储

P2's acceptance gate, verbatim:

> 集成 × 2 后端 × {pull, push} 逐名相同；40 trace push 下 SSIM ≥ 0.99 双后端；verify 零分歧；`HandleRecycleScenario` 绿且重键前红；G7 测试绿且拿掉一个字段能红；两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内；Blaze3D blend-toggle 微基准；CSO 内容寻址关闭的负面对照

Concretely, P1 must not paint P2 into a corner on:

1. **A `{pull, push}` A/B arm that is real.** P2 compares integration × 2 backends × `{pull, push}` by name. That requires the `MOBILEGL_PIPE_PUSH` bitmap to genuinely select per subsystem, per field (phase B: `> 填充器按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位`, `ARCHITECTURE.md:345`). P1's accessors and fill points must be structured so a *field-granular* yield is possible, not just a whole-build switch. Note the honesty caveat P1 inherits (`ARCHITECTURE.md:367`): after phase C the bitmap stops being a true A/B, which is why P2 also introduces the compile-time `MOBILEGL_PIPE_LEGACY_MEMOS` (default ON) arm.
2. **`SyncRenderState` untouched.** `ARCHITECTURE.md:348`-region §5.3, `ARCHITECTURE.md:347`: `> **Espryt 的 `SyncRenderState`（693 行）拿到的仍是 `const RenderStateParameters&`，单 `Uint16` 早退、三段 memcmp 一行不动**`. GO/NO-GO item 2 (`ROADMAP.md:47`) restates it: `> 两个后端上都已推送的渲染状态，`SyncRenderState` 693 行一行未动`. **P1's `PipeInputs::GetRenderStateParameters()` must therefore return `const RenderStateParameters&`** — a by-value or restructured return would force a `SyncRenderState` edit and break GO/NO-GO item 2. The doc's phase-A comment says exactly this: `// 阶段 A：类型与后端今天读到的完全一致` (`ARCHITECTURE.md:328`).
3. **Server-side working `RenderStateParameters`.** `ARCHITECTURE.md:347`: `> server 侧：每 context 一份 working `RenderStateParameters`（1168 B），`bind` 与 `set_dynamic_state` 各把自己的 chunk 散射进去。` P1's `PipeInputs` is where that working copy will live in P2. Size it for that (`~20 KB`, `ARCHITECTURE.md:326`).
4. **The five aggregate generations are new frontend fields P2 adds** (`ARCHITECTURE.md:164-169`, §5.2 table): `VertexArrayState::m_anyVaoAttributeGeneration`, `FramebufferState::m_anyAttachmentGeneration`, `TextureState::m_anyTextureContentGeneration` + `m_anyTextureParamsGeneration`, `BufferState::m_anyBufferChangeGeneration` — all marked 新增, landing "on existing bump points (~20 lines)" (`ARCHITECTURE.md:170`). P1 must not restructure those mutators in ways that scatter the bump points.
5. **The dirty-surface mapping file.** P1 creates it (73 entries); P2 turns the first mapping into a gate (`ROADMAP.md:18`) — see risk R2 for the phase disagreement.
6. **Residual value block shape.** See §2.7: `GetRenderStateParameters`, `GetPixelStoreParameters`, capability bits and the three patch fields must be reachable as whole-struct values.
7. **CSO cache and content addressing.** `ARCHITECTURE.md:63` (§2.3): the client-side content-addressed CSO maps with LRU caps (render-state 64 / vertex-elements 1024 / sampler 256 / sampler-view 4096 / shader tied to `ProgramObject` lifetime). P2 also needs the negative control bit inside `MOBILEGL_PIPE_PUSH` that disables CSO content addressing (`Config.h:320-325`, `ARCHITECTURE.md:367`, `ROADMAP.md:52`), so P1's use of the bitmap must leave that bit free and meaningful.
8. **Track H's first slice needs handles to be a `PipeInputs` field type.** Phase C (`ARCHITECTURE.md:346`) turns `SharedPtr<frontend object>` fields into `MGPipeHandle` + POD descriptors. Handles already exist (`MobileGL/MG_Pipe/MGPipeHandles.h`; composite-shader band at `MGPipeHandles.h:88-90`: `kMGPipeShaderCsoSlotLimit = 1u << 20`, `kMGPipeShaderCsoCompositeSlotBase = limit - (limit >> 4)`, predicate `MGPipeIsCompositeShaderSlot` at `:92-94`). P1's object-typed accessors should be shaped so the return type can be swapped without re-touching all 167 read points a second time.
9. **The eight validate entry points** are the frame P2's tracker hangs on (`ARCHITECTURE.md:153`). P1's fill points should be those same eight shapes, generated, not hand-placed ad hoc.
10. **G7 chunk table is frozen in P2, not P1** (`ARCHITECTURE.md:161`: `带 `offsetof` 的 chunk 表与 setter 一致性测试在 P2`), and `Coverage.def:72-74` warns `GetProvokingVertexMode` is not in `ComputePipelineStateHash` today `/* recorded here so the G7 chunk table has to answer for it before it freezes */`. P1 must keep that accessor visible in the field list (it is, field #39).

---

## 4. Commit / CI discipline (通用纪律) and the GO/NO-GO items P1 delivers

### 4.1 通用纪律, `ROADMAP.md:7` — verbatim (applies to **every commit**)

> 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议；每阶段出口跑一次五部分门；每阶段性能判据是**逐线程 CPU 时间**。

Enforcement in the tree (CI job `pipe-gates`, `.github/workflows/test.yml:835-887`, deliberately independent of `build-linux` per `test.yml:838-839`):
- `test.yml:850-853` — `python3 scripts/gen_pipe.py` then `git diff --exit-code -- MobileGL/MG_Pipe/generated`.
- `test.yml:862-869` — stdio-instrumentation grep gate over `MobileGL/MG_Backend` and `MobileGL/MG_State`: `fprintf(stderr|stdout`, `printf(`, `puts(`, `std::cout|cerr`. `test.yml:857-861` says the gate `starts with no exceptions, and any addition to it needs a reason in the pull request rather than a quiet whitelist entry`.
- `test.yml:871-874` — `python3 scripts/gen_pipe_dirty_surface.py --summary`, with the comment `# Informational: … It becomes a gate in P1, when the mapping file exists to diff against.`
- `test.yml:876-887` — `check_doc_citations.py`, currently `|| true` (warning only), `# It becomes --strict when the documents settle.` **P1 edits these docs, so keep `file:line` citations correct** — the lint checks exactly the `file:line` references in `docs/Disaggregated/*.md`.
- Separate job `include-graph-check` (`test.yml:332-349`): `python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all`. `test.yml:330-331` restates the discipline in English: `The script's own --self-test is always on: a negative control that stopped tripping fails the job, because a gate that cannot go red is not a gate (ROADMAP.md:7).`

Also binding on P1 commits (from repo memory / project convention, consistent with the docs): single-line `[Type] (Scope): description` messages; no `Co-Authored-By`.

CTS turnaround (`ROADMAP.md:34`) — verbatim: `> **CTS 周转单独计价**：`gl44to46` 约 56,271 例。逐阶段只跑该阶段可能影响的具名块（P4a `packed_pixels`、P3b/P4b `texture_*`/`shader_image_*`、P9 `transform_feedback*`）；完整 caselist 只在五个架构边界（P0.5、P3a、P4a、P3b/P4b、P13）与每次合并 `dev` 之前跑，放 CI 不放关键路径。若周转仍主导排期，加宽估时而不是削弱门。` — **P1 is not one of the five full-caselist boundaries**, but a merge to `dev` still triggers a full run.

### 4.2 The day-43 GO/NO-GO list, `ROADMAP.md:44-52` — verbatim, P1's contribution marked

> 手上必须有：
>
> - [ ] P1 交付的逐 draw 逐字段语义等价证明（40 trace + 全部集成测试零分歧）   ← **P1 delivers this one**
> - [ ] 两个后端上都已推送的渲染状态，`SyncRenderState` 693 行一行未动
> - [ ] 两片 Track H 的实测单位成本（Espryt 0b、Magma 子系统 4）
> - [ ] 两台设备（Adreno 830 `35d0befa`、Mali `3B159D009VZ00000`）reboot-clean 配对的逐线程 CPU 时间增量，p50 与 p99
> - [ ] tracker 每 draw 的**绝对 ns**（上限从设备基线定：稳态每 draw 6.5–9.3 次 accessor + memo 探测，见 `MEASUREMENTS.md`）
> - [ ] Blaze3D blend-toggle 微基准（enable/draw/disable/draw，MC batch 速率）
> - [ ] 负面对照：关掉 CSO 内容寻址重跑，把"推送更慢"与"CSO 设计更慢"分开

Verdict criteria, `ROADMAP.md:56` — verbatim: `> - **继续**：两台设备 p50 与 p99 逐线程 CPU 增量都不为负；tracker 绝对 ns 在上限内；Track H 单位成本不超出估计的 50%。按两条跑道推进。`

**The per-draw per-field semantic equivalence proof** = gate 13.2-② run to zero divergence over 40 traces + the whole integration suite, with both negative controls (corrupt a snapshot field → red; drop a field from `glGenerateMipmap`'s fill table → poison Fatal on that verb). It is P1's single most important artifact and it is the first line of the GO/NO-GO list.

The absolute-ns ceiling P1's work is later measured against comes from `MEASUREMENTS.md:50-60` (the four-trace table) and `MEASUREMENTS.md:64`: `> **真机稳态动态 accessor 成本是每 draw 6.5–9.3 次**（预测区间 10–25 的下沿；llvmpipe 的 15.5/20.7 是 memo 冷的）。推送要打败的是 ~8 次 accessor + memo 探测，不是 124/169 的静态调用点数。GO/NO-GO 的 tracker 绝对 ns 上限从这里定。` The measured per-draw accessor counts: Espryt 9.28 / Magma 8.56 on `minecraft-1.21.4-in-world` (`MEASUREMENTS.md:52-53`); Espryt 8.44 / Magma 6.53 on `improved-transparency-minecraft-26.3` (`MEASUREMENTS.md:56-57`). `MEASUREMENTS.md:60` warns `accessor-calls` is a **lower bound** (static counting at ~10 hot entry points; site list is a contract at `MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`).

---

## 5. Open questions and caveats that touch P1

From `ROADMAP.md:70-90` ("仍然开放的问题"), the ones that bear on P1:

- **#1** (`ROADMAP.md:74`) — `> **client 侧 dirty 走查的真实每 draw CPU 代价。** 拉取基线已实测为每 draw 6.5–9.3 次 accessor + memo 探测；推送要在这个数字下净减少。P2 的头号数字，逐线程 CPU + 绝对 ns，两台设备。` P1's accessor design determines whether this is measurable at all; P1 should keep the accessor call path cheap enough that the P2 number is not dominated by P1 scaffolding.
- **#5** (`ROADMAP.md:78`) — `> **`FramebufferSrgb` / `DepthClamp` 的拍板。** 事实已清（无存储、`glEnable` 静默吞掉、六个读点恒 false、41 个 fixture 无一开启）；建议在 chunk 表冻结前补真存储并把 `FramebufferSrgb` 划进 pipeline 半边。由计划所有者拍板，**拍板前不冻结 chunk 表**。` The six `FramebufferSrgb` read points are inside `MG_Backend` and are read points P1's `sed` will touch; today they consume a compile-time `false` (`MEASUREMENTS.md:84`). P1 must alias them without silently changing them, and must not freeze the chunk table.
- **#7** (`ROADMAP.md:80`) — `> **`MG_Util` 的切割缝。** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、格式处理器、POST 探针；client 需要 glslang phase A/B 与反射层。P0.5 解决了 `ProgramObject.h` 一处，`MG_Util` 内部是否有干净的 Transpile-vs-Reflect 缝未审计。` Unaudited; may surface as an unexpected include when P1 touches backend TUs.
- **#14** (`ROADMAP.md:87`) — `> **推送模型改变哪些按拉取模式调过的缓存命中率。** 幸存者容量在 P13 重调。`
- **#15** (`ROADMAP.md:88`) — `> **monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是潜在缺口**（compute 写的 indirect buffer）。独立 `dev` 问题，拆分不得借机顺手修。` **Directly aimed at P1's per-site ownership table**: the `*IndirectCount` rows must record today's behaviour verbatim, bug included. Do not "fix" it in P1.
- **#17** (`ROADMAP.md:89-90`) — `> **create-indirect fixture 在 Adreno 830 上的失败**是 `dev@81b17c0b` 就有的（基线 APK 复现），不是本分支造成；它是 P3a/P8 验收清单里的用例，需要先在 `dev` 上修。` P1's "40 trace" gate inherits this: `MEASUREMENTS.md:96` — `minecraft-1.21.1-neoforge-create-indirect-in-world` fails on **both** devices (Adreno: Espryt black frame after ~4.5 min, Magma `VK_ERROR_DEVICE_LOST` on texture upload submit; Mali: SSIM 0.85 / 0.45). Expect it to be excluded or waived when running the 40-trace verify sweep.

Other caveats in the docs that touch P1:

- `ARCHITECTURE.md:3` — `> 落地状态以 `feat/disaggregated@458ccde1` 为准` while `README.md:3` says P0.5 landed at `5d99ee43`; `ROADMAP.md:3` still says `P0 已落地（feat/disaggregated@458ccde1）`. The docs' own commit anchors are inconsistent; the ROADMAP table cells are the current truth.
- `ARCHITECTURE.md:172` measured surface: `> 926 次 mutator 调用落在 73 个不同 mutator 上` vs `MEASUREMENTS.md:81` which says `73 个不同 mutator（`RecordError` 一项就占 836 次）` and `92 次（36 个即时发布点、7 个 mutator …）`. `ARCHITECTURE.md:172` says 92 calls / 7 mutators without the 36. Consistent enough; use `MEASUREMENTS.md:81` for the full shape. Scanner limits are stated at `scripts/gen_pipe_dirty_surface.py:196-198`: it matches braced bodies textually, a mutator inside a lambda is attributed to the enclosing function, and a mutation published through a helper reads as deferred.
- `ARCHITECTURE.md:33` (§1.3) — the `Init.cpp` hook branch lands in P5, **not** P1: `> （分支在 P5 落地；P0 的 `Init.cpp` 尚未含它。）`
- `MobileGL/MG_Pipe/Coverage.def:16-17` says UNMAPPED rows are `allowed in P0 and merely counted, ZERO from P5 onward`, while gate 13.2-⑤ (`ARCHITECTURE.md:505`) says `G6 重生成 0 UNMAPPED` as a standing gate and `MEASUREMENTS.md:82` reports **0 UNMAPPED already**. No action for P1 beyond keeping it at zero when `GetBufferBindingSlot` is split by target (`Coverage.def:36-40`).
- `MEASUREMENTS.md:92-95` harness traps that will bite a P1 device run: the trace app never reaches `MobileGL::DestroyImpl` so `MOBILEGL_PIPE_STATS_FILE` JSON never lands on device (only the periodic `MGPipe stats:` line in `mobilegl.log`); `run_android_retrace_local.py` `rmtree`s one shared result root per tree, so two devices must run **serially** from one tree; `--env` values containing `/data/...` need `MSYS_NO_PATHCONV=1`.

---

## 6. Risks / discrepancies I found (read these before starting)

**R1 — the "293 sites" figure does not match this tree.** `ROADMAP.md:17` and `ARCHITECTURE.md:344` both say `sed` 293 sites. Measured here: `grep -rno "MG_State::pGLContext->" MobileGL/MG_Backend/` → **277** occurrences (274 distinct lines), distributed: `DirectGLES/DirectGLES.cpp` 91, `DirectGLES/Managers.cpp` 15, `DirectGLES/MultiDraw.cpp` 5, `DirectGLES/Utils.cpp` 2, `DirectVulkan/BackendObject_DirectVulkan.cpp` 2, `DirectVulkan/DirectVulkan.cpp` 12, `DirectVulkan/Renderer/UniformManager.cpp` 14, `DirectVulkan/Renderer/VkClearManager.cpp` 1, `DirectVulkan/Renderer/VkRenderPassManager.cpp` 3, `DirectVulkan/Renderer/VkTextureManager.cpp` 2, `DirectVulkan/Renderer/VulkanRenderer.cpp` 127. The **58 non-arrow lines matches exactly** (`DirectGLES/DirectGLES.cpp` 1, `DirectGLES/Managers.cpp` 8, `DirectVulkan/BackendObject_DirectVulkan.cpp` 2, `DirectVulkan/DirectVulkan.cpp` 35, `DirectVulkan/Renderer/UniformManager.cpp` 9, `DirectVulkan/Renderer/VertexInputStateFactory.h` 1, `DirectVulkan/Renderer/VulkanRenderer.cpp` 2). Likely the 293 was counted at a different commit or with a different spelling. **Re-count and update `ROADMAP.md:17` / `ARCHITECTURE.md:344` as part of P1**, since these are cited numbers under the doc-citation lint.

**R2 — the dirty-surface mapping gate is assigned to two different phases.** `ARCHITECTURE.md:172` (`P1 起成为门`), `ARCHITECTURE.md:568` (`信息性，P1 成门`), `scripts/gen_pipe_dirty_surface.py:21-22` and `.github/workflows/test.yml:872-873` all say **P1**. `ROADMAP.md:18` puts `gen_pipe_dirty_surface.py 首轮映射成门` in the **P2** deliverables and the P1 row does not mention it. Resolve before planning P1's day budget — 73 mapping entries is real work and the P1 row's 10–13 days does not obviously include it.

**R3 — `MOBILEGL_PIPE_PUSH` is simultaneously a preprocessor condition and a runtime `Uint64` bitmask.** `ARCHITECTURE.md:334` uses `#if MOBILEGL_PIPE_PUSH` to switch `MGB_CTX`; `MobileGL/Config.h:326` defines `Uint64 PipePush = 0;` and `MobileGL/ConfigLoader.cpp:245` reads it from the environment; `ARCHITECTURE.md:345` says the fillers yield per field **according to the bitmap**. These cannot both be the same symbol. P1 must pick the spelling (e.g. a CMake `MOBILEGL_PIPE_PUSH` selecting the alias arm, with `MG_Config::Features.PipePush` selecting subsystems at run time) and write it down; leaving it ambiguous is how P2's `{pull, push}` by-name A/B silently degenerates into a single arm.

**R4 — `MGB_CTX`'s `#else` arm spells `pGLContext`.** Purity gate C greps the bare token under `MG_Backend/` (`ARCHITECTURE.md:502`, and `ARCHITECTURE.md:349`'s last sentence says so explicitly). Define the macro outside `MG_Backend/` (e.g. in `MG_Pipe/`) so the pull arm's spelling does not live where the gate looks. Gate C is not required green until P13, but placing it wrong makes P13 a second migration.

**R5 — sticky fields are unaudited.** All 61 entries of `kMGPipeInputFieldSticky[]` are `false` today (`PipeFilled.inc:163-225`). Any `true` P1 introduces is a field the poison stops protecting (`PipeFilled.inc:160-162`). Candidates that will *look* sticky and probably are not: `GetTextureContextId`, `GetTextureBindGeneration`, `GetSamplingResolutionGeneration`, `GetTransformFeedbackGeneration`, `GetPipelineStateVersion`, `GetRenderStateParametersVersion`. Each `true` needs a written argument in the commit.

**R6 — `MGPipeTypes.h` still includes `MG_Backend/BackendObject.h`.** `MobileGL/MG_Pipe/MGPipeTypes.h:25-34` documents this as P0.5 debt: `MGPCaps` embeds `DynamicBackendParameters`, so purity gate A asserts `MGPipeValueTypes.h` instead of `MGPipeTypes.h`, and `// The caps block needs fixed-width members before it can move (a type change, not a move): P1/P7.` (`MGPipeTypes.h:32`). P1 may be expected to do the fixed-width conversion; it is **not** in the ROADMAP P1 row. Confirm scope with the plan owner rather than discovering it when gate A is re-tightened.

**R7 — the four memcmp-fallback types.** `scripts/gen_pipe.py:167-172` (`MEMCMP_FALLBACK_TYPES = {RenderStateParameters, PixelStoreParameters, DynamicBackendParameters, MGHostSpan}`) and `PipeVerify.inc:232-237` both say P1 gives them field lists. Not listed in the ROADMAP P1 row either, but it is a *precondition of "zero false positives"* for the P1 verify gate — `RenderStateParameters` is precisely the struct documented to false-DIFFER under memcmp (`PipeFields.def:11-13`). Treat it as part of deliverable 5.

**R8 — verify's cost.** 5–10× slowdown over 40 traces + the whole integration suite, on two contended devices, with the serial-run constraint from `MEASUREMENTS.md:93`. Budget device time; the P1 row's 10–13 days does not price CTS turnaround (`ROADMAP.md:34`).

---

## 7. Quick reference: files P1 touches or creates

| Path | Status | Role in P1 |
|---|---|---|
| `MobileGL/MG_Backend/MGPipe/PipeInputs.h` | **create** | The 61-field struct, Espryt 32 / Magma 55 accessors, poison state (`ARCHITECTURE.md:325-340`, `:553`) |
| `MobileGL/MG_Backend/MGPipe/MGPipeImpl_DirectGLES.cpp` | **create** | `ARCHITECTURE.md:553` |
| `MobileGL/MG_Backend/MGPipe/MGPipeImpl_DirectVulkan.cpp` | **create** | `ARCHITECTURE.md:553` |
| `MobileGL/MG_Backend/DirectGLES/*`, `DirectVulkan/*` | edit | 277 arrow sites + 58 non-arrow lines (R1) |
| `MobileGL/MG_Impl/GLImpl/**` | edit | ~93 (measured 91-line) boundary sites get generated validate/fill calls |
| `MobileGL/MG_Pipe/PipeFields.def` | edit | field lists for the four memcmp-fallback types (R7) |
| `MobileGL/MG_Pipe/Coverage.def` | edit | split `GetBufferBindingSlot` by `BufferTarget` (`Coverage.def:36-40`) |
| `MobileGL/MG_Pipe/generated/*.inc` | regenerate + commit | CI diffs them (`test.yml:850-853`) |
| `scripts/gen_pipe.py` | edit | G5 fill-table emission, drop fallback types |
| `scripts/gen_pipe_dirty_surface.py` + new mapping file | edit / create | 73-entry mapping; gate (R2) |
| `.github/workflows/test.yml` | edit | third CI mode (`MOBILEGL_PIPE_VERIFY`); promote dirty-surface to a gate |
| `CMakeLists.txt` | edit | `MOBILEGL_PIPE_VERIFY` build option (`ARCHITECTURE.md:583`) |
| `MobileGL/Config.h` / `ConfigLoader.cpp` | already landed | runtime `PipePush` / `PipeVerify` (`Config.h:320-332`, `ConfigLoader.cpp:245-246`) |
| `docs/Disaggregated/*.md` | edit | keep `file:line` citations valid (`check_doc_citations.py`, `test.yml:876-887`) |
