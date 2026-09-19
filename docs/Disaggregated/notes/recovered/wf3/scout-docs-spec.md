# P0.5 spec extract — MGPipe design docs (feat/disaggregated @ 6e0e3df3)

Sources, all under `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg/docs/Disaggregated/`:
`README.md` (57 lines), `ARCHITECTURE.md` (601 lines), `ROADMAP.md` (90 lines), `MEASUREMENTS.md` (96 lines).
All line numbers below are lines I actually opened. Docs are in Chinese; requirement wording is quoted verbatim.

---

## 0. Where P0.5 sits

- `README.md:3` (verbatim): "> 状态：**P0 已落地**（`feat/disaggregated@458ccde1`，基线 `dev@81b17c0b`）。下一步 P0.5 → P1 → P2，第 43 天 GO/NO-GO。见 `ROADMAP.md`。"
- `ROADMAP.md:9` (the two tracks, verbatim): "两条跑道：**monolith 跑道** P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止且 monolith 严格好于起点；**IPC 跑道** P5 → P6 → P9 → P10 → P11 → P12。"
- Day budget: `ROADMAP.md:32` "累计（低端）：P0 9 → P0.5 15 → P1 25 → P2 43 …" i.e. P0.5 is days 10–15 on the low estimate; the row itself says **6–9 人天**.
- `ROADMAP.md:3` sets the global framing: total 267–337 人天, two engineers, "真正的约束是两台设备的争用".

---

## 1. The P0.5 ROADMAP row (`ROADMAP.md:16`) — verbatim, field by field

Row header columns are `| 阶段 | 天 | 落地什么 | 验收门 | 依赖 |` (`ROADMAP.md:13`).

**阶段**: "**P0.5** 值头与制品头抽取"
**天**: "6–9"

**落地什么** (verbatim):
> `MG_Pipe/MGPipeValueTypes.h`（`RenderStateParameters`、`SamplerParameters`、`PixelStoreParameters`、`VertexAttribute`… 不 include `MG_State/GLState`）；`MG_State/GLState/ProgramState/ProgramArtifacts.h`（五个反射类型，不 include `ShaderObject.h`/`SpvcSession.h`，更新 7 个 includer）；`Visit()` 归档 + `sizeof` 绊线；CI `-H` include 闭包断言

**验收门** (verbatim):
> 全套测试逐名不变（纯搬移）；两条闭包断言绿且人为加回一个 `MG_State` include 能变红；`nm`/`.text` 变化可逐符号归因

**依赖** (verbatim):
> P0；**P1 与 P7 的硬前置**

Three acceptance clauses decoded for implementers:
1. **Pure move**: every test must still pass *by name* — no behavioural change, no renames of tests, nothing new. The move is mechanical.
2. **Two closure assertions must be green AND must be able to go red**: this is the project-wide discipline "**每个门必须能因它存在的理由变红**" (`ROADMAP.md:7`). Deliberately re-adding one `MG_State` include has to break the gate. Two assertions = one per new header (`MGPipeValueTypes.h` must not reach `MG_State/GLState`; `ProgramArtifacts.h` must not reach `ShaderObject.h`/`SpvcSession.h`).
3. **`nm`/`.text` diffs must be attributable symbol-by-symbol** — a pure header split may move symbols/inlining, but every delta must be explainable. (Contrast P1, `ROADMAP.md:17`, which demands `nm --defined-only` *unchanged*.)

### Other P0.5 mentions in ROADMAP.md

- `ROADMAP.md:17` — P1's 依赖 column is exactly "P0.5".
- `ROADMAP.md:24` — P7's 依赖 column: "P0.5、P2；可与 P5/P6/P8 并行".
- `ROADMAP.md:34` (CTS turnaround, verbatim): "完整 caselist 只在五个架构边界（P0.5、P3a、P4a、P3b/P4b、P13）与每次合并 `dev` 之前跑，放 CI 不放关键路径。" — so P0.5 exit owes a **full `gl44to46` caselist run (~56,271 例)**, in CI, not on the critical path. Per-phase otherwise only named blocks are run.
- `ROADMAP.md:57` (NO-GO branch, verbatim): "**不回滚**：P0/P0.5/P1/P2 的产物（句柄基建与重键、两个头文件抽取、计数器、verify harness、渲染状态 CSO）全是自洽的 monolith 交付物，留在 `dev`" — the two header extractions survive a NO-GO.
- `ROADMAP.md:58` (verbatim): "沉没成本：P0 与 P0.5 无论走哪条路都要花（后者本身是 monolith 净收益）；真正只为 MGPipe 押上的是 P1 + P2 ≈ 28–39 天".
- `ROADMAP.md:80` — open question 7, the `MG_Util` seam; see §5 below.
- P0.5 is **not** on the GO/NO-GO checklist (`ROADMAP.md:42-58`); the 第 43 天 gate is P2's exit.

---

## 2. What ARCHITECTURE.md says about the two headers

