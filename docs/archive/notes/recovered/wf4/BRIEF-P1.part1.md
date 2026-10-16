# P1 implementation brief — `PipeInputs` strangler, per-verb fill points, poison generations, `MOBILEGL_PIPE_VERIFY` comparator + third CI mode

Tree: `feat/disaggregated @ 087685d1` (worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, clean except untracked `.trace-work/`). WSL: base tree `~/w7/pipe` and five package worktrees `~/w7/p1-<slug>` on branches `p1/<slug>`, all created **from the contract commit C0** (section B.1) that the integrator lands first; every tree gets a configured `build-linux/` exactly as in the P0.5 brief (Release, `/usr/sbin/clang++`, ccache, `MOBILEGL_BUILD_TEST=ON`, `MOBILEGL_BUILD_INTEGRATION_TEST=ON`, `MOBILEGL_BUILD_DISAGGREGATED=OFF`, log level INFO) — that is the **pull build**; the core package additionally configures `build-verify/` (section C.1).

Every `file:line` below was re-opened at `087685d1`. Where a scout was wrong or incomplete the text says **[correction]** and the corrected fact is what the packages follow.

Corrections to the scouts, in one place:
1. **Boundary catalogue is incomplete.** `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp:1329` aliases the table (`auto& backendGL = MG_Backend::gBackendFunctionsTable.GL;`) and lines `1330-1334` invoke `FenceSync`/`ClientWaitSync`/`DeleteSync` through it; the "91 lines" count misses them. The complete boundary is **96 lines / 87 invocations** (table in B.9). There is **no** `pActiveBackendObject->` path from `MG_Impl` into a pull site: the only virtuals `MG_Impl` calls are `GetDynamicParameters` (35), `GetFormatCapabilities` (4), `GetBackendType` (1). `Present`/`SetSwapInterval` are not invoked from `MG_Impl` at all (`MG_Backend/Init.cpp:44` copies the table; the EGL path calls the backend's own virtuals), and neither reads a field (`DirectGLES.cpp:10550`, catalogue "no pulls").
2. **`ShaderStorageBlockBinding` does not call `SyncNeccessaryTextures`/`SyncRenderState`** (scout-pull-sites §4.2). `DirectGLES.cpp:7355-7372` reads only `ValidateProgramName`/`GetProgramObject`; the two sync calls at `:7376/:7378` belong to `ClearBufferfi` (`:7374`). It is a `Passive` verb (B.8).
3. **Three lines outside the 58 need hand edits**: `DirectGLES.cpp:144` (`static const MG_State::GLState::GLContext* g_fbSlotCacheContext`) and `:150` (`&ctx->GetFramebufferBindingSlot(...)` — an arrow through a local `GLContext*`, invisible to every `pGLContext` grep) change with `:147`; B.11 gives the exact text.
4. **Counts in the docs vs the tree**: `sed` sites 277 (not 293), on 274 lines; non-arrow lines 58 (exact); distinct accessors 62 (+ one `.get()`), Espryt 32 / **Magma 56** (doc: 55); `PipeFilled.inc` field set 61 → **62** (drop `GetBoundTransformFeedbackName`, add `GetBoundTransformFeedbackLifetimeId` + `HasOpenTransformFeedbackSpan`, both read at `VulkanRenderer.cpp:11219/:11236`, declared `Core.h:441/:450`). `SyncPersistentMappedRange` 20 / `SyncGpuWrites` 6: exact.
5. **`MOBILEGL_DEBUG` does not exist** (only `ARCHITECTURE.md:330` spells it). The repo's debug gate is `MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG` (`Defines.h:104-115`). D9 below fixes the poison gate.
6. **The dirty-surface mapping gate is P2**, not P1 (`ROADMAP.md:18` P2 row; `ARCHITECTURE.md:172,568` and `test.yml:871-874` say P1). The ROADMAP table cell is the authority for what P1 delivers; P1 leaves `gen_pipe_dirty_surface.py --summary` informational and the integrator corrects the three P1 mentions (D.6).
7. **The vendored inventory is not edited.** The dead accessor is retired through a new `MGP_COVERAGE_RETIRED_LIST` (D14) so G6 still maps its 477 rows; a live-tree cross-check in `gen_pipe.py` (D15) makes the field set follow the code from now on.
8. `PipeCatalogueTest.cpp:225` pins 61 → 62; `:213` (477) is unchanged.

---

## A. Goal and acceptance gate (verbatim, then decoded)

`docs/Disaggregated/ROADMAP.md:17`:

> **P1** `PipeInputs` 替换与 verify harness | 10–13 | `MG_Backend/MGPipe/PipeInputs.h`（Espryt 32 / Magma 55 访问器）；`sed` 293 处 + 58 行非箭头清单逐条转换（显式交付物）；逐 verb 类填充点（G5 表，~93 个边界站点）；逐 verb 世代 poison；G4 影子比对器 + 第三种 CI 模式；20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表 | pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）；40 trace + 全部集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；故意损坏一个快照字段能让 verify 变红；故意在 `glGenerateMipmap` 的填充表漏一个字段能在**那条 verb** 上触发 poison Fatal | P0.5

Supporting text every package must know: the strangler sketch and the three-phase table `ARCHITECTURE.md:323-351` (§9.2; P1 = phase A + the scaffolding for phase B); gate 13.2-② `ARCHITECTURE.md:501` (field-wise, never memcmp, first differing field + draw serial, ~5–10× slower, never shipped, survives P13); the P13 retention rule `ARCHITECTURE.md:369` (delete `MGB_CTX`/`MOBILEGL_PIPE_PUSH`/the non-verify branch of `SnapshotFromGLContext()`, keep `MOBILEGL_PIPE_VERIFY` with its `MG_State` include); purity gate C `ARCHITECTURE.md:500` greps the bare token `pGLContext` under `MG_Backend/`; the stale-index discipline table `ARCHITECTURE.md:233-238` and open question #15 `ROADMAP.md:88` (the `*IndirectCount` rows are recorded as they are, **not fixed**); `ROADMAP.md:7` — every gate must be able to go red for the reason it exists; Windows is not a correctness gate.

Decoded into checkable statements (the integrator ticks each in D.5):

| # | statement | how it is checked |
|---|---|---|
| G1 | The **pull build** (both new CMake options OFF, Release/INFO, LTO off) has the same `nm --defined-only` symbol set as the P0.5 baseline `087685d1`. | `scripts/symbol_report.py --json` → `added == [] and removed == []`. |
| G2 | Every `.text` byte delta in that build is attributed to a named line of the 58-line conversion list (or to the three extra lines in correction 3). Empty-guard / ternary **rewrites** are not done in P1 — they are converted to a spelling with identical codegen (D4) and rewritten in P2. | `docs/Disaggregated/P1-symbol-attribution.md`: one row per resized symbol with the line that caused it. Expected content: the five inliners of `GetFramebufferBindingSlotFast` (`DirectGLES.cpp:1611,1905,2743,2858,2933` — the `:147/:150` change) and nothing else. |
| G3 | On the **verify build**, all 40 traces (39 CI cases × 2 backends − 1 = 77 matrix entries; `rd12-odinlite` is `ci:false`, `iterationrp` is Magma-only) and every integration scenario (371 cases × 2 backends) run with `MOBILEGL_PIPE_VERIFY=1` and report zero divergence: no `Fatal{PipeVerifyDiffer` and no `Fatal{UnmigratedPipeInput` in any log, every case green, and the library's arming line present in every log. | `ctest -L integration-verify` (742 entries + the arming/negative lanes) and the `retrace-verify` matrix run with `verify_retrace=full`; `run_trace_case.cmake` fails a case whose `mobilegl.log` lacks `MGPipe: verify armed` (so a pull library under the lane cannot pass). |
| G4 | Negative control A: `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` makes a verify run red, naming that field. | Unit test (`PipeInputsTest.CorruptedSnapshotFieldIsNamed`), integration lane `*.VerifyCorrupted.*`, and an inverted CI step on one short scenario and one short trace. |
| G5 | Negative control B: omitting one field from `GenerateMipmap`'s fill set raises `Fatal{UnmigratedPipeInput, "GetTextureUnitObject@GenerateMipmap"}` **on that verb** (the process aborts inside `glGenerateMipmap`, not at a later draw). | Unit test (`PipeInputsTest.OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale`, fork/`waitpid` `...AbortsNamingTheVerb`), integration lane `*.PoisonOmission.*` (fork + `glGenerateMipmap`, both backends), and the literal control: delete the `X(TextureOp, GetTextureUnitObject)` row from `FillPoints.def`, regenerate, run — red on `GenerateMipmap`. |
| G6 | The 20 + 6 sync sites are tabulated per site with their §5.7 reconcile row. | `docs/Disaggregated/SYNC_SITES.md` (B.12), lint-clean under `check_doc_citations.py`. |
| G7 | Existing tests: no name removed (`ctest -N` diff vs baseline is additions-only), `ctest -L unit` and `ctest -L integration-gpu` green on the pull build, `gen_pipe.py --check` clean, `check_include_closure.py --require-all` green. | D.5. |

---

## B. Fixed design decisions (every package follows these; none is re-decided inside a package)

### B.1 The contract commit C0 (integrator, before any package branches)

Two new headers, nothing else. Both are owned by `p1-core` afterwards; the other four packages only include them.

**`MobileGL/MG_Pipe/PipeStrangler.h`** (exact content; banner as `MGPipe.h:1-7` with the path adjusted):

```cpp
#pragma once
#ifndef MOBILEGL_MG_PIPE_STRANGLER_H
#define MOBILEGL_MG_PIPE_STRANGLER_H
// The P1 strangler alias (ARCHITECTURE.md section 9.2, phase A). Every backend read of
// frontend state is spelled MGB_CTX-> from P1 on. In the PULL build the three macros expand
// to exactly the tokens the backends used before P1, so the preprocessed token stream - and
// therefore .text - is unchanged. In the PUSH build (-DMOBILEGL_PIPE_PUSH=1) they resolve to
// the pushed PipeInputs block. This header deliberately lives OUTSIDE MG_Backend/ so that
// purity gate C (`grep -c pGLContext MG_Backend/ == 0`, ARCHITECTURE.md:500) is a statement
// about backend code, not about the alias; P13 deletes this file.
//
// MGB_CTX_PRESENT replaces the seven empty guards, the four ternary conditions and the
// six `!= nullptr` / `== nullptr` tests of the 58-line list: in the pull arm it is the
// same null test those lines performed (unique_ptr::operator bool IS `get() != nullptr`),
// in the push arm the block is always there. P2 removes the guards; P1 only respells them.
// MGB_CTX_IDENTITY replaces the one `.get()` capture (DirectGLES.cpp:147): an opaque
// identity that changes when the frontend context is recreated.
#if MOBILEGL_PIPE_PUSH
#include <MG_Backend/MGPipe/PipeInputs.h>
#define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#define MGB_CTX_PRESENT (true)
#define MGB_CTX_IDENTITY (::MobileGL::MG_Pipe::gPipeInputs.FrontendIdentity())
#else
#define MGB_CTX (::MobileGL::MG_State::pGLContext)
#define MGB_CTX_PRESENT (::MobileGL::MG_State::pGLContext != nullptr)
#define MGB_CTX_IDENTITY (static_cast<const void*>(::MobileGL::MG_State::pGLContext.get()))
#endif
#endif
```

The pull arm needs no include: every backend TU that spells `MGB_CTX` already includes what declares `MobileGL::MG_State::pGLContext` (`Core.h:596`, `extern UniquePtr<GLState::GLContext>& pGLContext;`). The push arm's `MG_Pipe → MG_Backend` include is a deliberate, documented layering exception that dies with the file at P13.

**`MobileGL/MG_Impl/Pipe/PipeFill.h`** (stub; core replaces the body, the macro name and the pull no-op are the contract):

```cpp
#pragma once
// P1 fill points (ARCHITECTURE.md:348). One MGP_FILL_FOR(<Verb>) immediately before each
// backend-table invocation in MG_Impl. Pull build: nothing. Push build: an RAII verb scope
// that bumps the poison serial, fills gPipeInputs for the verb's class, runs the verify
// comparator when armed, and closes the verb on scope exit.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
namespace MobileGL::MG_Pipe {
    class MGPipeVerbScope {
    public:
        explicit MGPipeVerbScope(MGPipeVerb verb);
        ~MGPipeVerbScope();
        MGPipeVerbScope(const MGPipeVerbScope&) = delete;
        MGPipeVerbScope& operator=(const MGPipeVerbScope&) = delete;
    };
}
#define MGP_PP_CAT2(a, b) a##b
#define MGP_PP_CAT(a, b) MGP_PP_CAT2(a, b)
#define MGP_FILL_FOR(Verb) \
    ::MobileGL::MG_Pipe::MGPipeVerbScope MGP_PP_CAT(mgpVerbScope_, __LINE__)(::MobileGL::MG_Pipe::MGPipeVerb::Verb)
#else
#define MGP_FILL_FOR(Verb) ((void)0)
#endif
```

`MGPipeVerb` is the generated verb enum (`generated/PipeFillPoints.inc`, D12), so in the stub state `MGP_FILL_FOR` only compiles in the pull arm — which is all the fill-points package can build anyway. Commit message for C0: `[Feat] (Pipe): add the P1 strangler contract headers - MGB_CTX/MGB_CTX_PRESENT/MGB_CTX_IDENTITY in MG_Pipe/PipeStrangler.h and the MGP_FILL_FOR no-op in MG_Impl/Pipe/PipeFill.h`.

### B.2 Decisions

| # | decision | why / evidence |
|---|---|---|
| D1 | **Two CMake options**, both default OFF: `MOBILEGL_PIPE_PUSH` (→ `-DMOBILEGL_PIPE_PUSH=1`, selects the alias arm) and `MOBILEGL_PIPE_VERIFY` (→ `-DMOBILEGL_PIPE_VERIFY=1`; **forces `MOBILEGL_PIPE_PUSH` ON** with a `message(STATUS)`). The runtime `MG_Config::Features.PipePush` bitmap (`Config.h:326`) is **not consumed in P1** (a P1 push build pushes every field of the verb's set at every fill point); the runtime `Features.PipeVerify` (`Config.h:332`, env `MOBILEGL_PIPE_VERIFY`) **arms** the comparators at run time inside a verify build and does nothing in any other build. | Resolves scout R3: the `#if` in `ARCHITECTURE.md:334` is the CMake macro; the bitmap is phase B's per-field yield (P2). Verify without push is meaningless: the poison lives in the accessors, which only exist in the push arm. |
| D2 | **Namespaces and paths.** `struct PipeInputs` and `gPipeInputs` live in `namespace MobileGL::MG_Pipe` (the doc's spelling `::MobileGL::MG_Pipe::gPipeInputs`, `ARCHITECTURE.md:331`) and are declared in `MobileGL/MG_Backend/MGPipe/PipeInputs.h` (the doc's path, `:325,:553`), defined in `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp`. The frontend-facing half (`SnapshotFromGLContext`, the fillers, `MGPipeVerbScope`, knob parsing, init) lives in `MobileGL/MG_Impl/Pipe/PipeFill.{h,cpp}` — the directory P2's tracker joins (`ROADMAP.md:18` `MG_Impl/Pipe/Tracker`). `MGPipeImpl_DirectGLES/DirectVulkan.cpp` from the layout table `ARCHITECTURE.md:553` are **not created in P1** (they are the server-side table implementations, P5+); the integrator corrects the table cell to `PipeInputs.h + PipeInputs.cpp [P1+]`. | Gate C: nothing under `MG_Backend/` spells `pGLContext` after P1 (the pull arm's token lives in `MG_Pipe/PipeStrangler.h`, the filler in `MG_Impl/`). Gate A at P13 keeps `MG_State` includes for verify builds only. |
| D3 | **Pull build is byte-for-byte the P0.5 library.** Every new `.cpp` (`PipeInputs.cpp`, `PipeFill.cpp`) is wrapped whole in `#if MOBILEGL_PIPE_PUSH … #endif` (banner outside, everything else inside — no includes reachable in the pull arm); `MGP_FILL_FOR` is `((void)0)`; the three config knobs (D18) are declared and parsed under `#if MOBILEGL_PIPE_PUSH`; the `Init.cpp` hook is `#if MOBILEGL_PIPE_PUSH`; `PipeStrangler.h` is macros only. | G1/G2. |
| D4 | **The 58 non-arrow lines** convert as: 43 `MOBILEGL_ASSERT(… pGLContext …)` lines **deleted** (34 `DirectVulkan.cpp`, 9 `UniformManager.cpp`; no-ops in INFO builds, `Defines.h:113-115`; under push the block cannot be null); 5 bare `if (MG_State::pGLContext)` and 2 `&& MG_State::pGLContext` compounds → `MGB_CTX_PRESENT`; 4 ternary conditions (`Managers.cpp:7192/7200/7203`, `DirectVulkan.cpp:1284`) → `MGB_CTX_PRESENT ? MGB_CTX->X() : default`; `!= nullptr` at `DirectVulkan.cpp:1236`, `VulkanRenderer.cpp:12766` → `MGB_CTX_PRESENT`; `== nullptr` at `VulkanRenderer.cpp:11267` → `!MGB_CTX_PRESENT`; the `.get()` at `DirectGLES.cpp:147` → `MGB_CTX_IDENTITY` (with `:144`/`:150`, B.11); the comment `VertexInputStateFactory.h:133` reworded. Exact per-line text in B.11. | `ARCHITECTURE.md:344` (~34 assert deletions, 7 guards, 3 ternaries, `.get()`+`decltype`, 14 `!= nullptr`, 1 comment; the 9 UniformManager asserts are the doc's `!= nullptr` overlap). Pull codegen identical: `unique_ptr::operator bool` is `get() != nullptr`. |
| D5 | **Every accessor keeps the exact `GLContext` signature** (name, parameters, return type, const-ness — `Core.h` lines in B.4). `MGB_CTX->X(args)` is therefore the only edit at 277 sites (`sed 's/MG_State::pGLContext->/MGB_CTX->/g'`), and `GetRenderStateParameters()` returns `const RenderStateParameters&` so `SyncRenderState` is untouched (GO/NO-GO item 2, `ROADMAP.md:47`). | `ARCHITECTURE.md:328` "阶段 A：类型与后端今天读到的完全一致". |
| D6 | **Storage kinds** (B.4 has the per-field assignment). (V) *value copy*: the accessor's result is copied into a `PipeInputs` member at fill and returned from the copy; the 1168-byte `RenderStateParameters` copy is the only memoised one — copied only when `GetRenderStateParametersVersion()` changed since the last fill (Espryt's own early-out trusts that version, `DirectGLES.cpp:2026`), stamped every time. (H) *shared-pointer copy*: `SharedPtr` copied at fill (`GetBoundVertexArray`, `GetProgramForDraw`, `GetProgramForDispatch`, `GetTransformFeedbackProgram`). (R) *reference capture*: for the five accessors returning a mutable reference into a frontend container, `PipeInputs` stores the container base pointer(s) captured at fill and the accessor indexes them, returning the same live reference the backend reads today (`GetBufferBindingSlot` — one pointer per `GlobalBufferTargets` entry; `GetBufferBindingPoint` — one base pointer per `BufferBindPointTargets` entry into the contiguous `Array<BindingSlotRange1D<BufferObject>, 84>` (`BufferState.h:73`); `GetFramebufferBindingSlot` — two pointers; `GetTextureUnitObject` — base of `Array<TextureUnit, 192>` (`TextureState.h:128`); `GetImageTextureBinding` — base of `Array<ImageTextureBinding, 192>` (`:129`)), plus `GetCurrentVertexAttribute` (base of the 32-entry `Array<CurrentVertexAttributeValue, 32>`, `Core.h:513`, replicating the out-of-range branch at `Core.cpp:246-250`). (P) *pass-through*: six accessors that are not pipe data forward to the frontend through a resolver (D8). **No accessor derives its value from another field** (no re-implementation of `RenderState` logic inside `PipeInputs`): the cost is ~1 KB of copies per verb in the push build, which P2's tracker replaces. | Type identity for references cannot be met by a copy (`GetImageTextureBinding` returns `ImageTextureBinding&`; the backend only reads through it — `DirectGLES.cpp:1728-1745`, `UniformManager.cpp:1016-1030`, `VulkanRenderer.cpp:9214-9224` verified — but a copy would still break `.Bind()` semantics if ever used). Derivation would duplicate `RenderState.cpp` rules (`IsCapabilityEnabledIndexed` at `RenderState.cpp:471-490` has three branches); copying the accessor result cannot be wrong. Size: `sizeof(PipeInputs) < 4 KiB` (`static_assert(sizeof(PipeInputs) <= 8192)`), well under the doc's ~20 KB. |
| D7 | **Poison = per-verb generation, exactly the P0 skeleton** (`PipeFilled.inc:293-307`): `PipeInputs` embeds `MGPipeFilledState m_filled` plus `MGPipeVerb m_currentVerb` and `Uint32 m_verbDepth`. A fill stamps `FilledGen[f] = CurrentVerbSerial`. Every non-pass-through accessor begins with `MGP_POISON_CHECK(Field)` = `if (!MGPipeInputFieldIsFresh(m_filled, Field)) MGPipeInputPoisonFatal(Field, CurrentVerbName())` under `MOBILEGL_PIPE_POISON` (D9). `kMGPipeInputFieldSticky[]` stays **all false in P1**: no sticky field was needed once the six pass-throughs stopped being poison fields (D8). | `ARCHITECTURE.md:349`; scout R5 (each `true` must be argued — none is). |
| D8 | **Six pass-through accessors** declared in a new `MGP_COVERAGE_PASSTHROUGH_LIST(X)` in `Coverage.def`: `GetProgramObject(Uint)`, `GetTextureObject(Uint)` (by-name lookups, Track H residue), `HasOpenTransformFeedbackSpan(Uint64)` (a query over the TF object table, XFB is P9), `ValidateProgramName(Uint)`, `InvalidateCompileEnv()` (`kClientResolved`), `RecordError(...)` (`kReverseChannel`). They keep their `MGPipeInputField` ids (the enum stays "one id per accessor") but are neither stamped, poison-checked, compared, nor members of any fill set. They forward through `GLContext* PipeInputs::Frontend()` = `m_frontendResolver()` — a function pointer installed by `MG_Impl::Pipe::Init()` (D19) — with the same null tolerance the guarded pull sites had (`if (auto* c = Frontend()) c->InvalidateCompileEnv();`). `FrontendIdentity()` returns `static_cast<const void*>(Frontend())`. | `PipeInputs.h` must not spell `pGLContext` (gate C); `InitCapabilities` (`BackendObject_DirectVulkan.cpp:388-389`) runs before any verb. |
| D9 | **`MOBILEGL_PIPE_POISON`** is derived in `PipeInputs.h`, not a CMake option: `#define MOBILEGL_PIPE_POISON (MOBILEGL_PIPE_VERIFY || MOBILEGL_BUILD_DISAGGREGATED || (MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG))`. The Fatal is the generated `MGPipeInputPoisonFatal` (`MGLOG_F` + `std::abort()`, live at INFO). | Correction 5; `MOBILEGL_ASSERT` would vanish in CI builds (`test.yml:85`). |
| D10 | **Verb scope semantics** (`MGPipeVerbScope`, push builds): ctor — `if (m_verbDepth++ == 0) { ++CurrentVerbSerial; m_currentVerb = verb; }` then `SnapshotFromGLContext(gPipeInputs, kMGPipeFillSets[kMGPipeVerbClass[verb]], *pGLContext, verb)` (fills + stamps the class's set; skips the stamp of the field named by the omission knob when `verb` matches), then, if `Features.PipeVerify`, the verb-level compare (D11). dtor — `if (--m_verbDepth == 0) { ++CurrentVerbSerial; m_currentVerb = MGPipeVerb::kNone; }`. Nested scopes (e.g. `glEndTransformFeedback` → `EndTransformFeedback` then `FenceSync`, `GL_Drawing.cpp:1322-1336`) fill into the enclosing verb without re-bumping. **Closing the verb bumps the serial**, so a field read outside any verb — including from a backend path no fill point covers — is `Fatal{UnmigratedPipeInput, "<Field>@<outside any verb>"}`: that is how a missing fill point is found (D.3). | `PipeFilled.inc:19-23`: the poison must see "filled by the previous draw, read by the glTexSubImage that follows". |
| D11 | **What the comparator compares and where it runs** (verify builds, `Features.PipeVerify` set): (a) **verb-level**: at every fill point, after the fill, `SnapshotFromGLContext(scratch, set, ctx, kNone)` then `MGPipeVerifyInputs(gPipeInputs, scratch, set, &field)` over the verb's fill set; (b) **read-level**: every non-pass-through accessor, after the poison check, re-pulls the one field (argument-scoped: the `unit`, `target`, `index` asked for) into a thread-local scratch through `SnapshotFromGLContext` and compares it to the stored value. Per-field equality is `MGPipeFieldEqual` from `PipeVerify.inc` (float bitwise) for value fields, raw-pointer equality for (H), and a POD digest for (R): `{bound.get(), GetVersion()}` per slot, `{base pointer, per touched point: bound.get(), GetRange()}` for bind points, `{per unit ≤ GetMaxTouchedTextureUnit(): 12 bound-texture pointers + sampler pointer}` for texture units, `{Texture.get(), Level, Layered, Layer, Access, Format, Version}` per image unit, the 32 values for current attributes. A difference is `MGLOG_F("MGPipe: Fatal{PipeVerifyDiffer, \"%s@%s\", verb=%llu}", field, verbName, serial)` followed by `std::abort()` unless `Features.PipeVerifyFatal == false` (log and count). In P1 the pushed value and the snapshot come from the same source, so the comparator proves the harness, the fill-point placement (a frontend mutation between the fill and the read is red) and — through the corruption knob — its own sensitivity; P2's tracker is what gives it teeth. | `ARCHITECTURE.md:501`; `Config.h:327-332`. Retention mode (texture dirty rects) is P3b, not P1. |
| D12 | **Fill tables are data, generated.** New hand-maintained `MobileGL/MG_Pipe/FillPoints.def` (B.8) with `MGP_FILL_CLASS_LIST(X)`, `MGP_FILL_VERB_LIST(X)` (`X(Verb, Class, Table)`, 71 rows: the 69 function-pointer members of `GLFunctionsTable`, `BackendObject.h:117-289`, plus `Present`/`SetSwapInterval` from `GlobalBackendFunctionsTable`, `:293-300`) and `MGP_FILL_SET_LIST(X)` (`X(Class, Field)`). `gen_pipe.py` grows **G5b `gen_fill_points`** → `generated/PipeFillPoints.inc` (included from `MGPipe.h` after `PipeFilled.inc`): `enum class MGPipeVerb : Uint8 { …, kNone, kVerbCount }`, `kMGPipeVerbNames[]`, `enum class MGPipeVerbClass : Uint8`, `kMGPipeVerbClass[]`, `kMGPipeVerbClassNames[]`, `struct MGPipeFieldSet { Uint64 Words[(kMGPipeInputFieldCount + 63) / 64]; constexpr Bool Has(MGPipeInputField) const; }`, `kMGPipeFillSets[kMGPipeVerbClassCount]`, and `MGP_FILL_VERB_MEMBER_CHECK` (expands to `(void)&::MobileGL::MG_Backend::GLFunctionsTable::<Verb>, …` / `GlobalBackendFunctionsTable::Present` — a test TU instantiates it, so a verb name that is not a table member is a compile error). Validation in both modes: every verb's class is declared; every set field is a non-pass-through accessor; every non-pass-through accessor is in ≥ 1 set; verb count == `MGP_FILL_VERB_DOCUMENTED_COUNT 71`. | `ARCHITECTURE.md:348` ("G5 从 PipeCalls.def 生成…可能读哪些字段"); the inverse relation is not derivable from `Coverage.def` (scout-generated §10.3), so it is a new source of truth. |
| D13 | **Verb classes** (9): `Draw` (20), `Dispatch` (4), `Clear` (9), `BlitOrCopy` (3), `TextureOp` (4), `Readback` (3), `XfbSpan` (6), `Query` (4), `Passive` (18). B.8 lists the members and the initial field sets. The doc's eight validate entries map onto the first eight; `Passive` collects the entries that read nothing (sync, timer, occlusion, `GetIntegeri_v`, `PatchParameteri`, `ShaderStorageBlockBinding`, `DeleteBackendQuery`, `IsTimerQuerySupported`, `GetGpuTimestampNs`, `Present`, `SetSwapInterval`) and still bumps the serial. | `ARCHITECTURE.md:153`. |
| D14 | **`Coverage.def` changes**: delete row `X(GetBoundTransformFeedbackName, SetStreamOutputTargets)` (`Coverage.def:34`); add `X(GetBoundTransformFeedbackLifetimeId, SetStreamOutputTargets)` and `X(HasOpenTransformFeedbackSpan, SetStreamOutputTargets)` in alphabetical position; add `MGP_COVERAGE_PASSTHROUGH_LIST(X)` (six `X(Accessor, Reason)` rows, D8) and `MGP_COVERAGE_RETIRED_LIST(X)` with one row `X(GetBoundTransformFeedbackName, SetStreamOutputTargets, "D21 rekey (bd2b4158) replaced the name with the lifetime id + open-span query; inventory row VulkanRenderer.cpp:11137")`. G6 joins retired names as mapped (so `kMGPipeInventoryUnmapped` stays 0 and `PipeCatalogueTest.cpp:213` keeps 477); G5 excludes them from the enum. The `GetBufferBindingSlot` split by target (`Coverage.def:36-39`) stays a comment: in P1 the field is keyed by `BufferTarget` inside `PipeInputs`; the split into four wire calls is a P5 concern. | Correction 4/7. |
| D15 | **Live cross-check** `check_live_accessor_set()` in `gen_pipe.py` (both modes): scan `MobileGL/MG_Backend/**/*.{cpp,h}` for `\b(?:pGLContext\|MGB_CTX)\s*->\s*(\w+)` (comments masked with the same masker `gen_pipe_dirty_surface.py:46-85` uses — copy the function); the set of names must equal `ACCESSOR_LIST ∪ PASSTHROUGH_LIST`; any retired name found live, any live name not listed, or any listed name not found is `sys.exit`. This is what makes `PipeCatalogueTest`'s 62 a fact about HEAD instead of about the vendored snapshot. | Scout-generated §10.1 drift. |
| D16 | **Field lists for the four memcmp-fallback types** land in `PipeFields.def`: `MGP_FIELDS_RenderStateParameters` (every member of `MGPipeValueTypes.h:239-…`, in declaration order), `MGP_FIELDS_PixelStoreParameters` (8), `MGP_FIELDS_PerBufferBlendState` (7), `MGP_FIELDS_StencilFaceState` (7), `MGP_FIELDS_DynamicBackendParameters` (every data member of `BackendObject.h:316-549`, 84 lines match the member regex — enumerate by reading), `MGP_FIELDS_MGHostSpan` (`MGPipeHostSpan.h:126-137`). `MEMCMP_FALLBACK_TYPES` becomes empty and the fallback branch is **deleted** from `gen_verify` — a payload member without a field list is then a compile error in `PipeVerify.inc`, which is the "comparator coverage asserted" promise of `PipeFields.def:15-17`. Two dispatcher additions in `gen_verify`: `std::array<T, N>` iterates like the C-array overload; the Vec types (`VecBase::operator==` at `VectorTypes.h:41` is IEEE `==`, so a NaN patch level would false-DIFFER) compare **bitwise** through a trait `MGPipeIsBitwiseComparable<T>` specialised for `FloatVec2/3/4`, `IntVec4`, `UintVec4`, `BoolVec4` (all padding-free). | Scout R7; `PipeVerify.inc:9-23` NaN rule; P0.5 D8 deferred exactly this. |
| D17 | **`PipeCatalogueTest` edits**: `:225` 61 → 62; new expectations that `kMGPipeVerbCount == 71`, `kMGPipeVerbClassCount == 9`, `MGP_FILL_VERB_MEMBER_CHECK` compiles, every non-pass-through field is in ≥ 1 fill set, `kMGPipeVerifiedPayloadCount == 69` (63 + 6). | |
| D18 | **Three new runtime knobs**, declared in `Config.h` after `PipeVerify` and parsed in `ConfigLoader.cpp` after `:246`, all under `#if MOBILEGL_PIPE_PUSH`: `Bool PipeVerifyFatal = true` (`MOBILEGL_PIPE_VERIFY_FATAL`, `QueryEnvQuirkOverride(...) != ForceOff`), `String PipeVerifyCorrupt` (`MOBILEGL_PIPE_VERIFY_CORRUPT=<FieldName>`, perturbs the **snapshot** arm's field after every snapshot), `String PipePoisonOmit` (`MOBILEGL_PIPE_POISON_OMIT=<VerbName>:<FieldName>`, skips the **stamp** — not the value — of that field when that verb fills). Names are validated against `kMGPipeInputFieldNames` / `kMGPipeVerbNames` at `MG_Impl::Pipe::Init()`; an unknown name is `MGLOG_F` + `std::abort()` so a typo cannot disarm a control. | scout-test-infra B3/B4. |
| D19 | **Init**: `MobileGL/Init.cpp` calls `MG_Impl::Pipe::Init()` right after `MG_Util::PipeStats::Init()` (`Init.cpp:114`), under `#if MOBILEGL_PIPE_PUSH`. It installs `gPipeInputs.m_frontendResolver = +[] { return ::MobileGL::MG_State::pGLContext.get(); }`, parses/validates the knobs, and logs once: `MGLOG_I("MGPipe: push arm active: %u fields, %u verbs, %u classes; verify %s (fatal=%d)", …, Features.PipeVerify ? "armed" : "off", …)`. The exact substring the harness greps is **`MGPipe: verify armed`** (emitted only when `Features.PipeVerify`). A pull library never prints it, which is what fails a verify lane run against the wrong binary. | scout-test-infra A3 arming idiom. |
| D20 | **Third CI mode** = job `build-linux-verify` (build-linux's steps with `BUILD_DIR: build-linux` kept — so the tarball layout is identical — plus `-DMOBILEGL_PIPE_VERIFY=ON`, own ccache key, artifact `mobilegl-linux-runtime-verify`), job `integration-verify` (`ctest -L integration-verify --no-tests=error` on that runtime, plus the two inverted negative-control steps), job `retrace-verify` (the `retrace` matrix shape, unpacking the **verify** tarball so `build-linux/libMobileGL.so` is the verify library, `export MOBILEGL_PIPE_VERIFY=1`, `ctest --timeout 10800`, `timeout-minutes: 300`; matrix = the 8 `"verify": true` cases by default, the full 77 when `workflow_dispatch` input `verify_retrace=full`), and `run_trace_case.cmake`'s arming/divergence check. `remove-artifact-clutter` gains `retrace-verify` in `needs`. `MOBILEGL_BUILD_DISAGGREGATED` stays OFF in the verify build (poison comes from D9). | scout-test-infra B1/B2; `ENVIRONMENT`-property trap (`test.yml:252-259`, `MG_IntegrationTest/CMakeLists.txt:339-344`). |
| D21 | **Integration registrations** (in `MG_IntegrationTest/CMakeLists.txt`, inside `if (MOBILEGL_PIPE_VERIFY)`): `DirectGLES.Verify.` / `DirectVulkan.Verify.` (all scenarios; `LABELS integration-gpu integration-verify`; `TIMEOUT 900`; env = backend + `MOBILEGL_PIPE_VERIFY=1` + `MOBILEGL_LOG_FILE_PATH=<binary dir>/pipe-verify-<backend>.log` + the mandatory `${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}` tail), `*.VerifyCorrupted.` (filter `PipeVerifyArmingScenario.CorruptedFieldIsReported`; adds `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` and `MOBILEGL_PIPE_VERIFY_FATAL=0`), `*.PoisonOmission.` (filter `PoisonOmissionScenario.*`; adds `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetTextureUnitObject`). Name parity: `DirectGLES.Verify.<Suite>.<Case>` ↔ `DirectGLES.<Suite>.<Case>`. | `ARCHITECTURE.md:503` by-name parity; 120 s `TIMEOUT` property cannot be raised by `ctest --timeout` (`:393`). |
| D22 | **`SnapshotFromGLContext` signature**: `void SnapshotFromGLContext(PipeInputs& out, const MGPipeFieldSet& fields, MG_State::GLState::GLContext& ctx, MGPipeVerb omissionContext)`; declared in `PipeInputs.h`, defined in `MG_Impl/Pipe/PipeFill.cpp`. Its two callers are the two branches P13 talks about: the filler (`MGPipeVerbScope`, deleted at P13 with the tracker in place) and the comparator (kept). | `ARCHITECTURE.md:369`. |
| D23 | **Ownership of the 26 sync sites in P1: no code change.** Every `SyncPersistentMappedRange()`/`SyncGpuWrites()` call stays where it is; the `BufferObject` it is called on is reached through an aliased or copied field, so the calls are unaffected by the arm. The deliverable is the table `docs/Disaggregated/SYNC_SITES.md` (B.12): per site, the verb class whose scope contains it, the §5.7 reconcile row it implements, and the phase that moves it. The `*IndirectCount` sites keep "only `SyncPersistentMappedRange`" verbatim (`ROADMAP.md:88`). | `ARCHITECTURE.md:294`. |
| D24 | **`PipeStats`** counters are untouched: the `AccessorCalls` tallies (`PipeStats.cpp:15-100` contract) count reads in either arm. The counter names are pinned by `PipeStatsTest.CounterNamesAreStable`. | |
| D25 | **Threading**: `gPipeInputs` is a single global for the single GL thread (`pGLContext` is one global); the only backend reads that can run off the GL thread are pass-throughs (`RecordError`). `AttachPassthroughTessControlStage` (`Managers.cpp:7184`, reads patch state) runs from `SyncToBackend` at `:8070` on the GL thread inside `SyncCurrentProgram` (a Draw). Package B re-confirms that call chain while converting the site. | |
| D26 | **Documentation** is integrator-owned (D.6); packages do not edit `docs/`. `feat/disaggregated` stays off the push triggers (`test.yml:3-9`); CI runs are `workflow_dispatch`. | P0.5 D10. |

### B.3 `PipeInputs` shape (core writes this; the accessor list is the whole 62)

```cpp
// MobileGL/MG_Backend/MGPipe/PipeInputs.h
#pragma once
#ifndef MOBILEGL_MG_BACKEND_MGPIPE_PIPE_INPUTS_H
#define MOBILEGL_MG_BACKEND_MGPIPE_PIPE_INPUTS_H
#include <MG_Pipe/MGPipe.h>                 // field ids, verbs, fill sets, poison helpers, comparators
#include <MG_Pipe/PipeStrangler.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_State/GLState/Core.h>          // phase A: the accessor types ARE the frontend's types.
                                             // Gate A (P13) keeps this include in verify builds only.
#define MOBILEGL_PIPE_POISON (MOBILEGL_PIPE_VERIFY || MOBILEGL_BUILD_DISAGGREGATED || (MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG))
namespace MobileGL::MG_Pipe {
    using MG_State::GLState::GLContext;  // etc.
    struct PipeInputs;
    void SnapshotFromGLContext(PipeInputs& out, const MGPipeFieldSet& fields, GLContext& ctx, MGPipeVerb omissionContext);
    Bool MGPipeVerifyInputs(const PipeInputs& pushed, const PipeInputs& snapshot, const MGPipeFieldSet& fields, const char** outField);
    void ApplyVerifyCorruption(PipeInputs& snapshot, MGPipeInputField field);

    struct PipeInputs {
        // ---- accessors: 62, signatures verbatim from Core.h (B.4) ----
        Int GetActiveTextureUnit() const;                                   // V
        const FloatVec4& GetBlendColor() const;                             // V
        void GetBlendEquationIndexed(Uint, BlendEquation&, BlendEquation&) const;  // V (stored per draw buffer)
        …
        BindingSlot<BufferObject>& GetBufferBindingSlot(BufferTarget);      // R
        BindingSlotRange1D<BufferObject>& GetBufferBindingPoint(BufferTarget, Uint);  // R
        TextureUnit& GetTextureUnitObject(Int);                             // R
        ImageTextureBinding& GetImageTextureBinding(Int);                   // R (+ const overload)
        const CurrentVertexAttributeValue& GetCurrentVertexAttribute(Uint) const;  // R
        const SharedPtr<VertexArrayObject>& GetBoundVertexArray();          // H
        …
        const SharedPtr<ProgramObject>& GetProgramObject(Uint);             // P
        void RecordError(ErrorCode, UniquePtr<ErrorInfo>);                  // P
        // ---- identity / pass-through plumbing ----
        GLContext* Frontend() const { return m_frontendResolver ? m_frontendResolver() : nullptr; }
        const void* FrontendIdentity() const { return Frontend(); }
        const char* CurrentVerbName() const;
        // ---- storage (public: PipeFill.cpp fills it; the unit test corrupts it) ----
        GLContext* (*m_frontendResolver)() = nullptr;
        MGPipeFilledState m_filled{};
        MGPipeVerb m_currentVerb = MGPipeVerb::kNone;
        Uint32 m_verbDepth = 0;
        Uint m_renderStateVersionCopied = ~0u;      // memo key of the 1168-byte copy
        RenderStateParameters m_renderState{};
        … (one member per field, B.4)
    };
    extern PipeInputs gPipeInputs;
    static_assert(sizeof(PipeInputs) <= 8192, "PipeInputs grew past the P1 budget; a field moved to the wrong storage kind");
}
#endif
#endif
```

Accessor body pattern (generated by hand, one per field; the macros live in `PipeInputs.h`):

```cpp
inline const FloatVec4& PipeInputs::GetBlendColor() const {
    MGP_POISON_CHECK(GetBlendColor);       // no-op unless MOBILEGL_PIPE_POISON
    MGP_VERIFY_READ(GetBlendColor);        // no-op unless MOBILEGL_PIPE_VERIFY && Features.PipeVerify (read-level compare, D11 b)
    return m_blendColor;
}
```

`MGP_VERIFY_READ` for an argument-taking accessor passes the argument so the re-pull is scoped (`MGP_VERIFY_READ_ARG(GetTextureUnitObject, unit)`); the implementation is `PipeInputs::VerifyRead(field, …)` in `PipeInputs.cpp`, which calls `SnapshotFromGLContext(tls_scratch, MGPipeFieldSet::Of(field), *Frontend(), MGPipeVerb::kNone)` and then the per-field compare.
