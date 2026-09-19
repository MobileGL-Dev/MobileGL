# P4a docs specification — everything `docs/Disaggregated/` says about handle wave 2

Scout report. Tree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`
(branch `feat/disaggregated`, P3a landed at `fde5fda3`, docs at `37da3c3a`). Read-only; nothing edited.
Every claim below carries the path and line I opened. Chinese is quoted **verbatim** wherever the wording is
the requirement; English around it is my reading.

Conventions carried forward from P3a: `ID-n` are the rulings in `~/w7/notes/p3a/INTEGRATOR-DECISIONS.md`;
`Gn` are the gate ids of `~/w7/notes/p3a/BRIEF-P3A.md` §A.2. Paths written `MobileGL/...` are relative to the
repo root; `ARCHITECTURE.md`, `ROADMAP.md`, `MEASUREMENTS.md`, `README.md` are in `docs/Disaggregated/`.

---

## 0. Where P4a sits

- `README.md:3`: "**P0、P0.5、P1、P2、P3a 已落地**（`feat/disaggregated@fde5fda3`，基线 `dev@9eae9858`）。第 43 天
  GO/NO-GO 判定为**继续**。P3a（handle wave 1：Espryt 的 buffer 与 VAO）已交付，**下一步 P4a**（handle wave 2：
  FBO / 纹理 / sampler / program 的身份与描述符）。"
- `ROADMAP.md:9` — the two tracks. The monolith track is
  "**monolith 跑道** P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止且
  monolith 严格好于起点". P4a's dependency is P3a; P5 (transport) depends on P4a (`ROADMAP.md:21`), and so does
  P3b/P4b (`ROADMAP.md:23`).
- `ROADMAP.md:32` — the running day total: "P3a 61 → **P4a 87** → P5 99 …". P4a's own estimate is **26–34 days**
  (`ROADMAP.md:20`, column 2).
- `ID-24` (P3a closed): "Next: P4a (handle wave 2: FBO / texture / sampler / program)."

---

## 1. The P4a row of the phase table, cell by cell

`ROADMAP.md:20` in full. **All five cells verbatim** (this table row is the contract; nothing below adds a
deliverable to it):

> | **P4a** handle wave 2（Espryt）：FBO / 纹理 / sampler / program 身份与描述符 | 26–34 | `set_framebuffer_state`（解析后的 `ReadSurface`、内联格式、`ContentHash`、`{0,1}`）；sampler CSO（含 `borderColorForm`）；sampler view + `set_texture_params`；`set_sampler_views`/`bind_sampler_states`/`set_shader_images`；shader CSO（SPIR-V + 归档）；`set_draw/dispatch_program`；`set_global_constants`；`CompositeResolver`；纹理/renderbuffer 的 `resource_*`。emulation 在 split 下显式 Fatal 直到 P8 | 全套门；framebuffer/纹理/program 族场景；**新增"只作 attachment / image 单元 / CopyImage 端点的纹理其 `glTexParameter` 生效"场景（落地前必须红）**；两台设备 `KHR-GL46.direct_state_access.framebuffers*` 与整个 `packed_pixels` 块（~3300 例，句柄复用压力测试）。**再基线检查点 1b：超过 39 天** | P3a |

### 1.1 Deliverables, decoded (column 3)

Eleven items. Each maps onto a call that **already exists in the catalogue** — `PipeCalls.def` is closed
(`MGP_CALL_LIST_DOCUMENTED_COUNT 71`, `MobileGL/MG_Pipe/PipeCalls.def:75`) and
`MobileGL/MG_Pipe/PipeCalls.def:26-29` forbids reordering: "RECORD NUMBERING NEVER CHURNS … The wire opcode is
the 1-based position in this list, so reordering is a protocol break." P4a therefore **wires** calls; it does
not add rows. (P3a's precedent: it changed one flag word on an existing row, `ID-8`/BRIEF C.0.)

| # | deliverable | catalogue row | payload |
|---|---|---|---|
| 1 | `set_framebuffer_state` | `PipeCalls.def:113` `X(SetFramebufferState, MGPFramebufferState, kCtxState, kNone)` | `MGPFramebufferState` 304 B |
| 2 | sampler CSO incl. `borderColorForm` | `:104` `CreateSamplerState`, `:105` `DeleteSamplerState` (`kCtxCso, kNone`) | `MGPSamplerDesc` 32 B |
| 3 | sampler view | `:106` `CreateSamplerView`, `:107` `DeleteSamplerView` | `MGPSamplerView` 36 B |
| 4 | `set_texture_params` | `:133` `X(SetTextureParams, MGPTextureParams, kCtxObject, kNone)` | `MGPTextureParams` 32 B |
| 5 | `set_sampler_views` | `:117` `kCtxState, kVarTail` | `MGPSamplerViews` 16 B + `MGPBoundView[]` 24 B |
| 6 | `bind_sampler_states` | `:118` `kCtxState, kVarTail` | `MGPSamplerStates` 16 B + `MGPipeHandle[]` |
| 7 | `set_shader_images` | `:119` `kCtxState, kVarTail` | `MGPShaderImages` 16 B + `MGPImageView[]` 24 B |
| 8 | shader CSO (SPIR-V + archive) | `:108` `CreateShaderState` (`kHasBlob`), `:109` `BindShaderState`, `:110` `DeleteShaderState` | `MGPProgramDesc` 192 B |
| 9 | `set_draw/dispatch_program` | `:126` `SetDrawProgram`, `:127` `SetDispatchProgram`, both `MGPHandleOnly` | 16 B |
| 10 | `set_global_constants` | `:122` `X(SetGlobalConstants, MGPGlobalConstants, kCtxState, kHasBlob)` | `MGPGlobalConstants` 40 B |
| 11 | texture/renderbuffer `resource_*` | `:81-85` `ResourceCreate/Respecify/Destroy/MapPersistent/UnmapPersistent` (kScreen) + `:134-141` the `kCtxObject` transfers | `MGPResourceDesc` 88 B, `MGPSubData` 72 B, `MGPMipPlan` 16 B, `MGPCopyRegion` 64 B, `MGPReadbackInfo` 64 B |

`CompositeResolver` is not a call: it is a **client-side component** in the build layout,
`ARCHITECTURE.md:562` — `MobileGL/MG_Impl/Pipe/  Tracker、SlotAllocator、CsoCache、HostResolve、CompositeResolver  [P2+]`.
It does not exist in the tree today (`grep -rn CompositeResolver` hits only `ARCHITECTURE.md:562` and
`ROADMAP.md:20`); `MobileGL/MG_Impl/Pipe/` currently holds `CsoCache.h`, `PipeFill.{h,cpp}`, `ResourceTracker.h`,
`SetHashSuppressor.h`, `SlotAllocator.{h,cpp}`, `Tracker.h`, `VertexInputEmit.h`. Its contract is §5.6 below.

**"emulation 在 split 下显式 Fatal 直到 P8"** — the last clause of column 3. The emulations P4a's families
reach (client vertex arrays, max-index scan, `*IndirectCount`, CopyImage mirror, generate-mipmap storage) are
P8's work (`ROADMAP.md:25`: `MG_Impl/Pipe/HostResolve.cpp`, `Server/IndexHostMirror`, "CopyImage 镜像搬到
client", "`generate_mipmap` 计划 + CPU 回退纹素"). Until then, a split build that reaches one must **abort
loudly, not silently degrade**. In monolith the same code paths keep running as today (the Fatal is a
split-only arm), so this costs P4a a named `Fatal{...}` per unmigrated emulation site and nothing else.

### 1.2 The acceptance gate (column 4), decoded

Four clauses. Read literally.

**(a) 全套门** — the five-part gate of `ARCHITECTURE.md:507-517` (§13.2), run once at the phase exit
(`ROADMAP.md:7`: "每阶段出口跑一次五部分门"). Concretely, P3a's realisation of it is `MEASUREMENTS.md:328-381`
(§16) and the G-ids of `BRIEF-P3A.md:155-172`, all of which carry over:

| part | what | P4a shape |
|---|---|---|
| 1 interface purity | three builds pull/push/verify green; **A** include-closure; **B** `nm --undefined-only libMobileGLServer.so \| grep -E 'MG_State::GLState::\|glslang'` empty; **C** `grep -c pGLContext MG_Backend/ == 0`; plus **G1** pull symbols 0/0/0/0 and **G5** byte-identical regions | `ARCHITECTURE.md:511`; P3a's realisation `MEASUREMENTS.md:330-341`. G13 additionally: "no `MG_State::GLState` type appears in any `MGPipeResourceOps` signature" (`BRIEF-P3A.md:170`) — P4a's new op tables inherit that rule |
| 2 semantic shadow compare | `MOBILEGL_PIPE_VERIFY=1`: `integration-verify` zero `Fatal{`, every retrace case **armed**, zero divergence | `ARCHITECTURE.md:512`; P3a: 842/842 and 79/79 armed (`MEASUREMENTS.md:347-348`). §512 names the **保留模式** (retain mode) that P4a's texture path needs: "消费即清的组（纹理 dirty rect）发射后无法重算，verify 时 tracker 保留清除前的集合并比对发射出去的 `(UnionBox, RegionCount, Regions[])`" |
| 3 behaviour A/B | **G2** pull/push ctest names identical; **G14** 0 removed; `integration-gpu` green on {pull, push, `PUSH=0`, the previous phase's mask, named kill-switch arms}; 79 retrace cases at their own SSIM thresholds; **CTS within 0.5 pp** | `ARCHITECTURE.md:513`, `MEASUREMENTS.md:352-367`. The subsystem-off arm for P4a is `MOBILEGL_PIPE_PUSH=0x1ff` (P3a's default), exactly as P3a used `0x7f` (**G12**, `MEASUREMENTS.md:359`) |
| 4 monolith performance | **RECORDED, NOT GATED** — see §4 below | `ARCHITECTURE.md:514`, `MEASUREMENTS.md:324`, `ID-6` |
| 5 coverage + poison + handle discipline | G6 `gen_pipe.py --check`/`--self-test` 0 UNMAPPED; `gen_pipe_dirty_surface.py --check`/`--self-test`; per-verb poison; G7 setter-consistency; `ResidualValueBlock` `offsetof` asserts | `ARCHITECTURE.md:515`, `MEASUREMENTS.md:373-379` |

**(b) framebuffer / 纹理 / program 族场景** — the scenario families. The tree's existing homes:
`MobileGL/MG_IntegrationTest/Scenarios/` (P3a's buffer/VAO family list is enumerated at `MEASUREMENTS.md:361`
and is the template for naming the P4a family list in the brief).

**(c) THE NEW SCENARIO, and its red-before requirement.** Verbatim:

> **新增"只作 attachment / image 单元 / CopyImage 端点的纹理其 `glTexParameter` 生效"场景（落地前必须红）**

This is the acceptance case for the D10 split between `set_texture_params` and the sampler view. The design
statement it tests is `ARCHITECTURE.md:100`:

> `SetTextureParams` 按资源寻址、与 sampler view 分开（D10）：只作 FBO attachment / image 单元 /
> `glCopyImageSubData` 端点的纹理没有 sampler view，但 Espryt 对 attachment 也同步纹理参数，且
> `RequireImageBindableStorage` 需要在前端参数版本不动时强制重同步。

and the payload comment `MobileGL/MG_Pipe/MGPipeTypes.h:289-292`:

> "= pipe_sampler_view, and ONLY the view restrictions. Everything a `glTexParameter` writes lives on
> `set_texture_params` instead, because a texture that is only an FBO attachment, only an image binding or only
> a `glCopyImageSubData` endpoint has no sampler view to hang it on (section 4.4.3)."

"落地前必须红" is the same discipline as `ROADMAP.md:7`'s "**每个门必须能因它存在的理由变红**" and the same
shape P3a discharged for the buffer class at `MEASUREMENTS.md:383-399` (§17: the `.Handles` arm **visibly
SKIPs** with a stated reason on the contract tree, `.Legacy` passes, `.AbaControl` passes *by asserting the
corruption*; "**`.Handles` 可见地 skip** 并写明缺什么，而不是一条消失的测试"). Record the red-before evidence on
the contract tree before the consumer package lands (`BRIEF-P3A.md:1420-1431` D.2 is the procedure).

**(d) DEVICE CTS, both devices.** Verbatim: "两台设备 `KHR-GL46.direct_state_access.framebuffers*` 与整个
`packed_pixels` 块（~3300 例，句柄复用压力测试）". Two named blocks, and the second is explicitly a
**handle-reuse stress test** — that is why the block runs whole rather than sampled. Supporting rules:

- `ROADMAP.md:34`: "**CTS 周转单独计价**：`gl44to46` 约 56,271 例。逐阶段只跑该阶段可能影响的具名块
  （**P4a `packed_pixels`**、P3b/P4b `texture_*`/`shader_image_*`、P9 `transform_feedback*`）；完整 caselist 只在
  五个架构边界（P0.5、P3a、**P4a**、P3b/P4b、P13）与每次合并 `dev` 之前跑，放 CI 不放关键路径。若周转仍主导排期，
  加宽估时而不是削弱门。" — so P4a owes **both** the named blocks *and* a full ~56,271-case run, the latter off
  the critical path at the `dev` merge (P3a's handling: `MEASUREMENTS.md:381`, **G15**).
- Report shape is fixed (`ARCHITECTURE.md:513`, `BRIEF-P3A.md:172`): rows = GL version/extension, columns =
  status counts, rate = Pass/(Pass+Fail), **NS not in the denominator**; per-backend conformance within
  **0.5 pp** of the `$BASE` reading. The two devices are Adreno 830 `35d0befa` and Mali `3B159D009VZ00000`
  (`ROADMAP.md:50`).
- P3a also carried a caselist rider that lands in the same run: `MEASUREMENTS.md:381` — "`MOBILEGL_PIPE_POISON_OMIT`
  的清扫（§14 那 8 条静态过近似填充行）搭同一次 caselist 运行". Those 8 over-approximated `FillPoints.def` rows
  (`MEASUREMENTS.md:294`) are still open and three of them are P4a's own subject matter (`kTextureOp`/`kDispatch`
  + `IsCapabilityEnabled`, `kBlitOrCopy`/`kTextureOp` + the shader-blit viewport/vertex bindings).

**(e) 再基线检查点 1b：超过 39 天.** `ROADMAP.md:67` — the trigger table row "| P4a > 39 天 | 同上 |", where
"同上" = `ROADMAP.md:66`'s action, "'窄句柄化'的前提错了，P4a 开始前重定基线" (for 1b: re-baseline before the
next phase). `ROADMAP.md:70`: "任一触发，先跑 `inproc` 的证伪数字再决定是否继续." Precedent for the accounting:
P3a's checkpoint 1 was 27 days and came in at **1** actual calendar day (`ROADMAP.md:40`, `MEASUREMENTS.md:417`),
recorded per package. P4a must record elapsed days per package the same way.

### 1.3 Every other mention of P4a in the docs

1. `README.md:3` — next phase, named as "handle wave 2：FBO / 纹理 / sampler / program 的身份与描述符".
2. `ROADMAP.md:9` — position in the monolith track.
3. `ROADMAP.md:21` — **P5 depends on P4a**. P5's column-5 cell is literally `P4a`.
4. `ROADMAP.md:23` — **P3b/P4b depends on P4a** (column 5 = `P4a`).
5. `ROADMAP.md:32` — cumulative day count, P4a ends at day 87.
6. `ROADMAP.md:34` — the CTS rule above (`packed_pixels` named block; P4a one of the five full-caselist boundaries).
7. `ROADMAP.md:67` — re-baseline checkpoint 1b.
8. `ARCHITECTURE.md:369` (§9.6) — "对策：**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）在 **P3a/P4a 期间**
   保留 registry / `TwinLookupMemo` 实现活在同一个 `PipeInputs` 接口之下，随 pull 路径在 P13 退役（各阶段 +1 天维护）."
   → **P4a keeps the pre-handle arm compiling**, and budgets +1 day for maintaining it.
9. `MEASUREMENTS.md:465` reading (5) — "`CreateVertexElements` 每帧字节数：统计行没有这一类（`vtxc` 是 client
   数组），**未测**，**留给 P4a 给汇总行加类**." → P4a owes a new byte class on the `MGPipe stats:` summary line.
10. `MEASUREMENTS.md:503` (DriverBench) — "**不钉上限、也不强制上限**（规则 (a)）；数字发布出来，**让 P4a 自己
    决定要不要钉**." → P4a decides whether P3a's per-draw cost gets a ceiling. See §4.
11. `MEASUREMENTS.md:326` (§15) — "一次回归要写下数字与怀疑的成因并**带进 P4a 的排期**."
12. `MobileGL/MG_Impl/Pipe/SetHashSuppressor.h:45-52` — the suppressor slot enum labels
    `SetSamplerViews`/`BindSamplerStates` as **P3b** and `SetShaderImages`/`SetShaderBuffers`/
    `SetStreamOutputTargets` as **P4b**, while `ROADMAP.md:20` gives the three *calls* to P4a. **This is not a
    contradiction but it is a trap**: P4a lands the calls and their `ContentHash` fields; the *suppressor
    wiring* for those slots is P3b/P4b's ("**同时**在 Tracker 落地集合 hash 抑制器", `ROADMAP.md:23`). A P4a
    package that wires a suppressor slot is doing P3b/P4b's work; one that omits `ContentHash` from the record
    breaks `ARCHITECTURE.md:147`. Say which in the brief.

---

## 2. What ARCHITECTURE.md says about these calls, payloads and paths

### 2.1 The exact structs and `static_assert`s (`MobileGL/MG_Pipe/MGPipeTypes.h`)

Every payload is "平坦 POD、显式 padding、`static_assert` 平凡可复制与**精确尺寸**；**永不含指针**"
(`ARCHITECTURE.md:118`). `MGP_ASSERT_POD(T, Size)` is defined at `MGPipeTypes.h:44`. Sizes are identical on
arm64 and x86-64 (`MEASUREMENTS.md:86`).

**`MGPSurface` — 24 B, `MGPipeTypes.h:343-353`** (`= pipe_surface`; `:341-342`: "internalFormat is INLINE so the
four cross-object masks fall out at push time with no lookup"):

```cpp
struct MGPSurface {
    MGPipeHandle Res;
    Uint32 InternalFormat;
    Uint8 Kind;      // Texture | Renderbuffer | None
    Uint8 Layered;
    Uint16 Level;
    Uint32 Layer;
    Uint16 UploadTarget;
    Uint16 Pad0;
};
MGP_ASSERT_POD(MGPSurface, 24);
```

**`MGPFramebufferState` — 304 B, `MGPipeTypes.h:355-372`**:

```cpp
struct MGPFramebufferState {
    MGPipeHandle Fbo;            // kMGPipeDefaultFramebuffer for the default framebuffer
    MGPSurface Color[8];
    MGPSurface Depth, Stencil;
    MGPSurface ReadSurface;      // the RESOLVED read surface, not an index
    Int8 DrawBuffers[8];         // attachment index, -1 = NONE
    Uint16 Width, Height, Layers, Samples;
    Uint8 FixedSampleLocations, IsDefault, Complete, Pad0;
    Uint32 Pad1;
    Uint64 ContentHash;
};
MGP_ASSERT_POD(MGPFramebufferState, 304);
```

The three doc statements this row carries, all of which the P4a row's parenthetical
（解析后的 `ReadSurface`、内联格式、`ContentHash`、`{0,1}`）names one-for-one:

- **Resolved ReadSurface.** `ARCHITECTURE.md:136`: "**client 解析后的 `ReadSurface`**（按结构消灭
  read-buffer-shared-FBO 缺陷类）". Header, `MGPipeTypes.h:359-360`: "The RESOLVED read surface, not an index.
  This is what structurally closes the read-buffer-shared-FBO defect class." (That defect class is the one my
  memory file `read-buffer-shared-FBO` records as landed on `dev@bc2d698b`; P4a makes it unrepresentable.)
- **Inline formats.** `ARCHITECTURE.md:136`: "`MGPSurface::InternalFormat` 内联（四个跨对象 mask 推送时零查表）".
- **ContentHash, two jobs.** `ARCHITECTURE.md:136`: "`ContentHash` 既是 server 的 render-pass memo 键也是
  client 的发射抑制器". Header, `MGPipeTypes.h:366-369`: "Two jobs …: the server's render-pass memo key, and the
  CLIENT's emission suppressor — an unchanged hash means this record is not sent at all. The same pattern is
  mandatory for every kVarTail set_* below, or 26.2's redundant glBindSampler traffic reappears as a
  variable-length record per batch."
- **`{0,1}`.** `ARCHITECTURE.md:39`: "保留句柄：`{0,0}` = null；**`{0,1}` of `Framebuffer` = 默认帧缓冲**（退役
  Espryt 四处 `pDefaultFramebufferInfo->defaultFBO` 身份比较）". In the tree:
  `MobileGL/MG_Pipe/MGPipeHandles.h:70-71` `kMGPipeNullHandle{0,0}` / `kMGPipeDefaultFramebuffer{0,1}`, with the
  comment at `:66-69` naming the four identity comparisons that retire. Related: the default framebuffer's
  geometry stops being written by the swapchain — `ARCHITECTURE.md:281` (`OnSurfaceChanged`) and
  `ARCHITECTURE.md:574`: "`pDefaultFramebufferInfo` 由保留句柄 `{0,1}` + `OnSurfaceChanged` 取代" — which is
  one of the four process globals `inproc` needs gone.

**`MGPSamplerDesc` — 32 B, `MGPipeTypes.h:283-287`**, with the `borderColorForm` requirement stated twice.
`ARCHITECTURE.md:133`: "`SamplerParameters` 逐字节过线**含 `borderColorForm`**（三种 border color 表示永远都被
数值填满，没有它后端无法在 `Iiv`/`fv` 或 `VkBorderColor` 家族间选择）". Header `:279-282` says the same in
English and adds "Carried as a blob until P0.5 gives it a value header" — **P0.5 has landed**, so the blob is
the P0.5-extracted `SamplerParameters` of `MobileGL/MG_Pipe/MGPipeValueTypes.h` (which also holds
`BorderColorForm`, per `ARCHITECTURE.md:262`'s extraction list).

```cpp
struct MGPSamplerDesc { MGPipeHandle Cso; MGPBlobRef Parameters; };   // 32
```

**`MGPSamplerView` — 36 B, `MGPipeTypes.h:293-304`** and **`MGPTextureParams` — 32 B, `:307-318`**:

```cpp
struct MGPSamplerView {
    MGPipeHandle Cso; MGPipeHandle Texture;
    Uint32 InternalFormat;                    // aliasing format for glTextureView
    Uint8 Target; Uint8 Pad0[3];
    Uint16 MinLevel, NumLevels, MinLayer, NumLayers;
    Uint16 Samples; Uint8 FixedSampleLocations; Uint8 Pad1;
};                                            // MGP_ASSERT_POD(..., 36)

struct MGPTextureParams {                     // "Per texture OBJECT, independent of any view."
    MGPipeHandle Res;
    Uint16 BaseLevel, MaxLevel;
    Uint8 Swizzle[4];
    Uint8 DepthStencilMode;
    Uint8 ForceResync;                        // mirrors m_forceTextureParamsResync
    Uint8 Pad0[2];
    Float MinLod, MaxLod, LodBias;
};                                            // MGP_ASSERT_POD(..., 32)
```

`ARCHITECTURE.md:134`: "view 只带视图限制（min/num level、min/num layer、别名格式）；纹理参数（base/max level、
swizzle、depth-stencil mode、LOD 钳、`ForceResync`）挂在纹理对象上". `ForceResync` exists for
`RequireImageBindableStorage` (`ARCHITECTURE.md:100`; header `:312-313`: "the widened-channel carrier needs a
swizzle override that the frontend params version does not move for") — i.e. it is the one field that must move
**without** a frontend version bump, and the new red-before scenario is precisely its habitat.

**The three `kVarTail` set headers — all 16 B, all `Start/Count/ContentHash`** (`MGPipeTypes.h:440-468`):

```cpp
struct MGPBoundView { MGPipeHandle View; MGPipeHandle Texture; Uint32 Unit; Uint32 Pad0; };  // 24
struct MGPSamplerViews  { Uint32 Start, Count; Uint64 ContentHash; };                        // 16
struct MGPSamplerStates { Uint32 Start, Count; Uint64 ContentHash; };                        // 16  (tail: MGPipeHandle[Count])
struct MGPImageView { MGPipeHandle Res; Uint32 Unit; Uint32 InternalFormat; Uint32 Layer;
                      Uint16 Level; Uint8 Layered; Uint8 Access; };                           // 24
struct MGPShaderImages  { Uint32 Start, Count; Uint64 ContentHash; };                        // 16
```

**No stage dimension**, and this is a design ruling not an omission — `ARCHITECTURE.md:99`:

> `SetSamplerViews` / `BindSamplerStates` **没有 stage 维度**：MobileGL 的纹理单元空间是合并的
> （`TextureState::m_textureUnits` 是 192 个单元的一个数组，每 stage 32 只是广告数字），同一单元可被两个 stage
> 采样；stage 只在目标 API 需要时由 server 从反射归档推导。

Repeated at `MGPipeTypes.h:429-431` and again as an explicit non-migration at `PipeCalls.def:182-183` ("the
stage dimension of set_sampler_views … MobileGL's texture unit space is merged, not per stage").

**`MGPProgramDesc` — 192 B, `MGPipeTypes.h:323-335`**:

```cpp
struct MGPProgramDesc {
    MGPipeHandle Cso;
    Uint32 StageMask;                 // == GetLinkedShaderStages()
    Uint32 GlobalUboSize;
    Uint32 ReservedNumSamplesOffset;
    Uint8 SpirvStatus; Uint8 NativeFloat64; Uint8 PointSizeDemoted; Uint8 EnableSpirvValidation;
    MGPBlobRef Spirv[6];              // per stage
    MGPBlobRef Reflection;
};                                    // MGP_ASSERT_POD(..., 192)
```

`ARCHITECTURE.md:135`: "逐 stage SPIR-V blob ×6 + 反射归档 blob + `StageMask`/`GlobalUboSize`/
`ReservedNumSamplesOffset` + 四个状态字节，§7".

**`MGPGlobalConstants` — 40 B, `MGPipeTypes.h:507-513`** — "Covers the DEFAULT UNIFORM BLOCK only (D6)":

```cpp
struct MGPGlobalConstants { MGPipeHandle ShaderCso; Uint32 Version; Uint32 Pad0; MGPBlobRef Blob; };
```

`ARCHITECTURE.md:143`: "`(ShaderCso, Version)` 键控，每 program 每帧至多一次". `ARCHITECTURE.md:102`:
"`SetGlobalConstants` 只覆盖默认 uniform block（D6）：`globalUboScratch` 是 link phase B 的 CPU 数组，没有 GL
name、没有 `BufferObject`."

**`MGPResourceDesc` — 88 B, `MGPipeTypes.h:150-176`** — already carried by P3a for buffers; P4a adds the texture
and renderbuffer discriminants. `ARCHITECTURE.md:130`:

> buffer / 全部纹理 target / renderbuffer 一个判别式 create/respecify 形状；`BindMask` 的 `ELEMENT_ARRAY` 位是
> 索引镜像的开关；`ImageBindableHint` 预防性分配 image-bindable 存储；`ViewOf` 是纹理视图的存储属主（server 侧
> keep-alive）；`BufferForTexBuffer/BufOffset/BufSize` 实时解析（`kMGPipeWholeBuffer = ~0`）。**Renderbuffer 保持
> 独立类**（自己的 format-capability target、`ComponentSizes`、twin）

Texture-relevant members, from the header: `Target` ("Buffer | Tex1D..TexCubeArray | Tex2DMS.. | Renderbuffer |
TexBuffer", `:152`), `StorageKind` (`== TextureStorageType`), `InternalFormat` ("already resolved to an
uncompressed fallback by the client", `:158`), `Width/Height/Depth`, `ArrayLayers/Levels/Samples`,
`FixedSampleLocations/Immutable`, `HasDefinedContent`, `ImageBindableHint` ("client-side everImageBound;
pre-emptive allocation", `:165`), `GlNameForDiag` (**diagnostics only** — `:167-169`: "A GL name is NEVER an
identity, never a memo key and never part of a content hash"), `ViewOf`, `BufferForTexBuffer`,
`BufOffset/BufSize`, `kMGPipeWholeBuffer = ~0ull` (`:177`).

**`kNeedsAck` does not extend to textures.** `ARCHITECTURE.md:294`: "纹理分配的 OOM 在 monolith 里就已推迟到
sync 时刻（`glTexImage*`/`glTexStorage*` 只 `MarkStorageDirty`，Espryt 惰性分配；连 `glRenderbufferStorage*` 也在
`SyncToBackend` 里惰性做），拆分不改变可观察行为，**这批不同步 ack**." And `:295`: "**唯一允许同步 ack 的入口是
`glBufferStorage`**". The per-record predicate is `MGPipeResourceRespecifyNeedsAck(desc) == (desc.Immutable != 0)`
(`MGPipeTypes.h:680`) — P4a must not widen it. `MEASUREMENTS.md:83` is the corpus evidence: 0 OOM-probe idioms
in 41 fixtures.

**Transfer payloads P4a's texture path needs** (`MGPipeTypes.h`): `MGPSubRegion` 40 (`:607-614`), `MGPSubData` 72
(`:636-648`), `MGPSubDataComplete` 24 (`:686-691`), `MGPCopyRegion` 64 (`:708-716`), `MGPMipPlan` 16 (`:741-745`),
`MGPReadbackInfo` 64 (`:748-756`, "read_pixels and get_texture_image share one shape; both answer into a reply
slot", `Res` null for read_pixels).

**The blob rule** — one rule, stated on `MGPVertexElements` (`MGPipeTypes.h:254-265`) and repeated on `MGPSubData`
(`:624-635`): `Blob.Size` is the record's own statement; a **non-zero** size disagreeing with what the other
fields describe is `Fatal{ProtocolCorruption}` and the call is refused; a **zero** size means "this record does
not declare its blob", which is what a monolith emission is, and is not a fault. Every new P4a `kHasBlob` record
(`CreateShaderState`, `SetGlobalConstants`) obeys it. `ID-8b` restates it as an integration invariant: "one Blob
rule (0 = not declared)".

**Slot bounds.** `MobileGL/MG_Pipe/PipeApply.h:112-113`: `kMGPipeMaxResourceSlots = 1u << 20`,
`kMGPipeMaxVertexElementsSlots = 1u << 16`; `:97-111` explains that a slot at or above the bound is
`Fatal{ProtocolCorruption}` **and never a resize**, because the applier's tables are grown *by* the slot index.
P4a adds record tables per new kind and must set each bound the same way, with the same reasoning about the
record's own size. Related trap from P3a's final review (`MEASUREMENTS.md:507`): the client-minted
`VertexElementsCso` slot had **no backend-neutral death path**, so Magma leaked a slot per VAO and `Fatal`d past
65536 slots. **Every kind P4a mints needs a death path that does not depend on which backend is loaded.**

**Composite band.** `MGPipeHandles.h:83-96`: `kMGPipeShaderCsoSlotLimit = 1u<<20`,
`kMGPipeShaderCsoCompositeSlotBase = limit - (limit>>4)` (the top 1/16), `MGPipeIsCompositeShaderSlot(slot)`,
plus `static_assert` that the band does not swallow ordinary program slots. Doc: `ARCHITECTURE.md:39` "…
`ShaderCso` slot 空间的高 1/16 保留给 program pipeline 合成体".

### 2.2 The texture path (`ARCHITECTURE.md` §6, `:247-256`)

Seven statements, all binding on P4a's `resource_*` half:

1. **`glTexSubImage*` does not reach the backend table at all** (`:249`). "全部纹理上传由 Espryt 在 sync 时刻按
   **累积**区域做，那里跑 `MipmapStorage` 的 96-rect 级联合并与 `summedArea*4 >= unionArea*3` 的 union-box 回退，
   并在 unpack ring 可用时刻意塌成一个 box——Mali 按**作业数**给上传计价，~100 个精灵 rect 对一个 union box 实测
   +6 ms/frame。逐 `glTexSubImage` 发一条记录会精确复现那个形状。" This is the explicit exception to §5.1's
   "GL-call-time push" rule: `ARCHITECTURE.md:157` — "**只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**
   ——即 `BufferBackendOps` 的七个 hook。**纹理 subdata 不在此列**（§6）."
2. **Accumulate client-side, emit one record at the next validate/flush point** (`:250`): "client 在自己的
   `MipmapStorage` rect 模型里累积，在**下一个 validate / flush 点**把合并后的形状作为**一条**
   `ResourceSubData` 发出。`MOBILEGL_PIPE_STATS` 单列逐帧发射次数与上传作业数
   （`TextureUploadEmissions/Box/Rect/Jobs`）."
3. **Union box AND region list both travel; the server picks** (`:251`): "**同时携带 union box 与 region 列表，
   由 server 选上传形状**：决策留在付 GPU 代价的那一侧。实测：vanilla 世界同样 185 次发射，Espryt 的整 box 路径
   每帧 635 KB 纹素、Magma 的 rect 路径 40 KB，16×." The measured baseline is `MEASUREMENTS.md:52-53` and the
   reading at `:66`. Payload comment `MGPipeTypes.h:616-618` says the same, plus "Mali prices texture upload by
   JOB COUNT".
4. **Strides carried, not inferred** (`:252`): "`MGPSubRegion` 显式携带 `SrcRowStride/SrcSliceStride`,
   `MGPSubData::SourceIsVerbatimLevelShadow` 显式携带原来由 `uploadData == mipData` 指针比较回答的问题 … Espryt
   的上传路径改为从描述符取步长，`UNPACK_ROW_LENGTH` 从 `SrcRowStride/bpp` 设。形状照抄已存在的
   `UnpackStagingBlock`（ring 路径本来就紧密重打包、不发 `glPixelStorei`）." Note this is also the **one item
   moved out of the Espryt do-not-touch list**: `ARCHITECTURE.md:321` — "从'不动'里移出的一项：Espryt 的 sub-rect
   上传判定与跨步计算（§6，从描述符取步长）."
5. **Dirty-ownership inversion** (`:253`): "client 保留 rect 模型、维护一份发射游标、发射后清自己的标志，server
   从不碰 client 的标志。安全，因为 `MG_Impl` 里没有任何 `IsStorageDirty/GetStorageDirtyRects/
   GetStorageDirtyRegion` 调用点（前端从不读自己的 dirty 状态）。逐 level 'server 权威位'与纹理 ack 协议因此不必
   存在。" (`ROADMAP.md:23` puts "dirty 归属反转（按存储属主键控的发射游标）" in the **P3b/P4b** row — so the
   *cursor* is P3b/P4b; P4a lands the descriptors it keys on. Say which in the brief.)
6. **The cursor's key** (`:254`): "`(storageOwnerHandle, ownerUploadTarget, ownerLevel)`：`TextureObjectView`
   把 dirty 查询/清除全部转发给属主并做索引重映射，view 与属主共用同一份 dirty 状态。门：通过 view 上传、经属主
   采样（及反向），跨 draw 边界各一次."
7. **Two backend shadow writers, and what stays client-side** (`:255-256`): CPU-fallback mip generation
   (RGB16F/RGB32F) → `OnTextureWriteback`; `glCopyImageSubData` destination mirror → "CopyImage 镜像搬到 client".
   "Unpack PBO 完全在 client 解析；压缩纹理永不到达后端；`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在
   client（今天就是纯前端操作：借一次 `ReadPixels` 进 CPU scratch 再写 shadow），拆分后恰好是一次阻塞 ReadPixels
   round trip，脏区按普通 subdata 下发."

**Verify's retain mode** is the gate for (3): `ARCHITECTURE.md:512` — "**保留模式**：消费即清的组（纹理 dirty
rect）发射后无法重算，verify 时 tracker 保留清除前的集合并比对发射出去的 `(UnionBox, RegionCount, Regions[])`."
And the shape gold standard: `ARCHITECTURE.md:513` — "**`TextureUploadShapeScenario`** 把逐纹理逐帧的上传形状
（box vs N region、作业数）录金标比对——+6 ms 悬崖由形状相等把关，SSIM 对它完全不敏感." (`ROADMAP.md:23` lists
`TextureUploadShapeScenario` in the **P3b/P4b** gate cell with "形状金标，Mali 帧时增量必须发布"; P4a's texture
`resource_*` is what makes it meaningful, so expect to build it in P4a and gate on it in P3b/P4b — resolve this
in the brief.)