### 2.1 The normative paragraph — `ARCHITECTURE.md:260` (§7, Shader state)

Verbatim, in full (this is the single densest requirement in the whole P0.5 scope):

> - **P0.5 硬前置**：反射类型今天声明在 `ProgramObject.h` 里，而它 include `ShaderObject.h`（→ glslang）与 `SpvcSession.h`（→ spirv_reflect）。P0.5 把 `TypeFacts`、`ResourceReflection`、`XfbVarying`、`LinkArtifacts`、`SpirvArtifacts` 抽到 `MG_State/GLState/ProgramState/ProgramArtifacts.h`（只 include `<Includes.h>` 与容器），更新 7 个 includer，加 CI `-H` 闭包断言。同批抽取 `MG_Pipe/MGPipeValueTypes.h`（`MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute`、`VertexBufferBindingPoint`），它不 include `MG_State/GLState` 任何东西；`MGPipeTypes.h` 今天为此临时 include 了 `BackendObject.h` 与 `RenderState.h`（文件头注明为 P0.5 债务）。没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。

Reading the two content lists as the authoritative manifests:

**`MG_State/GLState/ProgramState/ProgramArtifacts.h` — the five reflection types** (`ARCHITECTURE.md:260`):
`TypeFacts`, `ResourceReflection`, `XfbVarying`, `LinkArtifacts`, `SpirvArtifacts`.
Allowed includes: "只 include `<Includes.h>` 与容器" — `<Includes.h>` plus container headers, nothing else. Forbidden by construction: `ShaderObject.h` (pulls glslang) and `SpvcSession.h` (pulls spirv_reflect).
Downstream: "更新 7 个 includer" — 7 files currently including the declarations must be repointed.