### 2.3 Sampler-view resolution stays on the client (`ARCHITECTURE.md` §5.5, `:204-206`)

> GL 是每 unit 每 target 各一个绑定；shader 看见哪一个取决于 sampler uniform 类型、mipmap 完备性
> （`IsMipmapCompleteForFilter`、`SamplesAsIncompleteTexture`）与 `IsUndefinedDefaultTexture`。gallium 的"每槽一个
> view"就是解析后的形态，解析留在 client 并带自己的 memo（~40 行搬迁）。**两处后端特定后处理留在 server**、作用于
> 已解析集合：Espryt 的 raw-depth-fetch sampler 替换、Magma 的 feedback-loop 检测。

Espryt's raw-depth-fetch sampler is a live `SharedPtr<SamplerObject>` today:
`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:61` (`static SharedPtr<MG_State::GLState::SamplerObject>
g_rawDepthFetchSamplerState`), constructed at `:208-217`. `ARCHITECTURE.md:323` puts its nativisation in the
same bucket as Magma's placeholder textures: "Espryt 的小号同类：`g_rawDepthFetchSamplerState` → 后端原生
sampler." (`ROADMAP.md:23` assigns "raw-depth-fetch sampler 原生化" to **P3b/P4b**.) It is a **purity-gate
blocker**: a `MG_State::GLState::SamplerObject` inside `MG_Backend` is exactly what gate B greps for.

### 2.4 Lifetime, ordering and the composite resolver (`ARCHITECTURE.md` §5.6, `:208-212`)

- `:210` — "resource_create 在前端对象构造时发，存储由 `resource_respecify` 惰性定义；`resource_destroy` 在析构时
  发。**三条顺序约束由 payload 表达**：view 先于存储属主销毁（`ViewOf` + server keep-alive）、**FBO attachment
  钉住纹理（surface handle 隐含 keep-alive）**、buffer texture 钉住 buffer（`BufferForTexBuffer`，范围实时解析）."
  Two of the three are P4a's.
- `:211` — share group: one screen, one context, one flow; `eglMakeCurrent` is a flow ownership transfer emitted
  under the existing `EGLOperationMutex`.
- `:212` — **the composite resolver**, in full:

  > program pipeline 合成体：`GLContext::GetProgramForDraw()` 今天就完全在前端合成（join、签名查 cache、
  > `Link(true)`）。tracker 拿到 `SharedPtr<ProgramObject>` 推**一个** handle，slot 从 `ShaderCso` 保留高位段分配，
  > pipeline cache 淘汰时释放 slot、`gen++`、发 `delete_shader_state`。合成体从不过线，server 不需要任何"解析后的
  > draw program"钩子；副带收益是阻塞的 `JoinLinkAndSpirv()` 离开 server 的 draw path。

  Coverage backs this: `MobileGL/MG_Pipe/Coverage.def:100` `X(GetProgramForDraw, SetDrawProgram)` and `:101`
  `X(GetProgramObject, CreateShaderState)`; `GetProgramObject` is one of the seven **sticky** fields
  (`Coverage.def:147`, "keyed by GL name: an object lookup, not verb state").