**`MG_Pipe/MGPipeValueTypes.h` — nine value types** (`ARCHITECTURE.md:260`; the ROADMAP row `ROADMAP.md:16` names a shorter, non-exhaustive subset with an ellipsis — treat ARCHITECTURE's list as the manifest):
`MAX_DRAW_BUFFERS`, `PerBufferBlendState`, `StencilFaceState`, `PixelStoreParameters`, `RenderStateParameters`, `SamplerParameters`, `BorderColorForm`, `VertexAttribute`, `VertexBufferBindingPoint`.
Constraint: "它不 include `MG_State/GLState` 任何东西".

**Debt being repaid**: "`MGPipeTypes.h` 今天为此临时 include 了 `BackendObject.h` 与 `RenderState.h`（文件头注明为 P0.5 债务）".
Confirmed in tree — `MobileGL/MG_Pipe/MGPipeTypes.h:25-33` carries the note verbatim:
```
// P0.5 DEBT, recorded here so it is impossible to miss: two payloads reach into headers
// this directory is eventually forbidden to see - MGPCaps embeds MG_Backend's
// DynamicBackendParameters, and ResidualValueBlock embeds MG_State's RenderStateParameters
// and PixelStoreParameters. Both are deliberate: the caps block IS that struct (section
// 4.4.1) and the residual block is the migration carrier for Track V (section 6.3). P0.5
// extracts MGPipeValueTypes.h and both includes below go away; until then purity gate A
// (section 10.3) cannot be armed for this header.
#include <MG_Backend/BackendObject.h>
#include <MG_State/GLState/RenderState/RenderState.h>
```
and `MobileGL/MG_Pipe/MGPipeTypes.h:36-40` has the `using` aliases (`MG_Backend::DynamicBackendParameters`, `MobileGL::PixelStoreParameters`, `MobileGL::RenderStateParameters`) with the comment "P0.5 moves them into `MG_Pipe/MGPipeValueTypes.h`".
Note the asymmetry the header records and the docs do not resolve: **`MGPCaps` embeds `MG_Backend::DynamicBackendParameters`** (via `<MG_Backend/BackendObject.h>`), which is *not* one of the nine types listed for `MGPipeValueTypes.h`. `ARCHITECTURE.md:112` confirms the design: "`MGPCaps` = `DynamicBackendParameters`（整块包含，~90 个标量含六个 compute 限制）+ `CallMask` + 两个 blob". Implementers must decide how that include is discharged; the ROADMAP row does not name it.

**Rationale sentence, verbatim** (`ARCHITECTURE.md:260`): "没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。"

### 2.2 Current tree locations of the types (verified by grep, for the mechanical move)

- `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:44` `struct TypeFacts`
- `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:76` `struct ResourceReflection`
- `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:1146` `struct XfbVarying`
- `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:1210` `struct LinkArtifacts`
- `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:1409` `struct SpirvArtifacts`
- `MobileGL/MG_State/GLState/RenderState/RenderState.h:191` `struct PixelStoreParameters`, `:202` `struct PerBufferBlendState`, `:212` `struct StencilFaceState`, `:222` `struct RenderStateParameters`
- `MobileGL/MG_State/GLState/SamplerState/SamplerObject.h:66` `enum class BorderColorForm : Uint8`, `:72` `struct SamplerParameters`
- `MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h:17` `struct VertexAttribute`, `:58` `struct VertexBufferBindingPoint`
- `MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h:106` `static constexpr Uint MAX_DRAW_BUFFERS = 8;` (note: it is a **class-scope constant inside `FramebufferObject`**, not a free constant — moving it is not a plain cut/paste; the doc lists it as part of the value header.)

Files that include `ProgramObject.h` today (candidate "7 个 includer" set; grep over `MobileGL/`, excluding `ProgramObject.h`/`.cpp` themselves):
`MG_Backend/DirectVulkan/Renderer/ProgramFactory.h`, `MG_Backend/DirectVulkan/Renderer/UniformManager.cpp`, `MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp`, `MG_Impl/GLImpl/Program/ProgramInterface.cpp`, `MG_State/GLState/ProgramState/ProgramLinkTask.h`, `MG_State/GLState/ProgramState/ProgramPipelineObject.h`, `MG_State/GLState/ProgramState/ProgramState.h`, `MG_State/GLState/ProgramState/ProgramTranslationCache.h` (8 files; `MG_Pipe/MGPipeTypes.h:297` only mentions the name in a comment, not an include). Verify against the doc's count of 7 when doing the work — three of the eight are the DirectVulkan backend files, which are exactly the ones P7's symbol gate cares about.

### 2.3 The `create_shader_state` reflection archive (§7)

- `ARCHITECTURE.md:258` (verbatim): "`CreateShaderState` 的 payload 是逐 stage SPIR-V + 反射归档（`LinkArtifacts` + `SpirvArtifacts` 全结构体），**不是源码**。"server 从源码重新 link"这条路显式关闭：链接真 `ProgramObject` 就链接 glslang。glslang 全在 client，SPIRV-Cross（`TranspileSpirvToEssl`）全在 server，文件级切割。没有 `MOBILEGL_IPC_PROGRAM` 开关、没有 server 侧 compile pool。"
- `ARCHITECTURE.md:259` — the archive mechanism and its **complete field coverage list**, verbatim:
  > - 归档机制：`Visit()` + `sizeof` 绊线（`static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`），一份字段表服务序列化两个方向。必须覆盖四个 `ResourceReflection`（各带 `TypeFacts`）、`uniformSamplerOrImageUnitIndex`、`uniformBlockBinding`、`shaderStorageBlockBinding`（按名字）、`explicitOpaqueUniformBindings`、`xfbVaryings/xfbStrides/xfbPackedStride/xfbNeedsScatteredCapture`、`computeLocalSize`、GS/TCS/TES 事实、`usesReservedNumSamples`、`uniformOffsets`。`XfbVarying` 带两套拼写（GL 名字 + block 实例/成员/元素）。
- Payload shape: `MGPProgramDesc` is 192 B — "逐 stage SPIR-V blob ×6 + 反射归档 blob + `StageMask`/`GlobalUboSize`/`ReservedNumSamplesOffset` + 四个状态字节，§7" (`ARCHITECTURE.md:134`, the payload table). Confirmed in tree at `MobileGL/MG_Pipe/MGPipeTypes.h:299-310` (`MGPProgramDesc` with `MGPBlobRef Spirv[6]; MGPBlobRef Reflection;`), and the comment at `:296-298` says "P0.5 extracts those types out of ProgramObject.h so a server can deserialize into them without dragging in glslang".
- Size of `MGPProgramDesc` = 192 is also pinned in `MEASUREMENTS.md:86`.
- Related archive consumers: `ShaderStorageBlockBinding` is explicitly **not** ported as a pipe call — "`ShaderStorageBlockBinding`（折进反射归档）" (`ARCHITECTURE.md:102`). Stage dimension is derived by the server *from the reflection archive*: "stage 只在目标 API 需要时由 server 从反射归档推导" (`ARCHITECTURE.md:97`).
- The archive also feeds client-side XFB scatter: "client 拥有目的 shadow 与反射归档里的 varying/stride，原样跑补丁循环" (`ARCHITECTURE.md:310`).
- Emission timing (matters for who may touch these types): "`create_shader_state` 从编译池的终止 continuation 发出（不是从 draw），SPIR-V 在首个用到它的 draw 之前到达 server——monolith 拿不到的异步收益" (`ARCHITECTURE.md:194`).

### 2.4 The `Visit()` + `sizeof` 绊线 idiom

- Definition is `ARCHITECTURE.md:259` (quoted above): one field table serving **both serialization directions**, guarded by `static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`. The tripwire's purpose is that adding a field to the struct without adding it to `Visit()` becomes a compile error.
- The same idiom family elsewhere in the design, for tone/precedent:
  - `ARCHITECTURE.md:116` — "每个 payload 是平坦 POD、显式 padding、`static_assert` 平凡可复制与**精确尺寸**；**永不含指针**。"
  - `ARCHITECTURE.md:359` — `ResidualValueBlock` discipline: "退役是编译错误（`MGL_RESIDUAL_BLOCK_SIZE` 只降不升，`MobileGL/MG_Pipe/MGPipeTypes.h:535`，P13 变成 `static_assert(sizeof == 0)`）；布局逐成员 `offsetof` 断言且 split 下逐字段序列化（异质 POD 并集的 padding 差异 monolith verify 看不见）".
  - `ARCHITECTURE.md:184` — why `RenderStateParameters` moves as one block and why field order is load-bearing: "`RenderStateParameters` 是平凡可复制 POD，Espryt 自己 `static_assert` 并做 head/blend/tail 三段 memcmp，**字段顺序承重**（`ScissorBoxWrittenMask`、`ClipDistanceEnabledMask` 故意放在 tail 段）；拆成 blend/depth-stencil/rasterizer 三个 CSO 要手工维护 ~150 字段划分表且无绊线。" → **Do not reorder fields while moving `RenderStateParameters`.** Espryt's own `static_assert` + three-segment memcmp must keep working; `ARCHITECTURE.md:188` says `SyncRenderState` (693 lines) must not change a line, and P2's gate repeats it (`ROADMAP.md:18`).
  - `RenderStateParameters` is 1168 B (`MEASUREMENTS.md:86`, `ARCHITECTURE.md:188`), inside `ResidualValueBlock` = 1248 B (`ARCHITECTURE.md:134`, `MEASUREMENTS.md:86`).
  - Also note `ARCHITECTURE.md:188`: "Espryt 的 head/blend/tail 划分（驱动侧增量）与 pipeline/dynamic 划分（线上与身份）是两回事，并存、各有绊线。" Two independent partitionings of the same struct both have tripwires.

### 2.5 Purity / include-closure gates (§13.2 item 1, and §16 CI)

`ARCHITECTURE.md:501`, verbatim (the three purity gates — P0.5 is the enabler for gate A, P7 for gate B):

> 1. **接口纯度三道门**（只跑非 verify 构建）：**A 门 include 图**——disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除（`nm --undefined-only` 对"只 include 不调用"是瞎的，而 `RenderState.h → FramebufferObject.h → TextureObject.h` 正是这种耦合），依赖 P0.5；**B 门符号**——`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**C 门未声明**——`grep -c 'pGLContext' MG_Backend/` == 0。外加 debug 断言"每个后端 memo 键都是 `{slot, gen}`，永不是裸前端指针"，由 `HandleRecycleScenario` 支撑（重键前必须在至少一个后端上是红的）。

Key mechanics for P0.5:
- Gate A is an **include-path removal**, not a grep: the disaggregated build of `MG_Backend` drops `MG_State/GLState` from the include search path. The cited coupling chain that makes symbol gates insufficient is `RenderState.h → FramebufferObject.h → TextureObject.h`.
- The three gates "只跑非 verify 构建" and turn green only at P13 (`ARCHITECTURE.md:369`, `ROADMAP.md:30`); the P0.5 deliverable is the `-H` closure assertion, not gate A itself being green.
- CI wiring: `ARCHITECTURE.md:568`, verbatim: "CI（`.github/workflows/test.yml:809` `pipe-gates`，P0 已落地）：`gen_pipe.py` 重生成 + diff；`MG_Backend`/`MG_State` 下禁止 stdio 插桩的 grep 门；`gen_pipe_dirty_surface.py --summary`（信息性，P1 成门）；`check_doc_citations.py`（警告级，文档定稿后 `--strict`）。独立 job `flatc-check`。后续：**`include-graph-check`（P0.5）**、`monolith-symbol-report`。"
  → The new CI job is named **`include-graph-check`**. In tree today: `.github/workflows/test.yml:809` is `pipe-gates:`, `:304` is `flatc-check:`, `:861` runs `check_doc_citations.py … || true` (warning-level, as documented).
- Also relevant, `ARCHITECTURE.md:425`: `WireLog.h` exists precisely so `Transport/` headers don't include the frontend umbrella — "纯度门 A 断言 `-H` 输出" — i.e. gate A's implementation is a compiler `-H` include-trace assertion, the same mechanism P0.5 must produce.
- P13's endpoint for the value header (`ROADMAP.md:30`, verbatim fragment): "`MG_Backend` 的 `MG_State` include 收缩到 `MGPipeValueTypes.h`" — so `MGPipeValueTypes.h` is designed to be the *only* surviving `MG_State`-origin include of the backend.
- Byte-level equalities that survive (context for the `nm`/`.text` attribution clause), `ARCHITECTURE.md:499` verbatim: ""改前改后 `nm --defined-only` 与 `.text` size 完全相等"的门在本方案里按构造死亡（不存在能让旧字节回来的配置）" and `:507`: "符号与 `.text` 漂移每阶段作为信息性指标发布。"

### 2.6 The `MG_Util` split seam — what the server may link vs. client-only

Two statements, both normative:

- `ARCHITECTURE.md:9` (verbatim): "server 进程因此只装 `MG_Backend` + MGPipe 对象表，不链接 `MG_State`、`MG_Impl`、glslang。"
- `ROADMAP.md:80`, open question 7 (verbatim): "**`MG_Util` 的切割缝。** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、格式处理器、POST 探针；client 需要 glslang phase A/B 与反射层。P0.5 解决了 `ProgramObject.h` 一处，`MG_Util` 内部是否有干净的 Transpile-vs-Reflect 缝未审计。"

| side | needs |
|---|---|
| **server** | SPIRV-Cross pass pipeline, ESSL transpile cache, format handlers, POST probes; loader-side pieces implied by `MG_Backend` linking alone |
| **client only** | glslang phase A/B, the reflection layer |

Status: **the seam inside `MG_Util` is explicitly un-audited** ("未审计"). P0.5's charter, per this question, closes exactly one point — `ProgramObject.h`. Implementers should not silently expand P0.5 into a `MG_Util` refactor; the doc treats that as still-open work. But the closure assertions they add are the tool that will later expose it.

Supporting facts:
- File-level cut of the two toolchains: `ARCHITECTURE.md:258` — "glslang 全在 client，SPIRV-Cross（`TranspileSpirvToEssl`）全在 server，文件级切割。"
- Espryt's SPIRV-Cross session and post-emission ESSL rewriting stay untouched on the server side: `ARCHITECTURE.md:316` lists "SPIRV-Cross 会话与 post-emission ESSL 重写、驱动 POST 自检族" among "原样不动的东西".
- The shipped shared library still *contains* glslang because it serves both roles: `ARCHITECTURE.md:537` (verbatim) — "生产 server 主体是 ~30 行 stub：`dlopen(libMobileGL.so)` → `dlsym("mobilegl_server_main")`。一份共享库、两个角色、版本必然匹配（Android 上那份库仍含 glslang/SPIRV-Cross，因为它同时服务 client；B 门检的是 server 侧代码有没有引用它们）。"
- Teardown ordering that assumes glslang is client-side: `ARCHITECTURE.md:520` — "client 排空 compile pool（先于 `glslang::FinalizeProcess()` 与 `pGLContext` 析构）".

### 2.7 The `nm -D libMobileGLServer.so | grep glslang` rule

- Stated as P7's acceptance gate, `ROADMAP.md:24` (verbatim, in P7's 验收门): "**`nm -D libMobileGLServer.so | grep glslang` 为空**".
- Its dependency on P0.5, `ARCHITECTURE.md:260` (verbatim): "没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。"
- The stricter sibling is purity gate B, `ARCHITECTURE.md:501`: "`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空".
- What `libMobileGLServer.so` is: desktop `add_executable` linking `MobileGL_s`; Android `add_executable` renamed `lib*.so` linking shared `MobileGL`, packed by AGP into `jniLibs` (`ARCHITECTURE.md:565`); `mobilegl_server_main` must be `extern "C" __attribute__((visibility("default")))` because shipping builds are RelWithDebInfo with hidden visibility (`ARCHITECTURE.md:530`). Surviving byte-level equality: "`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中" (`ARCHITECTURE.md:507`).
- Practical implication for the archive: because the server must deserialize `LinkArtifacts`/`SpirvArtifacts` **without** any glslang symbol being referenced, the `Visit()` field table and the artifact structs must be glslang-free at the *type* level, not merely at the call level (gate A's whole premise, `ARCHITECTURE.md:501`).

---

## 3. What P1 and P7 need from these two headers (do not paint P1 into a corner)

### 3.1 P1 deliverables (`ROADMAP.md:17`, verbatim)

> `MG_Backend/MGPipe/PipeInputs.h`（Espryt 32 / Magma 55 访问器）；`sed` 293 处 + 58 行非箭头清单逐条转换（显式交付物）；逐 verb 类填充点（G5 表，~93 个边界站点）；逐 verb 世代 poison；G4 影子比对器 + 第三种 CI 模式；20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表

P1 acceptance (verbatim): "pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）；40 trace + 全部集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；故意损坏一个快照字段能让 verify 变红；故意在 `glGenerateMipmap` 的填充表漏一个字段能在**那条 verb** 上触发 poison Fatal"

### 3.2 The `PipeInputs` / `MGB_CTX` shape P0.5 must not break (`ARCHITECTURE.md:325-340`, verbatim code block)

```cpp
// MG_Backend/MGPipe/PipeInputs.h —— 按 memo 键组织，不按读点组织（~20 KB，字段集全迁移期稳定）
struct PipeInputs {
    const RenderStateParameters& GetRenderStateParameters() const;   // 阶段 A：类型与后端今天读到的完全一致
    // … 每个后端真正用到的 GLContext 方法一个访问器（Espryt 32 / Magma 55）
#if MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED
    Uint64 m_filledGen[kFieldCount];   // 逐字段"上次填充的 verb 序号"
    Uint64 m_currentVerbSerial;
#endif
};
#if MOBILEGL_PIPE_PUSH
#  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#else
#  define MGB_CTX (::MG_State::pGLContext)
#endif
```

**Hard constraints this puts on P0.5:**
1. **`PipeInputs` accessors return the exact types the backend reads today** — "阶段 A：类型与后端今天读到的完全一致" (`ARCHITECTURE.md:328`). `GetRenderStateParameters()` returns `const RenderStateParameters&`. So the moved `RenderStateParameters` must remain **the same type**, same namespace-visible name, same layout, same field order — a `using` alias or an identical-but-different type would break the mechanical `sed` and the `.text` attribution. Same for `PixelStoreParameters`, `SamplerParameters`, `VertexAttribute`, `VertexBufferBindingPoint`, etc.
2. **`MGB_CTX` is a macro over `pGLContext` in the pull arm** — P1's phase A is a mechanical `sed` of `MG_State::pGLContext->` → `MGB_CTX->` at 293 sites plus 58 non-arrow lines (`ARCHITECTURE.md:344`). If P0.5 changes any of those spellings or the reachability of the value types from `MG_Backend`, the 293-site `sed` count and the "非箭头清单" become stale. **P0.5 must be a pure move that leaves every `MG_Backend` read site textually identical.**
3. **The pull arm still includes `MG_State`** during P1–P12 — gate A is only armed later. So `MGPipeValueTypes.h` must be includable from *both* arms and must not force `MG_Backend` to stop seeing `MG_State` types yet.
4. **Field-id table**: G5 generates `PipeInputs` field ids — 61 fields (`ARCHITECTURE.md:79`, `MEASUREMENTS.md:82`: "71 条调用（11 screen / 60 context）、63 个 verify payload、61 个 `PipeInputs` 字段"). Poison is a **per-verb generation**, not a bitmap (`ARCHITECTURE.md:349`): "每次 verb 递增 `m_currentVerbSerial`，字段被填时记下序号，读取时断言相等（跨 verb 有效的字段显式标 sticky）… 读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}`。纯度门 grep 的是 `pGLContext` 不是 `pGLContext->`."
   → Purity gate C greps the bare token `pGLContext`. Any comment or identifier P0.5 leaves in `MG_Backend/` containing that token counts against gate C later.
5. **P1's verify harness (G4) compares field-by-field, never `memcmp`** — `ARCHITECTURE.md:502`: "逐字段而非 `memcmp`（padding 会 false-DIFFER）". So the moved structs' padding is not required to be identical to anything, but their **field list must be enumerable** by the `PipeFields.def` table (`ARCHITECTURE.md:78`: "字段表来自 `PipeFields.def`；浮点按位比较，NaN patch level 不会误报"). If P0.5 adds/removes/reorders a field, `PipeFields.def` and G4 must move with it — another reason for a pure move.
6. **Track V is 55% of read points and rides `ResidualValueBlock`** (`ARCHITECTURE.md:353,359`), whose payload is `{RenderStateParameters, PixelStoreParameters, CapabilityBits, patch 三字段}` — i.e. it is built out of exactly the types P0.5 extracts, with per-member `offsetof` assertions and per-field serialization under split. Keep `offsetof` computability (standard-layout, no virtuals, no private/public interleaving that would break it).
7. **`MOBILEGL_PIPE_VERIFY` survives P13** together with `SnapshotFromGLContext()` and its `MG_State` includes (`ARCHITECTURE.md:369`, D-B5: "**保留 `MOBILEGL_PIPE_VERIFY` 连同它需要的 `SnapshotFromGLContext()` 与 `MG_State` include**"). So `MGPipeValueTypes.h` must coexist with `MG_State` headers being included in the verify build — no `#error`-style mutual exclusion.

### 3.3 What P7 needs (`ROADMAP.md:24`, `ARCHITECTURE.md:321,263,142`)

- P7 deliverables (verbatim): "§5.5 其余 10 个子系统（子系统 1、4 已在 P2）：`SetupDrawSnapshot` 探测字段塌成 dirty mask；占位纹理原生化（~120 行删除）；具名 UBO host payload（D-B8，`kCapNeedsHostUboBytes`）；blit/depth-mipmap 内部 shader 烘焙 + 新鲜度测试；`VertexInputStateFactory` 裸指针写回删除；D18 容器纪律原样保留"
- The gate that depends on P0.5: "**`nm -D libMobileGLServer.so | grep glslang` 为空**" (§2.7 above).
- Magma reads the reflection archive with SPIRV-Reflect and is one of the three archive consumers (see open question 8, §5 below).
- Baked internal shaders (`ARCHITECTURE.md:263`, verbatim): "Magma 的两个内部 shader（blit、depth-mipmap）烘焙成签进树的 SPIR-V + uniform location + UBO 布局，用一个 `MG_Test` 重跑树内 glslang 逐字节比对守新鲜度（`MOBILEGL_BAKED_INTERNAL_SHADERS`，P7）；顺带把一次 glslang 编译从 monolith 启动路径上删掉。" — this is the other half of getting glslang out of the server; P0.5's header split alone does not do it, and open question 13 (`ROADMAP.md:86`) flags "烘焙后的内部 shader 能否在没有活 `ProgramObject` 的情况下表达 uniform location 与 UBO 布局。**未做原型**。" That question bears directly on what `ProgramArtifacts.h` must be able to express standalone.
- Server-side lazy specialization (`ARCHITECTURE.md:142`, D-B2): the backend program still needs 8 extra inputs beyond the archive (draw-FBO snorm/unorm clamp mask, fragColor broadcast count, storage-block binding signature, atomic counter set, live image formats, patch params; Magma adds default-FB height for FragCoord-Y-flip and XFB layout) — those are *pushed state*, not archive fields, so `ProgramArtifacts.h` must not try to absorb them.

---

## 4. Commit / CI discipline — ROADMAP §"通用纪律" (`ROADMAP.md:5-7`)

Verbatim, in full:

> ## 通用纪律（每个 commit）
>
> 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议；每阶段出口跑一次五部分门；每阶段性能判据是**逐线程 CPU 时间**。

Unpacked, with the backing references:
- **默认 ALL target 必须完整构建** — every commit builds the whole default ALL target.
- **禁止提交热路径插桩（CI grep 门）** — the existing gate is in `pipe-gates`: "`MG_Backend`/`MG_State` 下禁止 stdio 插桩的 grep 门" (`ARCHITECTURE.md:568`).
- **每个门必须能因它存在的理由变红** — every gate must be demonstrably falsifiable. This is exactly what the P0.5 row demands ("人为加回一个 `MG_State` include 能变红", `ROADMAP.md:16`) and it recurs at P1 ("故意损坏一个快照字段"), P2 ("拿掉一个字段能红"), P4a ("落地前必须红"), P8, P9.
- **Windows 机器不是正确性门** — repeated at `ARCHITECTURE.md:545`.
- **设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议** — see `ARCHITECTURE.md:504` for the full method (two devices, `tools/bench.sh` + trace replay `--benchmark`, per-frame JSON, p50 and p99).
- **每阶段出口跑一次五部分门** — the five-part gate is `ARCHITECTURE.md:497-505` (§13.2): ① three purity gates, ② `MOBILEGL_PIPE_VERIFY=1` shadow comparison, ③ behavioural A/B (40 traces SSIM ≥ 0.99, `ctest -L integration-gpu` name-for-name, unit tests, CTS within 0.5 pp with rate = Pass/(Pass+Fail), NS not in the denominator), ④ monolith performance non-regression (per-thread CPU time, p50/p99, absolute ns cap on the tracker, Blaze3D blend-toggle microbenchmark, CSO-content-addressing-off negative control), ⑤ coverage + poison + handle discipline (G6 0 UNMAPPED, `gen_pipe_dirty_surface.py` 0 unmapped mutators, per-verb poison, G7 setter consistency, `ResidualValueBlock` `offsetof` asserts).
- **每阶段性能判据是逐线程 CPU 时间** — restated at `ARCHITECTURE.md:381` ("monolith 论据是逐线程 CPU 数字（§13.2-④），不是删除行数") and `ARCHITECTURE.md:519`.
- CTS turnaround rule: `ROADMAP.md:34` — P0.5 is one of the five architecture boundaries owing a full `gl44to46` caselist (~56,271 cases), run in CI, off the critical path.
- Repo-wide commit-message style (from project memory, not these docs): `[Type] (Scope): description`, single line, never add `Co-Authored-By`.

---

## 5. Open questions in ROADMAP that touch these headers

All from `ROADMAP.md:70-90` ("仍然开放的问题"). `ROADMAP.md:72` notes what P0 already answered and is no longer listed.

**#7 — `MG_Util` 的切割缝** (`ROADMAP.md:80`, verbatim):
> **`MG_Util` 的切割缝。** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、格式处理器、POST 探针；client 需要 glslang phase A/B 与反射层。P0.5 解决了 `ProgramObject.h` 一处，`MG_Util` 内部是否有干净的 Transpile-vs-Reflect 缝未审计。

**#8 — one reflection archive serving three consumers** (`ROADMAP.md:81`, verbatim):
> **一份反射归档能否服务三个消费者**（Espryt 读前端表、Magma 跑 SPIRV-Reflect、`DirectVulkan.cpp` 为 `glGetProgramResource*` 又反射一遍）。

→ Direct bearing on `ProgramArtifacts.h`'s field set: the three consumers are (a) Espryt reading the frontend tables, (b) Magma running SPIRV-Reflect, (c) `DirectVulkan.cpp` reflecting a second time to answer `glGetProgramResource*`. The archive coverage list at `ARCHITECTURE.md:259` is the current best answer; whether it is sufficient for all three is **not settled**. Implementers should extract the types without narrowing them, and should not assume the field list is final.

**#5 — `FramebufferSrgb` / `DepthClamp` storage** (`ROADMAP.md:78`, verbatim):
> **`FramebufferSrgb` / `DepthClamp` 的拍板。** 事实已清（无存储、`glEnable` 静默吞掉、六个读点恒 false、41 个 fixture 无一开启）；建议在 chunk 表冻结前补真存储并把 `FramebufferSrgb` 划进 pipeline 半边。由计划所有者拍板，**拍板前不冻结 chunk 表**。

Supporting facts:
- `ARCHITECTURE.md:190` (verbatim): "`FramebufferSrgb` 与 `DepthClamp` 今天**没有存储**（`glEnable` 被静默吞掉且不报错，六个后端读点恒为 false）；chunk 表冻结前要补真存储并把 `FramebufferSrgb` 划进 pipeline 半边（它改变 attachment/blend 的解释）——待拍板，见 `ROADMAP.md`。"
- `MEASUREMENTS.md:84` (verbatim): "**`FramebufferSrgb` / `DepthClamp`**：`FramebufferSrgb` 的六个后端读点全部消费一个编译期常量 `false`，`DepthClamp` 零读点；两者的 `glEnable` 落到 `RenderState.cpp` 的 `default:` 分支既不存储也不报 `GL_INVALID_ENUM`；41 个 fixture 无一开启任一项（补真存储不会改动任何既有 fixture 的输出）。"
- The work itself is scheduled in **P2**, not P0.5: `ROADMAP.md:18` ends its deliverable list with "补 `FramebufferSrgb`/`DepthClamp` 存储".
- **Impact on P0.5**: adding real storage means **adding fields to `RenderStateParameters`** — the very struct P0.5 moves, whose field order is load-bearing (`ARCHITECTURE.md:184`) and whose size (1168 B) is pinned by tripwires. P0.5 must move it *as-is* and leave the field addition to P2; but implementers should make the `sizeof`/`offsetof` tripwires easy to update in one place, since a P2 decision will change those numbers. Note also the chunk table (`MG_Pipe/MGPipeRenderStateSpans.{h,cpp}`, P2) must not be frozen before this ruling.

**#4 — wire granularity of render state** (`ROADMAP.md:77`, verbatim): "**渲染状态的 wire 粒度。** chunk 划分定下来后，CSO LRU 容量（暂定 64）与 `set_dynamic_state` 的 chunk 粒度由计数器定。" — same struct, same "don't freeze yet" posture.

**#6 — named-UBO host payload shape (D-B8)** (`ROADMAP.md:79`): first real number is in hand (Magma repacks 331 KB named-UBO bytes per frame in the 26.3 world, Espryt 0; `MEASUREMENTS.md:57,65`). Touches `MGPShaderBuffers`, not the two P0.5 headers directly, but the reflection archive's `uniformBlockBinding`/`shaderStorageBlockBinding` fields are what the server resolves against.

**#13 — baked internal shaders without a live `ProgramObject`** (`ROADMAP.md:86`, verbatim): "**烘焙后的内部 shader 能否在没有活 `ProgramObject` 的情况下表达 uniform location 与 UBO 布局。** 未做原型。" — a direct question about whether `ProgramArtifacts.h`'s types are expressive enough standalone.

**#12** (`ROADMAP.md:85`) and **#14** (`ROADMAP.md:87`) are post-P13 concerns, no header impact.

---

## 6. Quick checklist for the P0.5 implementer

1. Create `MobileGL/MG_Pipe/MGPipeValueTypes.h` with the nine items from `ARCHITECTURE.md:260`; includes limited to `<Includes.h>` + containers; **no** `MG_State/GLState` include. Watch `MAX_DRAW_BUFFERS` (class-scope constant at `FramebufferObject.h:106`).
2. Create `MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h` with `TypeFacts`, `ResourceReflection`, `XfbVarying`, `LinkArtifacts`, `SpirvArtifacts`; includes limited to `<Includes.h>` + containers; **no** `ShaderObject.h`, **no** `SpvcSession.h`. Repoint the includers (doc says 7; grep finds 8 candidates, listed in §2.2).
3. Add the `Visit()` field table (one table, both serialization directions) covering the full list at `ARCHITECTURE.md:259`, plus `static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)` and the sibling for `SpirvArtifacts`.
4. Delete the two P0.5-debt includes at `MobileGL/MG_Pipe/MGPipeTypes.h:32-33` and the debt comment at `:25-31`; resolve the `MGPCaps`/`DynamicBackendParameters` include separately (it is not one of the nine value types — see §2.1).
5. Add CI job **`include-graph-check`** with two `-H` closure assertions; prove each can go red by re-adding one forbidden include.
6. Do not reorder or add fields anywhere. Pure move. Tests unchanged by name. `nm`/`.text` deltas attributable per symbol.
7. Run the full `gl44to46` caselist in CI (architecture boundary, `ROADMAP.md:34`), and the five-part gate once at phase exit (`ROADMAP.md:7`).