### 2.5 Ordering invariant and the emission suppressor (`ARCHITECTURE.md` §5.4)

`:196`, the D-B3 norm, verbatim:

> **一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态
> 惰性特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间没有顺序要求。**

The recommended order (framebuffer → program → texture/sampler/image/buffer/global constants → render
state/dynamic → vertex elements/buffers/index/attrib defaults → patch/XFB → verb) is "只是代码组织，不是契约".
`:196` also names what lazy specialisation retires: "退役 Espryt 的 fragColor 重推导 workaround、`g_broadcastMemo*`
与 `ImageUnitFormatsStillMatch` 的机制是惰性特化，不是调用顺序" — (`ROADMAP.md:23` schedules the actual deletions
of "fragColor 重推导 workaround 与 `g_broadcastMemo*`" in **P3b/P4b**). The live site:
`MobileGL/MG_Backend/DirectGLES/Managers.h:2030` `Bool ImageUnitFormatsStillMatch() const;` with 20 lines of
rationale at `:2010-2029` (format-less image uniforms are compiled against the *binding*).

`:200`, the four merge rules, including the fourth that P4a's three var-tail sets live or die by:

> **集合 hash 抑制器**——每条 `kVarTail` `set_*` 在 client 算已解析集合的 xxHash，未变不发。最后一条是从后端搬到
> client 的 ~175 行去抖（`UnitBindingsSnapshot`/`PairingsIntact`/`g_fboTextureSyncList` 族）的载体：
> `GetTextureBindGeneration()` 在冗余重绑时也 bump（MC 26.2 每次纹理单元切换都重绑同一个 sampler），没有抑制器
> 每个 batch 都会重发一条几百字节的变长记录并冲掉 server 的两个 memo。

Also `:200` first three rules: 整块结构优于逐字段; high-water marks (`GetTouchedBindPointCount`,
`GetMaxTouchedUnit`) stay in the tracker walk and are directly the `count` argument; only the program-resolved
set is emitted (`uniformSamplerOrImageUnitIndex`).

### 2.6 Dirty bits P4a's families hang off (`ARCHITECTURE.md` §5.2, `:161-172`)

| bit | class | shutter |
|---|---|---|
| `NEW_SHADER`, `NEW_SHADER_BINDINGS`, `NEW_GLOBAL_CONSTANTS` | value | link / image-unit / backend-state / block-binding / uniform-write-set / UBO-content versions (`:165`) |
| `NEW_FRAMEBUFFER` | object | **`FramebufferState::m_anyAttachmentGeneration`**(new) + object/slot version → recompute `ContentHash` (`:168`) |
| `NEW_SAMPLER_VIEWS`, `NEW_SAMPLERS`, `NEW_SHADER_IMAGES` | object | **`TextureState::m_anyTextureContentGeneration` + `m_anyTextureParamsGeneration`**(new) + bind/sampling-resolution generation → walk the `GetMaxTouchedUnit()` prefix, recompute the set hash (`:169`) |

`:172` — the five aggregate generations all sit on existing bump points (~20 lines) and turn the object-class
shutter from "每 validate 走查 192 单元 / 84×4 绑定点 / 32 属性 / 40 attachment" into one `Uint64` compare;
"对象类不能靠轮询逐对象版本（没有聚合能回答'有没有哪张已绑定纹理动了'，这正是 Magma 不得不用有损
`sampledContentSum` 的原因）".

Completeness is a **gate**, `:174`: `scripts/gen_pipe_dirty_surface.py` enumerates every mutator in
`MG_Impl/GLImpl` → the aggregate generation it must bump; unmapped, or a row naming a mutator that no longer
exists, fails. P3a widened the scan root to `MG_State/GLState` (**G9**, `BRIEF-P3A.md:166`; result
`MEASUREMENTS.md:376`: 75 mutators all mapped, 0 COARSE / 0 UNDECIDED, plus 21 self-test negative controls).
P4a's new generations land in that table or the gate goes red.

`:176` — the three wrapping `Uint16`s are widened **at the tracker boundary** (`m_lastPushed[]` is the tracker's
own field, `MG_State` is not changed); wrapping is locally harmless (one extra push, never a missed one) and is
swallowed by the set-hash suppressor.

### 2.7 Shader state (`ARCHITECTURE.md` §7, `:258-265`)

- `:260` — "**`CreateShaderState` 的 payload 是逐 stage SPIR-V + 反射归档（`LinkArtifacts` + `SpirvArtifacts`
  全结构体），不是源码**。'server 从源码重新 link'这条路显式关闭：链接真 `ProgramObject` 就链接 glslang。glslang
  全在 client，SPIRV-Cross（`TranspileSpirvToEssl`）全在 server，文件级切割。没有 `MOBILEGL_IPC_PROGRAM` 开关、
  没有 server 侧 compile pool." (`ARCHITECTURE.md:608` repeats the non-knob: "显式不设立：`MOBILEGL_IPC_PROGRAM`
  （没有 relink 档）".)
- `:261` — the archive mechanism: `Visit()` + a `sizeof` trip wire
  (`static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`), one field table serving both directions.
  **The must-cover list, verbatim**: "必须覆盖四个 `ResourceReflection`（各带 `TypeFacts`）、
  `uniformSamplerOrImageUnitIndex`、`uniformBlockBinding`、`shaderStorageBlockBinding`（按名字）、
  `explicitOpaqueUniformBindings`、`xfbVaryings/xfbStrides/xfbPackedStride/xfbNeedsScatteredCapture`、
  `computeLocalSize`、GS/TCS/TES 事实、`usesReservedNumSamples`、`uniformOffsets`。`XfbVarying` 带两套拼写
  （GL 名字 + block 实例/成员/元素）."
- `:262` — **P0.5 is the hard precondition and it has landed**: the five reflection types live in
  `MG_State/GLState/ProgramState/ProgramArtifacts.h`. "没有这一步，P7 的 `nm -D | grep glslang` 判据不可达."
- `:263` — **server-side lazy specialisation (D-B2)**, and the eight extra inputs a backend program still
  depends on: "draw FBO 的 snorm/unorm clamp mask、fragColor 广播数、storage-block 绑定签名、atomic counter 集、
  活的 image 格式、patch 参数；Magma 另加 FragCoord-Y-flip 的 default-FB 高度与 XFB 布局",
  "`create_shader_state` 发布**制品**，server 在 verb 时刻从已推送状态特化".
- `:264` — **link failure needs no synchronous return**: "今天只是一行 `MGLOG_E` 加 bind program 0 的空 draw，
  `GL_LINK_STATUS` 永不撤回，同步查询由 client 从 `ProgramObject` 回答。`OnLog` 逐字复现——由此要求日志按严重级
  分级（§8.3)." The log grading rule is `:283`/`:297`: ≤WARN lossy, ≥ERROR lossless + rate limited, because "后端
  link 失败只以一行 ERROR 呈现".
- `:198` — the async win P4a unlocks: "`create_shader_state` 从编译池的终止 continuation 发出（不是从 draw），
  SPIR-V 在首个用到它的 draw 之前到达 server——monolith 拿不到的异步收益."
- `:265` — Magma's two internal shaders baked to in-tree SPIR-V is **P7**, not P4a.

### 2.8 The reverse-channel callbacks a texture / FBO needs

`ARCHITECTURE.md:269-286` (§8.1), and the tree: `MobileGL/MG_Pipe/MGPipeCallbacks.h:27-51`, ten function
pointers with `kMGPipeCallbackCount = 10` and a `static_assert` on the struct size (`:55-57`). Of the ten, the
ones P4a's families produce or consume:

| callback | header | why P4a |
|---|---|---|
| `OnGpuWritten(res, rangeCount, ranges)` | `MGPipeCallbacks.h:31` | ARCHITECTURE `:276`: replaces 6 `MarkGpuWritten`; the client builds a conservative pending set per draw/dispatch emission — a **narrowing** channel |
| `OnTextureWriteback(res, box, bytes)` | `:33` | `:278` "CPU 回退生成 mip 的纹素（唯一生产者）" |
| `OnTexturePullRequest(res, target, firstLevel, levelCount, pullSerial)` | `:37-38` | §8.4, the only new stall class |
| `OnMipLevelsGenerated(res, base, count)` | `:41` | `:280` shape only, never bytes: monolith's `EnsureGenerateMipmapStorageAllocated` also only allocates and `MarkStorageDirty(false)` |
| `OnSurfaceChanged(info)` | `:44` | `:281` retires the swapchain→`pDefaultFramebufferInfo` layering inversion; pairs with handle `{0,1}` |
| `OnGlError(code)` | `:29` | `:275` **must be ordered w.r.t. the command stream** or `glGetError` answers wrong |
| `OnCapsInvalidated()` | `:45` | `:282` two `InvalidateCompileEnv` sites |
| `OnLog(level, text)` | `:47` | `:283`, and §7's link-failure requirement |
| forward terminator `ResourceSubDataComplete` | `PipeCalls.def:136`, `MGPipeTypes.h:686-691` | §8.4 item 4 |

**§8.4 — the only new stall class (D-B6), `ARCHITECTURE.md:299-308`.** Three causes a server that keeps no
texels must ask for a level back: "`RequireImageBindableStorage` 的 re-dirty、整格式再生、view 源重铸". Four
mitigations, all four at once:

1. Prevention: `everImageBound` → `ImageBindableHint` on every `ResourceCreate/Respecify`.
2. Async: the server marks the twin not-ready and the client re-sends at its next publish; **the blocked thread
   is `mgl-srv-apply`, not the application thread**.
3. Bounded retention, default off: `MOBILEGL_PIPE_TEXEL_RETAIN_MB = 0` — "`MipmapStorage` 保有每 level 完整 CPU
   影子，拉取总能被服务，缓存买的是延迟不是正确性".
4. **Explicit terminator**: `ResourceSubDataComplete(res, target, firstLevel, levelCount, pullSerial)`, "**可携带
   零个 region**"; on zero regions the server continues with allocated-but-empty storage (monolith's own
   behaviour) and logs `MGLOG_W`. "没有终止符 apply 线程会永久 park."

Gate (`:308`): "`TextureRemintPullScenario`（含无解用例，**且在终止符落地前必须是红的**）；拉取次数逐 trace 用例
发布。本设计从不声称'零 round trip'，它测量并公布." — a second red-before scenario in P4a's blast radius, and
its rate is `ROADMAP.md:85`'s open question 2 ("真实语料上纹理重铸拉取的发生率… 若 MC/Iris fixture 上非平凡，保留
LRU 从默认 0 升为强制并拿真预算").

**§8.2 — ordering is a correctness requirement**, `ARCHITECTURE.md:290`: "每一次 `WritebackFromBackend` 后面都
紧跟 `BumpBufferMutationEpoch()` … 写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。
**反向通道需要与正向通道相同的有序保证。**"

**§8.1's remaining-95-sites paragraph, `:286`** — the rules P4a inherits: `MarkStorageDirty` is mostly
server-local bookkeeping (zero messages); backend-invented frontend objects (Magma placeholder textures,
swapchain default-FB placeholder) become server-native; `SetBackendResource` is deleted **except** that
`D-K (P3a)` did not delete it (see §5 traps); "`SetBackendStateMemo`（前端 VAO 里存后端堆裸指针）直接删除；
`SetBackendHashMemo/AuxMemo` → server 侧 per-slot 字段".

### 2.9 The Track H census rows P4a pays (`ARCHITECTURE.md` §9.5, `:363-365`)

The census's single fact, verbatim (`:365`):

> 统一事实：每个进入 memo 键的版本计数器要么是回绕 `Uint16`，要么根本不会被它害怕的那个 mutation bump；身份比较
> 是堵回绕洞的补丁。`{slot, gen}` + 显式 destroy 让 **11 条直接删除** … **2 条** server 删除但去抖搬到 client
> （§5.4），**7 条重键**成更便宜的比较（`StampSyncedFBO` 四元组 → `ContentHash` + server 私有
> `attachmentRemintEpoch`；`ResolvedTextureBindingMemo` 9 键 → `(shaderCso.slot, viewSetSerial)`；
> `SetupDrawSnapshot` 的 ~14 探测字段与两个有损求和 → 三个 handle + 两个 server 纪元 + dirty mask；
> `VertexInputStateFactory::ComputeHash` 里的 lifetimeId → `gen` **混进** server 侧每个 content hash），
> **1 条**（D18）原样不动。

Ledger to date: **P2** paid all 11 direct deletions plus 2 of the 7 re-keys (`MEASUREMENTS.md:425`); **P3a** paid
the last direct deletion (`ConvertedVertexStreamKey`'s `sourcePin`), retired 5 more twin members *on the handle
arm only*, and paid 4 re-keys including **`ResolvedDrawBuffers`** (`MEASUREMENTS.md:429-431`, `ROADMAP.md:19`).

**Which memos remain, and whose row they are.** `ROADMAP.md:23`, the **P3b/P4b** cell, verbatim:

> memo 重键（`ResolvedDrawBuffers`、`ResolvedTextureBindingMemo`、`SamplerPassMemo`、image sweep、program
> registry…）；server 删 `g_unitTextureSyncList`/`g_fboTextureSyncList`/`DirectGLES.cpp` 的 ~115 行 unit-bindings
> epoch 推导，**同时**在 Tracker 落地集合 hash 抑制器；dirty 归属反转（按存储属主键控的发射游标）；
> `MGPSubRegion` 跨步描述符改造；XFB scatter 搬到 client；删 fragColor 重推导 workaround 与 `g_broadcastMemo*`；
> raw-depth-fetch sampler 原生化；回读 / pack state

`ResolvedDrawBuffers` in that list **is already done** (P3a, `MEASUREMENTS.md:431`: "`configVersion` →
`{elementsHandle, elementsSerial, buffersSerial}`，`Entry` 与 `iboFrontend` 各加一个 `MGPipeHandle`"). So the
P4a/P3b/P4b boundary is: **P4a lands identity + descriptors; P3b/P4b lands the deepening re-keys.** The sites
the later phase will touch, located for you now (so P4a can decide what to leave reachable):

| census row | site in the tree |
|---|---|
| `ResolvedTextureBindingMemo` (9 keys → `(shaderCso.slot, viewSetSerial)`) | the unit/FBO texture sync lists, `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:1814-1830` (`g_unitTextureSyncList` + 6 key statics; `g_fboTextureSyncList` + 5) and the two memo-hit tests at `:1879-1885` and `:1950-1955` |
| `SamplerPassMemo` | `MobileGL/MG_Backend/DirectGLES/Managers.h:1913-1924` (the struct, `kMaxEntries = 16`, keyed on contextId / unitBindingsEpoch / samplingGeneration / backendStateVersion / textureContextGeneration + a row snapshot), accessor `:2001`, member `:2117`; its invalidation contract is documented at `:1895-1912` |
| image sweep / `ImageUnitFormatsStillMatch` | `Managers.h:2030-2032` + `ComputeImageUnitFormatSignature()`; rationale `:2010-2029` |
| program registry | `Managers.h:2130` `extern TwinRegistry<ProgramObject, BackendProgramObjectImpl, MGPipeKind::ShaderCso>` |
| `StampSyncedFBO` 四元组 | the FBO trio is named in the code at `DirectGLES.cpp:1936` ("heap address (pointer + slot version together, the StampSyncedFBO trio)"); registry `Managers.h:1612` |
| `SetupDrawSnapshot` ~14 probe fields | Magma, **P7** (`ROADMAP.md:24`) |
| D18 container discipline | untouched, `ARCHITECTURE.md:318` |

**The six P2 registries are the six kinds.** `MobileGL/MG_Backend/DirectGLES/Managers.h`:
`:1181` VertexArrayObject→`MGPipeKind::VertexElementsCso`, `:1521` ITextureObject→`Texture`,
`:1612` FramebufferObject→`Framebuffer`, `:2130` ProgramObject→`ShaderCso`, `:2230` SamplerObject→`SamplerCso`,
`:2257` RenderbufferObject→`Renderbuffer`. P3a added the **seventh** table, the first one keyed by a
client-minted handle that travels with the call:
`Managers.h:691` `BackendSlotTable<BufferObject, GLESBufferResource, MGPipeKind::Buffer>` (`ROADMAP.md:19`).
**P4a's job is to do to the remaining five (Texture, Renderbuffer, Framebuffer, SamplerCso, ShaderCso) what P3a
did to Buffer.** The template, its dispatch and the P2/P3a switch live at `Managers.h:303-553`
(`StateBackendObjectRegistry`, `GetOrCreate` routing to `m_slotTable` when `EsprytSlotTablesEnabled()`,
`TwinRegistry` alias that drops the kind parameter in the pull build so the mangled name is the pre-P2 one).

**The recorded P3+ debt this phase inherits**, `MobileGL/MG_Backend/DirectGLES/SlotTables.h:70-77`, verbatim:

> P3+ DEBT, recorded rather than hidden: this header is under `MG_Backend/` and it MINTS handles
> (`MGPipeSlots().Acquire` below) off a frontend `SharedPtr`'s `GetLifetimeId()`. `MGPipeHandles.h:13-16` says a
> handle is minted by the CLIENT and never by the server … This is monolith glue: the minting and the
> lifetimeId → handle resolution both belong on the client, and the backend should receive the handle in the
> verb payload. It is NOT part of "Track H done" and `check_include_closure.py` does not probe `MG_Backend`
> headers, so nothing catches it automatically.

For Buffer, P3a closed exactly that (client mints, the handle rides the call). **Each kind P4a converts closes
one more slice of this debt, and nothing automated will notice if it does not** — make it an explicit review item.

`SlotTables.h:36-68` also states the death protocol the five new kinds must obey: all six re-keyed object
classes raise `MG_State::GLState::NotifyStateObjectDestroyed()` from their destructor, consumed in
`Managers.cpp`, `OnFrontendObjectDestroyed()` drops the twin in **every** holder of the kind and returns the slot;
there is **no** draw-path tick, no creation tick and no sweep on this arm; every table links itself into a
per-table-type list so one notice resolves the handle once, drops the twin in each holder by handle, and frees
the slot last.

### 2.10 The emulation ownership table (`ARCHITECTURE.md` §5.7, `:216-245`)

The rule, `:216`: "**驱动表达不了的变换在 tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering。**
只有三个'读前端字节的纯 CPU 变换'下放到 client."

| emulation | owner | what crosses |
|---|---|---|
| client 顶点数组 `(first+count-1)*stride+elementSize` | **client** | bytes via `MGHostSpan`, never a pointer |
| 最大索引扫描 `TryComputeMaxIndexFromHostBytes` (the only unbounded application-pointer read) | **client** | `MGPDrawInfo::MinIndex/MaxIndex`, flag-gated, `~0` = unknown |
| client 索引数组 | **client** | `MGHostSpan` in the var tail |
| `*IndirectCount` count resolution | **client** | resolved `MGPDrawRange[]` |
| primitive-restart rewrite (whole EBO, `kMaxRestartRewriteBytes` = 64 MiB) | **server** | zero wire traffic — reads the index host mirror (§10.3) |
| multi-draw five-tier + flatten (`ResolveTierForBatch`) | **server** | same |
| viewport-array N-pass replay | **server** | nothing new — 16 sets already in the render state |
| fp64 vertex narrowing | **server** | raw bytes; `IsLong` and `Type` travel separately |
| **image-bindable storage widen/split** | **server** | forward `ImageBindableHint`; reverse = texture pull + terminator |
| **generate-mipmap frontend storage** | **split**: client allocates level storage, server generates | `MGPMipPlan`; `OnMipLevelsGenerated` carries shape not bytes |
| **CopyImage shadow mirror** | **client** | only "copy succeeded" — deletes a whole server→client byte channel |
| XFB CPU primitive accounting | **client** | `XfbCpuCapturedVertices` (flag-gated) + `EndStreamOutput`'s `MGPXfbAccounting` |
| XFB scatter read-modify-write | **client** | §8.5 |
| **压缩纹理 / pixel unpack 规整** | **client** | nothing |

The **stale-index discipline is a per-site table, not a blanket rule** (`:235-243`): "client 侧扫描/解析之前要做的
reconcile 必须逐字复现 monolith 的集合" — client vertex arrays: nothing; max-index scan from an EBO:
`SyncPersistentMappedRange()` **+** `SyncGpuWrites()`; max-index scan from a client pointer: nothing;
`*IndirectCount`: **only** `SyncPersistentMappedRange()`, never `SyncGpuWrites()` ("monolith 今天就只做这一个，
加了会给 Create/Flywheel 的每 batch 平白加一次 publish-and-wait"); server-side restart/flatten: reads the
mirror, GPU-writer visibility decided server-locally from `OnGpuWritten`'s narrowed set. Shape of the first two
client reconciles (`:245`): "publish → 等 `appliedSeq` → 排空事件 → 再碰 shadow", gated by
`ClientArrayAfterComputeWriteScenario` (removing the wait must show missing geometry) and by
`roundtrips-per-frame == 0` on the `create-indirect` fixture — both **P8**.

Explicit non-migrations that bind P4a (`ARCHITECTURE.md:104`, `PipeCalls.def:169-183`):
`GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv`; `ShaderStorageBlockBinding` (folded into the reflection
archive); **`set_pixel_unpack_state` does not exist** ("前端已在 `glTexImage` 时解析压缩格式、强制默认 unpack");
the compressed-format concept; `pipe_transfer`; the stage dimension of `set_sampler_views`.
`MGPPixelPackState` is PACK only (`:140`, D5), and `Coverage.def:170-180` explains why
`GetPixelStoreParameters` is deliberately **absent** from the emitted list: `PipeInputs::m_pixelStore[2]` holds
both halves, so claiming it supplied would blind both the poison and the verify comparator to the unpack half.

### 2.11 The purity gates

`ARCHITECTURE.md:511` (part 1 of the five-part gate), verbatim:

> **接口纯度三道门**（只跑非 verify 构建）：**A 门 include 图**——disaggregated 配置编译 `MG_Backend` 时把
> `MG_State/GLState` 从 include 搜索路径移除（`nm --undefined-only` 对"只 include 不调用"是瞎的，而
> `RenderState.h → FramebufferObject.h → TextureObject.h` 正是这种耦合），依赖 P0.5；**B 门符号**——
> `nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**C 门未声明**——
> `grep -c 'pGLContext' MG_Backend/` == 0。外加 debug 断言"每个后端 memo 键都是 `{slot, gen}`，永不是裸前端
> 指针"，由 `HandleRecycleScenario` 支撑（重键前必须在至少一个后端上是红的）。

Note the A-gate example is **literally P4a's include chain** (`RenderState.h → FramebufferObject.h →
TextureObject.h`). Realisations: G13 (`BRIEF-P3A.md:170`) adds "no `MG_State::GLState` type appears in any
`MGPipeResourceOps` signature" and `check_include_closure.py --mode both --compiler clang++-20 --self-test
--require-all`; P3a's readings at `MEASUREMENTS.md:335-337` (A: 4 probes, 0 skip, 0 problem; C: empty; G13:
`PipeApply.h`'s ops block free of `MG_State`). P13 is where the three gates finally turn green on non-verify
builds (`ARCHITECTURE.md:377`, `ROADMAP.md:30`).

Two byte-level equalities survive everything (`ARCHITECTURE.md:517`):
`MOBILEGL_BUILD_DISAGGREGATED=OFF` ⇒ `nm --defined-only libMobileGL.so | grep MG_Remote` empty and no added
link libraries; `nm -D libMobileGL.so | grep mobilegl_server_main` hits in RelWithDebInfo.

### 2.12 The switch and its bits

`ARCHITECTURE.md:598` — `MOBILEGL_PIPE_PUSH`: pull build `0`, **push build `0x1ff`**
(`kMGPipeSubsystemsMigratedAtP3a`, `MobileGL/MG_Pipe/MGPipe.h:95`, read at `MobileGL/ConfigLoader.cpp:257`).
Bits are allocated in ROADMAP order and **never reused**: `0x01` render state, `0x02` pixel pack, `0x04` patch,
`0x08` vertex attrib defaults, `0x10` residual, `0x20` Espryt slots, `0x40` Magma vertex input, `0x80` resources
(P3a), `0x100` vertex input (P3a); "**位 9..62 留给后续阶段**" — P4a's subsystems take bits 9 and up. Bit 63 is
not a subsystem but the behaviour control `kMGPipeBehaviourNoCsoContentAddressing` (`MGPipe.h:90`).

Precedent for a bit that **requires** another (P3a's bit 8 requires bit 7): `0x17f` prints one ERROR naming the
refusal and falls back to the legacy arm (`Managers.cpp:2312`, `ARCHITECTURE.md:374`, `:598`). P4a's sampler
views plausibly require the texture-resource bit for the same reason (attribute→resource-slot resolution's
analogue), and the arm classification must be **diagnose at bring-up, stop at first lookup** — `SlotTables.h`'s
`ClassifyEsprytSlotArm` / `CurrentEsprytSlotArmVerdict` and the long comment explaining why the stop cannot live
at bring-up (a forked pre-flight child dying on a signal makes the whole lane SKIP green, "which is what
`ROADMAP.md:7` forbids").

Also `ARCHITECTURE.md:590`: `MOBILEGL_PIPE_LEGACY_MEMOS` default ON for P2..P13; `MOBILEGL_PIPE_PUSH=OFF` forces
it back ON with a `message(STATUS)` (`CMakeLists.txt:476-479`) "因为 pull 构建里 pre-handle 臂就是唯一的实现".
And `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` (`:599`) is negative control C: "故意打掉句柄身份，让 `HandleRecycle` 的
ABA 臂重现旧的 A-B-A 污染。**它变绿即为控制失效**" — P4a extends `HandleRecycleScenario` with its own kinds the
way P3a added a buffer case (`ROADMAP.md:19`, `MEASUREMENTS.md:341`).

---

## 3. What later phases expect P4a to leave in place

**P3b / P4b** (`ROADMAP.md:23`, depends on P4a). Deliverables quoted in §2.9 above. Its gate cell, verbatim:

> ~25 个纹理场景、21 个 program 场景 + `MG_Test/ShaderTranspiler`；两台设备 `KHR-GL46.texture_*`/
> `internalformat.texture2d.*`/`shader_image_*`/`packed_pixels` 在 pull 基线 0.5 pp 内；每一个 Iris trace；
> **`TextureUploadShapeScenario`**（形状金标，Mali 帧时增量必须发布）；view/owner 发射游标别名场景；verify 保留
> 模式下 subdata 形状逐项相等；XFB 场景 + `capture_special_interleaved_test`

So P4a must leave behind: descriptors and handles that a per-storage-owner emission cursor can key on
(`(storageOwnerHandle, ownerUploadTarget, ownerLevel)`); the `ContentHash` fields the suppressor will latch;
sampler-view/sampler/image sets resolved on the client so the backend memos can be re-keyed onto
`(shaderCso.slot, viewSetSerial)`; and the verify **retain mode** comparison for subdata shape.

**P5** (`ROADMAP.md:21`, depends on P4a): `MG_Remote/Client` emit tables; `Server/PipeApplier` + `ServerLoop`;
one `Init.cpp` hook installing `BackendObject_Remote`; `MGPCaps` snapshot; blocking `read_pixels`; client-side
conservative `MarkGpuWritten`; client-side block-granular persistent-map push; `InProcessTransport` through the
**same G3 codec** as spawn; trace-replay `SPLIT` suffix. Its acceptance includes "未迁移字段读 =
`Fatal{UnmigratedPipeInput}`" and the day-99 first IPC frame. Implication for P4a: every payload it fills must
be transport-ready (flat POD, blob rule, declared counts) because P5 encodes them without re-deciding shapes.

**P7 — Magma** (`ROADMAP.md:24`, depends on P0.5/P2, **parallel** with P5/P6/P8, not on P4a): the other 10
subsystems, `SetupDrawSnapshot` probe fields collapsed to a dirty mask, **placeholder textures nativised
(~120 lines deleted)**, named-UBO host payload (D-B8, `kCapNeedsHostUboBytes`), blit/depth-mipmap internal
shaders baked, `VertexInputStateFactory` raw-pointer writeback deleted, D18 discipline preserved. Gate includes
`nm -D libMobileGLServer.so | grep glslang` empty. Two P4a-adjacent consequences: the content-addressed
vertex-elements CSO table (1024 entries) is P7's, not P4a's (`ARCHITECTURE.md:65`, D-G1); and P4a's shader CSO
must be shaped so Magma's `ProgramFactory` can consume the same archive later (`ROADMAP.md:91`, open question 8:
"一份反射归档能否服务三个消费者").

**P8** (`ROADMAP.md:25`, depends on P6 and P3b/P4b): `MG_Impl/Pipe/HostResolve.cpp`, `MGHostSpan` split fill,
`Server/IndexHostMirror`, **CopyImage 镜像搬到 client**, `draw_vbo` absorbing the multi-draw family,
viewport-array replay validation, **`generate_mipmap` 计划 + CPU 回退纹素**, G3 chunked path, no-present fence
tick + a no-present split case, `kCapDriverOrderedXfbCapture`. **This is the phase that retires P4a's
"emulation 在 split 下显式 Fatal"** — so each Fatal P4a plants must name the emulation and be greppable, and its
gate is "`'^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` 逐名相同" plus 40-trace split SSIM ≥ 0.99. Also
`ARCHITECTURE.md:391-396` (§10.3): the index host mirror is built out of the `ResourceCreate/Respecify/SubData`
stream P3a/P4a already send — "零额外线上流量、零 round trip" — and is "本设计里唯一的'数据副本'".

**P9** (`ROADMAP.md:26`) consumes the reverse channel P4a produces: `OnTextureWriteback`, `OnMipLevelsGenerated`,
the four texture-pull mitigations + terminator, `OnGlError` ordered + the `glBufferStorage` ack,
`OnCapsInvalidated`, `OnSurfaceChanged`. Its gate: "`TextureRemintPullScenario` 绿且含无解用例（终止符前表现为
apply 线程挂死/超时）".

**P13** (`ROADMAP.md:30`) is where P4a's parked debt actually dies: delete `SnapshotFromGLContext()`'s non-verify
branch, `MGB_CTX`, `MOBILEGL_PIPE_PUSH`, `MOBILEGL_PIPE_LEGACY_MEMOS`; keep `MOBILEGL_PIPE_VERIFY`; delete
`set_residual_value_state`; shrink `MG_Backend`'s `MG_State` includes to `MGPipeValueTypes.h`; **retune surviving
cache capacities with the counters alive** — the named list includes `syncedTextureMemo 8`, i.e. a P4a-owned
cache. Open question 14 (`ROADMAP.md:97`): "推送模型改变哪些按拉取模式调过的缓存命中率。幸存者容量在 P13 重调."

---

## 4. Commit / CI discipline, and the rulings that bind P4a

### 4.1 The per-commit discipline (`ROADMAP.md:7`), verbatim

> 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器
> 不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议；每阶段出口跑一次五部分门；
> 每阶段性能判据是**逐线程 CPU 时间**。

Also `ROADMAP.md:100`, the no-drive-by rule: "**拆分不借机顺手修 `dev` 的 bug**". P3a honoured it twice — the
`g_uploadRing` reset asymmetry (`MEASUREMENTS.md:519`) and `create-indirect` (`ROADMAP.md:100`); P4a inherits
both the rule and those two open items.

### 4.2 CI (`ARCHITECTURE.md:578`)

`pipe-gates` (`.github/workflows/test.yml:1538`): `gen_pipe.py` regenerate + diff; the stdio-instrumentation grep
gate over `MG_Backend`/`MG_State`; `gen_pipe_dirty_surface.py --check` + `--self-test` (a gate since P2,
`test.yml:1601-1604`); `check_doc_citations.py` (warning level, `--strict` once the docs are final). Separate job
`flatc-check` (`test.yml:304`). Already landed: `include-graph-check` (P0.5), `monolith-symbol-report`,
`build-linux-verify` / `integration-verify` / `retrace-verify` (P1). Generated `.inc` files are **committed to
the tree** and the build never depends on python (`ARCHITECTURE.md:73`, `:431`).

Test-wiring traps, `ARCHITECTURE.md:577`: ctest `ENVIRONMENT` is **replace, not append**; `;` must be escaped;
**the property overrides the job env** — use `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})`;
`add_trace_replay_test` needs the `SPLIT` suffix (otherwise it collides with the same case+backend) and
`-DTRACE_TRANSPORT=` for `run_trace_case.cmake`.

Commit-message style (my memory, `commit-style`): `[Type] (Scope): description`, single line, never a
`Co-Authored-By`. Milestone flow (`milestone-merge-flow`): commit + push + merge to `dev` + open the next item
without asking.

### 4.3 The user's 2026-09-08 rule — performance is recorded, never gated

`MEASUREMENTS.md:324`, the rule **verbatim in the user's own words** (§15 exists because it was written nowhere
in `docs/Disaggregated/`):

> (a) advance the roadmap as fast as possible without a large performance regression; performance is RECORDED
> against the pull baseline, never a blocking gate in P3a; correctness gates stay.

Its consequences, `MEASUREMENTS.md:326`: part **4** of the five-part gate (`ARCHITECTURE.md:514`) and
`ROADMAP.md:19`'s "MC 26.3 在 Adreno 上 p99 不变" become **measure-and-publish obligations**, not pass/fail;
parts 1, 2, 3, 5 stay hard gates; "一次回归要写下数字与怀疑的成因并带进 P4a 的排期——**唯一不可谈判的是数字必须
真的采到**：没测到的回归不算记录在案。" Same ruling as `ID-6`. `ARCHITECTURE.md:514` carries it too: "**口径
（2026-09-08 起）**：这一条对着 pull 臂**记录**而不阻塞 … 专门的优化阶段排在路线图之后."

**The baseline of record is `MEASUREMENTS.md` §20** (`:436-503`), not §10. §10's numbers are **-O0** and carry an
erratum (`ID-17`; `ROADMAP.md:18`, `:57`). §20's protocol, verbatim at `:438`: reboot-clean, same thermal window,
`tools/device_bench/pin_device.sh` (big cores 1.96 / little 1.55 GHz, GPU maxed, 40 °C gate, `check` before and
after every run, a DRIFT run voided and repeated), two arms back to back, one device at a time, p50/p99 over the
tail 200 frames of `benchmark.json`'s `frameCpuTimesMs[]` computed host-side. Arms in P3a were three: pull APK /
same push APK with `--env MOBILEGL_PIPE_PUSH=0x7f` (the previous phase's boundary) / push APK at the default
mask. **P4a's analogue: pull / `0x1ff` (P3a) / P4a's default mask.**

Numbers P4a is measured against (`MEASUREMENTS.md:444-461`, `:493-501`, `ID-19`, `ID-24`):

- Device p50, `--benchmark-no-finish`: P2's Release boundary **+6–12%** on both devices and both backends; P3a
  added ~0 on 26.3 and sodium but **+17–19 points on rd12** (Espryt +30%, Magma +27% total).
- **MC 26.3 p99 on Adreno: pull 25.457 → P3a 26.322 ms (+3.4%)** — the number `ROADMAP.md:19` names, still in
  the 21–26 ms band of the adoption baseline at `MEASUREMENTS.md:87` (p99 163→21 ms, 40→115 fps, ~400 MB).
- DriverBench `mc_vanilla_draw` ns/draw at `c20e2f2b`: **T1** (push `0x1ff` − pull) = Espryt **+1048** (+20.5%) /
  Magma **+611** (+3.6%); **T2** (`0x7f` − pull) = +349 / +189; **P3a itself = +699 / +422**. Blend-toggle
  +5.2% / +2.8%; pass switch ≈ 0; CSO content addressing ≈ 0.
- `MEASUREMENTS.md:503`: "**不钉上限、也不强制上限**（规则 (a)）；数字发布出来，**让 P4a 自己决定要不要钉**."
  The optimisation candidates P3a handed forward (`:501`): per-draw suppression of unchanged vertex-input sets,
  and in-place applier record updates.

Reading discipline that must sit **above** the numbers, `MEASUREMENTS.md:440`: `CallClass::AccessorCalls` is a
**static** count at ~10 hot entry points (`MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`, "那份站点清单本身就是
契约"), so a change that only **moves** a read gets a lower `acc/draw` without doing less work; lead with the six
memo gates' hit/miss and the CPU time series, and re-check the tally constants before quoting `acc/draw`.

DriverBench mechanics (`:503`): its own `build-bench` (the gate builds set `-DMOBILEGL_BUILD_BENCHMARK=OFF`), and
**`DRIVERBENCH_EGL_LIB` must `dlopen` a provider explicitly — never shadow via `LD_LIBRARY_PATH`**, because on a
glvnd system a bare `libEGL.so.1` resolves to Mesa/llvmpipe and silently swaps the GPU for a software rasteriser
underneath the benchmark.

### 4.4 P3a-era rulings that bind P4a

- **ID-6** — performance recorded, not gated; parts 1/2/3/5 stay hard gates. (Same as §4.3.)
- **ID-11 / ID-13 / ID-15, the G5 shape.** G5 is now **eleven** functions, in two ladders, and the ruling is
  `MEASUREMENTS.md:509` verbatim:

  > **ID-15（取代 ID-13 里"定义在任何 `#if` 之外"那句）。** `Managers.cpp` 把三层 flush 排水梯各带一份、每个预
  > 处理臂一份，任一构建只编译其中之一：`#if MOBILEGL_PIPE_PUSH` 下的 `FlushPendingRangesFrom` 是 **push** 构建
  > 跑的（legacy 调用点与 `Ops_H_Readback` 都到它），`#else` 里与 `5cb826b0` 逐字节相同的 `FlushPendingRangesNow`
  > 是 **pull** 构建跑的；push 构建根本不编译 `FlushPendingRangesNow`。转发函数不可行：G5 的提取器不认预处理，
  > 一个转发的 `FlushPendingRangesNow` 会让同名出现两个定义、门直接退出 2（门跑不了）而不是比对。因此**两个名字
  > 都是 G5 行**：十一个函数，pull 梯对着 P3a 基线比、push 梯对着钉在 `3e298c9a` 的 sha 比；两条梯都不许漂，也不
  > 许彼此漂而门不响。

  The ten from P3a: `IsPoolable`, `EnrollIntoPool`, `AcquireFromPool`, `TrimBufferPool`, `ClearBufferPool`,
  `ProcessDeferredBufferReleases`, `CreateRingStorage`, `RingAvailable`, `RingAllocate`,
  `FlushPendingRangesNow` (`BRIEF-P3A.md:162`, `ID-11`); the eleventh is `FlushPendingRangesFrom` (`ID-8f`).
  Sites: `Managers.cpp:1051` (`FlushPendingRangesFrom`, callers `:1721`, `:2044`, `:2766`, `:2904`) and
  `:1316` (`FlushPendingRangesNow`, callers `:1723`, `:2906`) per `MEASUREMENTS.md:521`. **P4a must not touch
  any of the eleven**, and if it adds an untouched region of its own (the Espryt do-not-touch list at
  `ARCHITECTURE.md:318` is the candidate pool: the three rings, the pool, seven fallback-repack paths,
  `m_backendColorSlots`, the three scratch FBOs and their driver-side shadows, `PackState`, all driver binding
  shadows, the Adreno disabled-attribute SIGSEGV workaround, the Mali XFB-capture-loss workaround,
  `ScopedDefaultUnpackState`, the SPIRV-Cross session and post-emission ESSL rewrite, the driver POST self-test
  family), it extends the same script (`scripts/p3a_untouched_regions.sh` → its P4a sibling) with its own
  negative controls — P3a's had two (`ClearBufferPool`, `FlushPendingRangesNow`, `MEASUREMENTS.md:379`).
  A shared template over an accessor interface is **rejected**: it resizes pull symbols and breaks G1
  (`ID-13`, `MEASUREMENTS.md:521`).
- **ID-17 — Release APKs only.** The P2-era A/B APKs were unoptimised: the P3a pull APK's `libMobileGL.so` is
  15.5 MB with a 9.4 MB `.text`; the P2 A/B's was 43.2 MB / 21.8 MB `.text`, same clang 18.0.4 — a 2.3× `.text`
  is a Debug-flavour `-O0` build, and it replays 26.3 on the Oppo at **42 ms/frame CPU where Release takes 9.7**.
  So `MEASUREMENTS.md` §10's absolutes and its +8–18% deltas are `-O0` readings. `wsl_build_trace_apks.sh` prints
  the lib size "so an -O0 APK can never pass unnoticed again (Release .text ~9.4 MB)". **P4a's device A/B is
  Release-only**, and the baseline of record is §20's table.
- **ID-18 — the exit-order rule.** `exit()` → `PipeInputs::~PipeInputs` (a namespace-scope holder of a
  `SharedPtr<VertexArrayObject>`) → `~VertexArrayObject` → `~BufferObject` →
  `MGPipeEmitResourceDestroyAndFree` → `MGPipeSlotAllocator::FindByLifetimeId` **on a hash table already freed**
  by `~MGPipeSlotAllocator` (`MGPipeSlots()` is a Meyers singleton constructed at the first buffer, so it dies
  first). The fix, landed as `d54ec57a` / `6515c8e6` / `fde5fda3` (`ID-21`, `MEASUREMENTS.md:515`): the MGPipe
  singletons (`MGPipeSlots()`, `MGPipeResourceTrackerInstance()`, `g_applier`, and four more) are
  **never-destroyed** (heap-constructed, intentionally leaked at exit), and **leak-at-exit storage** is used for
  the four static `PipeInputs` holders (`gPipeInputs`, `g_snapshot`, `g_readScratch`, `probe`) that hold frontend
  `SharedPtr`s — "树里只有它们持有前端对象，于是没有任何前端析构函数会从 exit handler 里跑；这是
  `Init.cpp`/`GlobalObjects.cpp` 已有的规则". **Every new static P4a introduces that holds a frontend
  `SharedPtr`, and every new MGPipe singleton, follows this rule.** Native reproduction key:
  `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`; with it armed, P3a's finished tree ran `integration-verify`
  844/844 and push `integration-gpu` 966/966 (`ID-21`).
- **The pin / tapper traps** (`MEASUREMENTS.md:469`, `ID-23`, and my memory files): Oppo ColorOS's install
  confirmation is `topResumedActivity=…InstallGuideActivity` — `mCurrentFocus` shows a systemui window, so
  grepping for it never matches, and not tapping it hangs the harness to timeout; **tapping during a streamed
  install drops the device off adb for seconds and voids all three repeats**; the Xiaomi cannot reboot+pin
  immediately after ~32 runs (thermal clamp: big cores pin at 1689600, or the GPU reads 1050 MHz) — wait for
  `cpuss-0-0` < 42 °C; **a crash resets the GPU pwrlevel range to 2..5**, so every row after a crash carries a
  DRIFT verdict on both arms. `ID-23`: Xiaomi rd12 on Magma aborts on **every** arm including pull (scudo map
  error inside a calloc from `libMobileGL`) — pre-existing, excluded from the table, task chip open.
- **ID-16 / ID-22 — CI reds that are not code signals.** Artifact-download failures (`digest-mismatch`,
  "Artifact download failed after 5 retries") on `retrace`/`retrace verify` matrix entries, and the
  "Delete intermediate Linux retrace artifacts" step dying on a GitHub HTTP 504, are housekeeping/flake, not
  signal. Only real retrace reds **after a successful download** matter.
- **ID-14 — the `BufferTest.cpp` fixture waiver**, and its shape is a warning for P4a: under a push build the
  backend installs both `BufferBackendOps` and `MGPipeResourceOps` at bring-up, so a fixture that scoped only
  the former let 26 of 86 dispatch cases route into the pipe (`MEASUREMENTS.md:523`: "这是**合并缝**的典型形态——
  两个分支各自绿、合起来红"). **Any P4a unit fixture that scopes one op table must scope the new one too.**
- **ID-2 / ID-4 / ID-5 / ID-7 process rulings** carry over unchanged: integration order contract → wire →
  client → espryt → gates; agents on opus, one adversarial review per package, at most two rework rounds;
  durable locations `~/w7/notes/p4a/`; UNC editing works, `mkdir` does not — create directories from a WSL
  script, run builds through a `.sh` with LF endings via `wsl.exe -d Arch -- bash /mnt/c/...`, and launch
  anything over ~8 minutes with `setsid nohup … &` polled through its log.

---

## 5. Traps and open questions a P4a brief must resolve

1. **Suppressor-slot ownership** — `SetHashSuppressor.h:45-52` labels sampler-view/sampler-state slots P3b and
   image/buffer/SO slots P4b, while `ROADMAP.md:20` gives the calls to P4a. Decide: P4a emits with `ContentHash`
   populated, P3b/P4b latches the suppressor. (§1.3 item 12.)
2. **`TextureUploadShapeScenario` and the emission cursor** are in the **P3b/P4b** cells but are only meaningful
   once P4a's texture `resource_*` exists. Decide where each lands. (§2.2.)
3. **`ResolvedDrawBuffers` is already paid** (P3a) despite appearing in P3b/P4b's memo list; do not re-scope it.
4. **The `SlotTables.h:70-77` P3+ debt** (backend minting handles off a frontend lifetime id) is closed *per
   kind*, and nothing automated catches a kind that skips it.
5. **Every client-minted kind needs a backend-neutral death path** — P3a's final review found the
   `VertexElementsCso` leak (Magma installs no death ops → one leaked slot + ~1.3 KB record per VAO, `Fatal`
   past 65536 slots, `MEASUREMENTS.md:507`). `MG_IntegrationTest/Harness/PipeSlotPeek.{h,cpp}` (`:513`) is the
   "see through the GL API" seam that made the leak testable.
6. **Peak RSS is still unmeasurable**: `~/w7/retrace_gate.py` takes only `--tree --lib --out -j --only` and has
   no `rss`/`maxrss`/`getrusage` (`MEASUREMENTS.md:527`). The number was wanted precisely to watch a slot table
   leak. P4a adds five more tables — either instrument the tool or state the hedge (unconditional destroy
   emission + `HandleRecycleScenario` arms).
7. **Still unmeasured from P3a, inherited**: per-dirty-bit fire rates (`FireCount`/`WalkCount` implemented but
   not in `FormatWindowLine`), the 24-bucket per-draw payload histogram (teardown-JSON only, and the trace app
   never reaches teardown), and the `CreateVertexElements` bytes/frame class (`MEASUREMENTS.md:525`, `:465`(5),
   `:288`, `:290`) — the last is explicitly "留给 P4a".
8. **`create-indirect` on device** stays excluded from the A/B and blocked on a `dev` fix
   (`ROADMAP.md:100`, open question 17); G3b is recorded as "desktop green, device partial", never as a pass.
9. **The `--only` regex trap**: `retrace_gate.py --only` takes a **regex**, not a comma list; P3a's named sweep
   silently selected 0 cases (`MEASUREMENTS.md:367`, `ID-15`). Spell it
   `--only 'a\|b\|c'`.
10. **LFS fixtures**: a fresh package worktree has `tools/trace_replay/fixtures` as 131/133-byte LFS pointers, and
    retrace against pointers reports `passed 2 / 79` with `ssim=None` — a **false red** that reads exactly like a
    total regression (`MEASUREMENTS.md:413`).
11. **Open questions in `ROADMAP.md` that touch P4a**: 2 (texture-remint pull rate on real corpora — P4a's
    `MOBILEGL_PIPE_TEXEL_RETAIN_MB` decision), 6 (D-B8 named-UBO host payload shape; Magma repacks 331 KB/frame
    on 26.3, Espryt 0), 7 (`MG_Util`'s cut line — server needs SPIRV-Cross, ESSL transpile cache, format
    handlers, POST probes; client needs glslang phase A/B and reflection), 8 (one reflection archive for three
    consumers), 9 (viewport-array replay inside one `draw_vbo`; `EndViewportRoutingPasses` calls
    `InvalidateSyncedRenderState`), 13 (can baked internal shaders express uniform locations and UBO layout
    without a live `ProgramObject`), 14 (which pull-tuned cache hit rates the push model changes).
