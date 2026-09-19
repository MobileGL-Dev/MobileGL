# P2 implementation brief — the frontend state tracker, render state as a CSO + dynamic state, the residual value block, and the first Track H slice

Tree: `feat/disaggregated @ 48268068` (worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`). **This is the P2 base ref.** The scouts were written at `e7a6a72f`; the one commit since is CI-only (`48268068 [CI] (Workflows): run the test and apk lanes on every push to feat/disaggregated`, `.github/workflows/{apk,test}.yml`, +8 lines), so every scout `file:line` still holds and P2 branches from `48268068`.

WSL: `~/w7/pipe` is a worktree of the same repo on `feat/disaggregated`; `~/w7/base` is `dev@81b17c0b` (the performance anchor, untouched). Tree creation is `scratchpad/wf4/wsl_p1_tree.sh <slug> <base-ref> [verify]` with `p1`→`p2` throughout (it creates `~/w7/p2-<slug>` on branch `p2/<slug>`, initialises submodules, copies `3rdparty/glslang/External` and the trace fixtures, then configures and builds `build-linux` (pull), `build-push`, and with a third argument `verify` also `build-verify`). The device-lock loop is `scratchpad/wf2/stats_baseline.sh:16-29` verbatim (mkdir lock under `.../8ab6b3cb-.../scratchpad/w7/locks/<serial>`, 1500 s acquire timeout, 20 s poll, one hold per run, `rm -rf` after).

Every `file:line` below was re-opened at `48268068`. Corrections to the scouts are **[correction]**; deliberate departures from the design documents are **[deviation]** and carry their reason.

**Scout verification summary.** All five scouts were read in full and their load-bearing claims re-opened. The following are wrong or incomplete and are fixed here:

1. **[correction] Three capabilities have no storage, not two.** `RenderState::SetCapability`'s `default: // not supported currently` arm (`MobileGL/MG_State/GLState/RenderState/RenderState.cpp:381-382`) and `IsCapabilityEnabled`'s `default: return false;` (`:429-430`) swallow `DepthClamp`, `FramebufferSrgb` **and `TextureCubeMapSeamless`** — the last one is reachable from `glEnable` (`MG_Util/Converters/GLToMG/RenderStateEnumConverter.cpp:301` maps `GL_TEXTURE_CUBE_MAP_SEAMLESS` onto `CapabilityInput::TextureCubeMapSeamless`, `MGPipeValueTypes.h:202`) and has zero read points anywhere. `scout-render-state.md` §1.3, `generated/PipeSpanTable.inc:24-26`, `ROADMAP.md:78` and `MEASUREMENTS.md:84` all name only two. D8 gives all three real storage.
2. **[correction] The free padding hole is exactly 3 bytes at `[581, 584)`**, between `ColorMasks` (offset 549, size 32, align 1) and `ClearColor` (offset 584, align 4). `scout-render-state.md:143-151` gropes at this and half-retracts it. It is verified from the declarations: `using Bool = bool` (`MG_Util/Types.h:33`), `BoolVec4 = Vec4<Bool>` = `VecBase<…, Bool, 4>` whose only member is `Array<Bool,4>` (`MG_Util/Math/VectorTypes.h:16-18`, alias `:222`) → align 1, size 4. Three bytes is exactly enough for the three new capability bools, so `sizeof(RenderStateParameters)` stays 1168 and **no existing member's offset moves** — which is what keeps Espryt's `kBlendSpanBegin`/`kBlendSpanEnd` (312/536) and the whole hand-derived offset table valid.
3. **[correction] `RenderState`'s three value members are not value-initialised.** `RenderStateParameters m_parameters;` (`RenderState.h:172`) and the two `PixelStoreParameters` (`:175-176`) have no `{}`, so their padding bytes are indeterminate. A byte-range content hash over them (which is what the CSO key is) would then be stable only within one context. D3 adds `{}` to all three and pays for it with one attributed `.text` resize.
4. **[correction] `ComputePipelineStateHash` is not a pure function of `RenderStateParameters`.** Its signature is `(Uint32 colorAttachmentCount, VkSampleCountFlagBits rasterizationSamples)` (`VulkanRenderer.cpp:4821-4822`) and it folds `ResolveEffectiveSampleMask` (`:4813-4819`, itself reading `Multisample`, `SampleMask` and `SampleMaskValue`). The scouts list the hashed fields but not that the signature makes the hash render-pass-dependent. Consequence, and it is load-bearing: a render-state CSO handle **cannot** replace `pipelineStateHash` on its own; the render-pass facts must stay in the memo key. D12 keeps `renderPassHash` as a separate key component, which already separates them.
5. **[correction] `ARCHITECTURE.md:504` names `tools/bench.sh`; the script is `tools/device_bench/bench.sh`** (verified: `tools/device_bench/{README.md,bench.sh,profile.sh,session.sh,devices/odinlite.env}`). The integrator fixes the citation.
6. **[correction] The eight/nine validate-entry disagreement resolves to nine.** `ARCHITECTURE.md:153` names eight; the landed `MGP_FILL_CLASS_LIST` has nine (`MG_Pipe/FillPoints.def:107-108`, with the reason in its own comment). P2 adopts nine and the integrator corrects the doc (D5).
7. **[correction] The dirty-surface gate is not a gate yet.** `ARCHITECTURE.md:172` and `:568` say "P1 起成为门"; `.github/workflows/test.yml:1524-1528` says it stays informational and becomes a gate in P2. The workflow is what runs. P2 promotes it (D16).
8. **Confirmed at `48268068`, unchanged from the scouts**: `python3 scripts/gen_pipe_dirty_surface.py --summary` still prints `41 files / 926 mutator calls / 73 distinct mutators / 92 in 36 IMMEDIATE PUBLISH POINTS across 7 distinct mutators / 834 DEFERRED`. `sizeof(RenderStateParameters)==1168`, `PerBufferBlendState`/`StencilFaceState`/`PixelStoreParameters` all 28 (`MGPipeValueTypes.h:536-544`). `StencilFaceState` is `Func(0) Ref(4) ValueMask(8) WriteMask(12) FailOp(16) PassDepthFailOp(20) PassDepthPassOp(24)` (`:229-237`) — so the pipeline half of a face is `[0,4) ∪ [16,28)` and the dynamic half `[4,16)`, exactly as `scout-render-state.md` §7.2 says. `ResidualValueBlock` is 1248 = 1168 + 28 + pad + 8 + 4 + 4 + 16 + 8 + 8 (`MGPipeTypes.h:516-537`). `kMGPipePipelineStateMembers[]` is 24 names (`generated/PipeSpanTable.inc:34-60`), `kMGPipePipelineChunks`/`kMGPipeDynamicChunks` are declared and never defined (`:66-67`). `VertexArrayObject`'s three backend memo triples are at `VertexArrayObject.h:106-150`, storage `:190-197`, and `SetBackendAuxMemo` has no live reader (only writer `VertexInputStateFactory.cpp:83-84`). `MGP_FILL(Verb)` expands to `MGPipeFillForVerb(MGPipeVerb::Verb)` (`MG_Impl/Pipe/PipeFill.h:52`) and `((void)0)` in the pull build (`:54`).
9. **[correction] `scout-render-state.md` §6.4 under-counts the residual ratchet.** It says landing the render-state CSO takes 1248 → 80. P2 also lands `set_pixel_pack_state` and `set_patch_state`, so the ratchet is **1248 → 8** (D9).

---

## A. Goal and acceptance gate

`docs/Disaggregated/ROADMAP.md:18`, verbatim:

> | **P2** 渲染状态 CSO + 第一片 Track H + 残余值块 | 18–26 | `MG_Impl/Pipe/Tracker`（dirty 位、5 个聚合世代、抑制器骨架）；`gen_pipe_dirty_surface.py` 首轮映射成门；`MGPipeRenderStateSpans` + G7 setter 一致性测试；`CsoCache`（64 项，键 = pipeline 子集）；`create/bind_render_state` + `set_dynamic_state`（Espryt `SyncRenderState` 一行不动；Magma `ComputePipelineStateHash`/`GetOrCreatePipeline`/`ApplyDynamicDrawStateTail` 改从 CSO 与动态 payload 取）；`set_pixel_pack_state`、`set_patch_state`、`set_vertex_attrib_defaults`；`set_residual_value_state` + `ResidualValueBlock` 绊线；**第一片 Track H**：Espryt 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`/`g_fbSlotCache`/GC）与 Magma 子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，删前端 VAO 里的后端裸指针）；`MOBILEGL_PIPE_LEGACY_MEMOS`；补 `FramebufferSrgb`/`DepthClamp` 存储 | 集成 × 2 后端 × {pull, push} 逐名相同；40 trace push 下 SSIM ≥ 0.99 双后端；verify 零分歧；`HandleRecycleScenario` 绿且重键前红；G7 测试绿且拿掉一个字段能红；两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内；Blaze3D blend-toggle 微基准；CSO 内容寻址关闭的负面对照 | P1 |

Binding context: `ARCHITECTURE.md:147-155` (§5.1 — push happens at validate, before the verb, never in the GL setter, because Blaze3D brackets every batch with `glEnable/glDisable(GL_BLEND)`; only `BufferBackendOps`' seven hooks push at GL-call time), `:157-174` (§5.2 — the dirty-bit table, the five aggregate generations, wrap-around widened at the tracker boundary), `:176-190` (§5.3 — D-B1: whole blob on the wire, CSO identity = the pipeline subset only, the dynamic subset enumerated, the split written in exactly one place, `SyncRenderState` untouched, the client value-fetch order, the `FramebufferSrgb`/`DepthClamp` hole), `:192-200` (§5.4 — the D-B3 validation invariant and the four coalescing rules), `:357-359` (§9.4 — the residual value block and its three disciplines), `:363` (§9.5 — the 21-memo census the Track H slice pays into), `:365-369` (§9.6 — why `MOBILEGL_PIPE_LEGACY_MEMOS` must be a compile-time arm), `:497-507` (§13.2 — the five-part validation gate), `:552` (`MG_Impl/Pipe/` is where `Tracker`, `SlotAllocator`, `CsoCache` land), `:576-597` (the switch tables). `ROADMAP.md:7`: **每个门必须能因它存在的理由变红**; ALL must build; no hot-path instrumentation may be committed; Windows is not a correctness gate; device comparisons are reboot-clean, same-thermal-window, paired A/B. `ROADMAP.md:39`: **day 43 is the GO/NO-GO.** `ROADMAP.md:42-58` is the checklist and the verdict criteria; items 2–7 of it are P2's to produce.

Decoded into checkable statements. Unless a row says otherwise, commands run in a P2 WSL worktree; `$BASE` = `48268068`.

| # | statement | the exact command that checks it |
|---|---|---|
| **G1** | The **pull build** (`MOBILEGL_PIPE_PUSH=OFF`, `MOBILEGL_PIPE_VERIFY=OFF`, Release/INFO) has an identical `nm --defined-only` symbol set before and after P2 — 0 added, 0 removed, 0 renamed — and the only `.text` resize is `MG_State::GLState::RenderState::RenderState()` (D3's three `{}`), named in the commit message. | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --json` → `added==removed==renamed==0`; `resized` is empty or exactly the one mangled `RenderState::RenderState()` symbol. |
| **G2** | `ctest -L integration-gpu` is **name-for-name identical between the pull and the push build**, on both backends, and green in both. | `for d in build-linux build-push; do ctest --test-dir $d -N \| grep -E '^\s+Test #' \| sed 's/^ *Test *#[0-9]*: //' \| sort > ~/w7/p2-names-$d.txt; done; diff ~/w7/p2-names-build-linux.txt ~/w7/p2-names-build-push.txt` → empty; then `ctest --test-dir build-linux -L integration-gpu --no-tests=error -j 4` and the same on `build-push`, both green. |
| **G3** | The 40-trace corpus replays **under push** with SSIM ≥ 0.99 on both backends (79 desktop cases: 40 × 2 minus `iterationrp × DirectGLES`). | `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-push/libMobileGL.so --out ~/w7/retrace-out/p2-push -j 4 --ssim 0.99` (the P0.5/P1 script, `scratchpad/wf2/retrace_gate.py`). |
| **G4** | The **verify** build shows zero divergence: no `Fatal{PipeVerifyDiffer`, no `Fatal{UnmigratedPipeInput`, no `Fatal{PipeResidualDiverged`, and every case armed. | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4`; `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-verify/libMobileGL.so --out ~/w7/retrace-out/p2-verify -j 4`; then `grep -l 'Fatal{' ~/w7/retrace-out/p2-verify/*/mobilegl.log` empty and `grep -L 'MGPipe verify:' ~/w7/retrace-out/p2-verify/*/mobilegl.log` empty. |
| **G5** | **`SyncRenderState` is not one line changed.** The whole of `namespace RenderStateImpl` in `DirectGLES.cpp` is byte-identical to the base ref (line numbers may move; the text may not). | `for r in $BASE HEAD; do git show $r:MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp \| awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' \| sha256sum; done` → the two hashes are equal. |
| **G6** | **G7 setter consistency**: for every public `RenderState` setter, the pipeline-subset hash moves ⟺ `m_pipelineStateVersion` moves; and the chunk table partitions `[0, sizeof(RenderStateParameters))` exactly (sorted, non-overlapping, complete). | `ctest --test-dir build-push -R 'RenderStateSpans\.' --no-tests=error --output-on-failure` (the partition half is a `static_assert` in `MGPipeRenderStateSpans.cpp`, so a gap is a build break). |
| **G7** | **G6's negative control**: moving one member from `kMGPipePipelineChunks` to `kMGPipeDynamicChunks` (partition still complete, so it still compiles) turns the setter-consistency test red, naming that member's setter. | `scripts/g7_negative_control.sh` (D19) — patches `MGPipeRenderStateSpans.cpp` to demote `ColorMasks`, rebuilds, expects `ctest -R 'RenderStateSpans\.SetterConsistency'` to **fail**, then reverts. Non-zero rc from ctest is the pass. |
| **G8** | **`HandleRecycleScenario` is green after the re-key and red before it**, on at least one backend, as three always-on ctest entries: handle arm green, legacy arm green, ABA-control arm red. | `ctest --test-dir build-verify -R 'HandleRecycle' --no-tests=error --output-on-failure` — `*.Handles` and `*.Legacy` pass, `*.AbaControl` passes **by asserting the corruption** (D18). |
| **G9** | `gen_pipe_dirty_surface.py` is a **gate**: the mapping file covers all 73 mutators, an unmapped mutator or a mapping row naming a mutator that no longer exists fails the job, and the report regenerates with `git diff --exit-code`. | `python3 scripts/gen_pipe_dirty_surface.py --check` (rc 0) and `python3 scripts/gen_pipe_dirty_surface.py --self-test` (rc 0; canned negative controls). CI: `pipe-gates`. |
| **G10** | The **`ResidualValueBlock` tripwire** holds: `MGL_RESIDUAL_BLOCK_SIZE` is **8** (down from 1248, never up), every member has an `offsetof` assertion, `MOBILEGL_PIPE_STATS`'s `resid=` byte class is **non-zero** in a device window, and a capability the residual block and the working block disagree about aborts. | build (the ratchet is a `static_assert`); `ctest --test-dir build-verify -R 'Residual' --no-tests=error`; on device `grep 'MGPipe stats:' mobilegl.log \| tail -1` shows `resid=` > 0. |
| **G11** | **Two devices, reboot-clean, paired A/B: per-thread CPU p50 and p99 deltas are not negative on either device, and the tracker's absolute ns/draw is inside the pinned ceiling.** | D.4 — the `benchmark.json` `frameCpuTimesMs[]` series (E adds it) reduced host-side to p50/p99, plus the DriverBench `ns_per_op` decomposition T1/T2. The ceiling is pinned in `MEASUREMENTS.md` **before** the run (D.4.5). |
| **G12** | The **Blaze3D blend-toggle microbenchmark** and the **CSO-content-addressing negative control** are both published, and the negative control cannot rot: a ctest entry proves the switch actually changes CSO mint/bind counts. | `MobileGL/MG_Benchmark/Driver/run_driver_bench.sh {native,espryt,magma}` on `mc_state_toggle`; `ctest --test-dir build-push -R 'CsoContentAddressing' --no-tests=error`. |
| **G13** | Nothing reintroduces a purity violation: `pGLContext` still appears zero times under `MG_Backend/`, the include closure still holds, no stdio instrumentation, generators regenerate clean. | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` empty; `python3 scripts/check_include_closure.py`; `python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test`; the `pipe-gates` stdio grep. |
| **G14** | Existing test names are never removed (additions only), and the full CI matrix is green. | `ctest --test-dir build-linux -N \| … \| comm -23 ~/w7/p2-before-ctest-names.txt -` empty; `gh workflow run test.yml --ref feat/disaggregated`. |

Baseline captures the integrator takes **before** any package lands (in `~/w7/pipe` at `48268068`):

```sh
cp ~/w7/pipe/build-linux/libMobileGL.so ~/w7/p2-before-libMobileGL.so
ctest --test-dir ~/w7/pipe/build-linux -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort > ~/w7/p2-before-ctest-names.txt
git -C ~/w7/pipe show HEAD:MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp \
  | awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' | sha256sum > ~/w7/p2-before-syncrenderstate.sha
python3 scripts/gen_pipe_dirty_surface.py > ~/w7/p2-before-dirty-surface.txt
```

---

## B. Fixed design decisions

Nothing in this section is re-decided inside a package. Where a decision departs from the design documents it is marked **[deviation]** and the doc line the integrator must correct is named.

### D1 — Where the tracker runs: `MGP_FILL` stays; `MGPipeFillForVerb` becomes the tracker's entry point

`MGP_FILL(Verb)` (`MG_Impl/Pipe/PipeFill.h:52`) is already exactly one statement before every `gBackendFunctionsTable.GL.*` call, after every early return (`PipeFill.h:10-15`, and the 83 statements over 69 verbs the P1 brief §B.7 enumerates). It is the validate point `ARCHITECTURE.md:151` demands and it is already generated-table-driven.

**Decision: the macro spelling, the 83 call sites and the verb enum do not change. `MGPipeFillForVerb(MGPipeVerb)` is renamed `MGPipeValidateForVerb(MGPipeVerb)` and its body becomes the tracker's walk; `MGP_FILL` expands to the new name.** The tracker does **not** sit above `MGP_FILL` and there is no new `ValidateForDraw`/`ValidateForClear`/… family of functions.

- **[deviation] against `ARCHITECTURE.md:153`**, which asks for eight named `ValidateFor*` entry points. Reason: the landed machinery already dispatches on `kMGPipeVerbClass[verb]` (`generated/PipeFillPoints.inc:197`) into **nine** classes (`FillPoints.def:107-108`), and eight separate entry points would either duplicate that table or force a merge of `kProgramOp` back into `kDraw` that `FillPoints.def:105-106` explicitly argues against. One entry point with a class-indexed switch is the same code with one call site per verb instead of nine. The integrator corrects `ARCHITECTURE.md:153` to nine classes and names them.
- `MGPipeLeaveVerb()` (`PipeFill.h:34`, used by `MG_Test/ScopedPipeVerb.h`) keeps its name and semantics.
- `MGP_NOTE_MUTATION` (`MG_Pipe/PipeMutation.h`, four statements over three fields at `TextureState.h:85,98,111,133`) **stays exactly as it is**. `MEASUREMENTS.md:117` is right that it is the shape the tracker needs; the tracker does not absorb it, because it answers a different question (a backend writing frontend state *inside* a verb, after the walk has run).

The body of `MGPipeValidateForVerb(verb)`, in order:

1. everything `MGPipeFillForVerb` does today up to and including `SetIdentity` / the null-context early return (`PipeFill.cpp:566-589`) — unchanged, including the poison serial bump and the verify arming;
2. **the dirty walk** (D4): compare the tracker's `m_lastPushed` shutters against the live counters, producing a `Uint32` dirty mask;
3. **emission** (D6, D7, D9, D10, D11): for each set dirty bit whose subsystem bit is on in the runtime `MOBILEGL_PIPE_PUSH` bitmask, build the payload and hand it to the applier (D2);
4. **the residual fill**: the P1 per-class copy loop (`PipeFill.cpp:591-609`), restricted to the fields **not** covered by an emitted call (D5's `kMGPipeFieldEmittedBy[]`), with stamping unchanged;
5. `EntryCompare(inputs, mask)` under verify (`PipeFill.cpp:611`) — unchanged, and now **no longer tautological**, which is the P1 brief's D8 promise coming due.

### D2 — The applier and where the server's working state lives

**Decision: in the monolith, the server's per-context working `RenderStateParameters` *is* `PipeInputs::m_renderState` (`MG_Backend/MGPipe/PipeInputs.h:645`).** `bind_render_state` and `set_dynamic_state` scatter their chunks straight into it. That is why Espryt's `const RenderStateParameters&` binding at `DirectGLES.cpp:2056` and the `Uint16` read at `:2028` need no change at all — the block they read is the assembled block. This is the mechanism behind "`SyncRenderState` 一行不动" (`ARCHITECTURE.md:188`, `ROADMAP.md:47`), and it is what makes the verify comparator a real oracle: the compare-at-read now proves *assembled == live*, field by field, at every backend read.

New files: `MobileGL/MG_Pipe/PipeApply.{h,cpp}`, `namespace MobileGL::MG_Pipe`, compiled only under `MOBILEGL_PIPE_PUSH`.

```cpp
// PipeApply.h — the in-process applier. Under split this file is the server half
// (ARCHITECTURE.md:375, MG_Remote/Server/PipeApplier); in the monolith it writes gPipeInputs
// directly, so a call and its effect are one function call apart and nothing is serialised.
void MGPipeApplyCreateRenderState(const MGPRenderStateDesc& desc, const void* chunkBytes);
void MGPipeApplyBindRenderState(const MGPBindRenderState& bind);
void MGPipeApplySetDynamicState(const MGPDynamicState& dyn, const void* chunkBytes);
void MGPipeApplySetPixelPackState(const MGPPixelPackState& pack);
void MGPipeApplySetPatchState(const MGPPatchState& patch);
void MGPipeApplySetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr, const MGPAttribValue* tail);
void MGPipeApplySetResidualValueState(const ResidualValueBlock& block);
```

The applier owns a per-context CSO store: `Vector<RenderStateCsoRecord>` indexed by `MGPipeHandle::Slot`, each record holding the 396 pipeline bytes and a `Gen`. `MGPipeApplyBindRenderState` memcpy-scatters the record's seven pipeline chunks into `m_renderState`, then writes `m_renderStateParametersVersion = bind.Version` and `m_pipelineStateVersion = bind.PipelineVersion`. `MGPipeApplySetDynamicState` scatters the chunks named by `dyn.ChunkMask` and writes `m_renderStateParametersVersion = dyn.Version`.

**After any scatter the applier calls `MGPipeDeriveRenderStateFields(PipeInputs&)`** — see D5.

The applier `MOBILEGL_ASSERT`s (debug/verify only) that a `bind` names a live `{slot, gen}` and that `desc.ChunkMask` covers every chunk of a brand-new CSO.

### D3 — Value-type changes in `MGPipeValueTypes.h` and `RenderState`

1. **Three new members in `RenderStateParameters`**, declared *immediately after `Array<BoolVec4,8> ColorMasks;` and before `FloatVec4 ClearColor;`*, filling the 3-byte hole at `[581, 584)`:

```cpp
        // GL_FRAMEBUFFER_SRGB / GL_DEPTH_CLAMP / GL_TEXTURE_CUBE_MAP_SEAMLESS. Until P2 these
        // three fell to SetCapability's `default:` arm - glEnable was swallowed and
        // IsCapabilityEnabled answered a compile-time false, so DirectGLES' sRGB block
        // (DirectGLES.cpp:2168) and five DirectVulkan read points consumed a constant.
        // Placed HERE, in the three alignment bytes between ColorMasks and ClearColor, so
        // sizeof(RenderStateParameters) stays 1168 and no existing offset moves: the Espryt
        // span constants and the P2 chunk table both depend on that.
        Bool FramebufferSrgbEnabled = false;
        Bool DepthClampEnabled = false;
        Bool TextureCubeMapSeamlessEnabled = false;
```

   The existing `static_assert(sizeof(RenderStateParameters) == 1168, ...)` (`:541`) is the check; if it fires, the hole is not where this brief computed it and the implementer must find the real one with a temporary `static_assert(offsetof(...))` rather than growing the struct (growing it is a `MGL_RESIDUAL_BLOCK_SIZE` violation).
2. **Three rows in `MGP_FIELDS_RenderStateParameters(F)`** (`PipeFields.def:230-247`), in declaration order. `gen_pipe.py` asserts every direct member has a row (`PipeFields.def:224-228`), so omitting one fails `pipe-gates`.
3. **Three `SET_CAPABILITY` arms and three `RETURN_CAPABILITY` arms** in `RenderState.cpp` (`:317-338`, `:390-413`), keeping the source order of `CapabilityInput`. All three therefore call `BumpVersions()` and land in the **pipeline** half (D6). `FramebufferSrgb` on the pipeline side is what `ARCHITECTURE.md:190` asks for; `DepthClamp` is `VkPipelineRasterizationStateCreateInfo::depthClampEnable` and `TextureCubeMapSeamless` changes sampler interpretation, so both belong there too.
4. **`{}` on the three value members** of `RenderState` (`RenderState.h:172`, `:175`, `:176`) so padding is deterministic. Cost: one `.text` resize of `RenderState::RenderState()`, allowed by G1 and named in the commit message. Reason: the CSO key is a byte-range hash (D6); indeterminate padding would make a CSO handle reproducible only within one context, and would make the residual block's byte-level trip wire meaningless.

**Behaviour change, and it is real, so it is gated.** With storage, `glEnable(GL_FRAMEBUFFER_SRGB)` now (a) makes `glIsEnabled` answer true and (b) makes Espryt issue a real `glEnable(GL_FRAMEBUFFER_SRGB)` at `DirectGLES.cpp:2170` — that block is **ungated** by the span dirty flags and already reads the accessor, so it needs no edit and is not a `SyncRenderState` change. `DepthClamp` and `TextureCubeMapSeamless` gain no reader on either backend (verified: no `depthClampEnable`/`alphaToOneEnable` anywhere in `PipelineFactory.{h,cpp}`/`VulkanRenderer.cpp`; `TextureCubeMapSeamless` appears only in the three enum converters), so their only effect is `glGet`/`glIsEnabled`. `MEASUREMENTS.md:84` records that none of the 41 fixtures enables any of them, so G3's SSIM gate is the check that no fixture output moves.

### D4 — Dirty bits and the five aggregate generations

**Representation.** `Uint32 MGPipeTracker::m_dirty`, one bit per row of `ARCHITECTURE.md:161-168`, declared as a plain `enum class MGPipeDirty : Uint32` in `MG_Impl/Pipe/Tracker.h` (hand-written, not generated — the list is 15 long and is design, not derived data), with `static_assert(kMGPipeDirtyCount <= 32)`:

| bit | name | class | shutter the tracker compares |
|---|---|---|---|
| 0 | `NEW_RENDER_STATE` | value | `GetRenderStateParametersVersion()` (`Uint16`, widened) |
| 1 | `NEW_PIPELINE_STATE` | value | `GetPipelineStateVersion()` (`Uint16`, widened) |
| 2 | `NEW_PIXEL_PACK` | value | `PixelStoreParameters` pack, `BitwiseEqual` |
| 3 | `NEW_PATCH_STATE` | value | the patch trio, `BitwiseEqual` (**NaN is legal**, `ARCHITECTURE.md:162`) |
| 4 | `NEW_VERTEX_ATTRIB_DEFAULTS` | value | `ContentHash` over the 32 `CurrentVertexAttributeValue`s |
| 5 | `NEW_VERTEX_ELEMENTS` | value | `VertexArrayObject::GetConfigVersion()` |
| 6 | `NEW_SHADER` | value | `ProgramObject::GetLinkVersion()` |
| 7 | `NEW_SHADER_BINDINGS` | value | image-unit / backend-state / block-binding / uniform-write-set versions |
| 8 | `NEW_GLOBAL_CONSTANTS` | value | `GetUBOContentVersion()` |
| 9 | `NEW_VERTEX_BUFFERS` | object | `VertexArrayState::m_anyVaoAttributeGeneration` **(new)** → 32-attribute prefix |
| 10 | `NEW_INDEX_BUFFER` | object | index slot version + bound object `{slot, gen}` |
| 11 | `NEW_FRAMEBUFFER` | object | `FramebufferState::m_anyAttachmentGeneration` **(new)** + object/slot version → recompute `ContentHash` |
| 12 | `NEW_SAMPLER_VIEWS` | object | `TextureState::m_anyTextureContentGeneration` **(new)** + bind generation → `GetMaxTouchedUnit()` prefix |
| 13 | `NEW_SAMPLERS` | object | `TextureState::m_anyTextureParamsGeneration` **(new)** + sampling-resolution generation |
| 14 | `NEW_SHADER_IMAGES` | object | the same two texture aggregates + image-unit version |
| 15 | `NEW_CONST_BUFFERS` / `NEW_SHADER_BUFFERS` / `NEW_SO_TARGETS` (three bits, 15–17) | object | `BufferState::m_anyBufferChangeGeneration` **(new)** → `GetTouchedBindPointCount()` prefix |

**P2 only *emits* for bits 0–4.** Bits 5–17 are computed, counted and asserted by the tracker but their emission is P3a/P3b/P4a/P4b; their fields keep going through the residual fill loop. The tracker records, per verb class, how many draws each bit fired on, which is the first honest measurement of "does this shutter actually shut" and is what the dangerous direction (`ARCHITECTURE.md:502`, a dirty bit that fires too *rarely*) is judged against.

**The five aggregate generations.** `Uint64`, monotonic, bumped at the existing bump points, all **`#if MOBILEGL_PIPE_PUSH`-guarded** so the pull build's `MG_State` objects do not change size or code (G1):

| member | file | bumped at |
|---|---|---|
| `VertexArrayState::m_anyVaoAttributeGeneration` | `MG_State/GLState/VertexArrayState/VertexArrayState.h` | every `VertexArrayObject::BumpConfigVersion` site (`VertexArrayObject.cpp:299,305,311`) — reached through a new `NotifyAttributeChanged()` on the owning state |
| `FramebufferState::m_anyAttachmentGeneration` | `.../FramebufferState/FramebufferState.h` | `FramebufferObject.cpp:192,203,215` |
| `TextureState::m_anyTextureContentGeneration` | `.../TextureState/TextureState.h` | `TextureObject.cpp:285,354,365`; `TextureObject2DCube.cpp:51,62` |
| `TextureState::m_anyTextureParamsGeneration` | `.../TextureState/TextureState.h` | `TextureObject.cpp:101,126,140,154,203,210,227,238,258,294,303`; `TextureObject.h:182`; `SamplerObject.cpp:27-35` |
| `BufferState::m_anyBufferChangeGeneration` | `.../BufferState/BufferState.h` | `BufferObject.cpp:45,52,61,75,82,304,368` |

The bump is spelled `MGP_NOTE_AGGREGATE(VaoAttribute);` — a new macro in `MG_Pipe/PipeMutation.h` next to `MGP_NOTE_MUTATION`, `((void)0)` in the pull build, exactly the same shape and for the same reason (`PipeMutation.h:29-40`). A per-object bump reaches its owning state through a back-pointer the object already has, or, where it does not, through a free function `MGPipeNoteAggregate(MGPipeAggregate)` that finds the live `GLContext` — the second form costs a global load on a cold path and is what `MGP_NOTE_MUTATION` already does (`PipeFill.cpp:479-491`).

**Wrap-around.** Three shutters are `Uint16` (`RenderState::m_version`, `m_pipelineStateVersion`, `FramebufferObject::m_objectVersion`) and two more are `Uint16` on `SamplerObject`/`TextureObject` params. The tracker widens them in its **own** `m_lastPushed[]` (`Uint32` accumulator plus the last observed `Uint16`; a decrease means a wrap and adds 65536). **`MG_State` is not changed** (`ARCHITECTURE.md:174`). A wrap is harmless locally: one extra re-push, never a missed push.

### D5 — The derivation step, and which `PipeInputs` fields stop being copied

29 of the 47 `kDraw` `PipeInputs` fields are pure functions of `RenderStateParameters`. Once the applier assembles the working block, copying them again from `GLContext` would be a second pull. **Decision: the applier derives them from the working block.**

`MGPipeDeriveRenderStateFields(PipeInputs&)` in `PipeApply.cpp` recomputes exactly:
`m_blendColor`, `m_blendEquation[8][2]`, `m_blendFunc[8][4]`, `m_clampReadColor`, `m_clearColor`, `m_clearDepth`, `m_clearStencil`, `m_colorMask[8]`, `m_cullFaceMode`, `m_depthFunc`, `m_depthMask`, `m_depthRange[16]`, `m_lineWidth`, `m_logicOp`, `m_minSampleShading`, `m_patchInner`, `m_patchOuter`, `m_patchVertices`, `m_polygonModeFront`, `m_polygonOffsetFactor`, `m_polygonOffsetUnits`, `m_primitiveRestartIndex`, `m_provokingVertexMode`, `m_scissorBox`, `m_stencil[2]`, `m_viewport`, `m_viewportIndexed[16]`, `m_capability[35]`, `m_capabilityIndexed` (Blend×8, ScissorTest×16).

- **[deviation] against P1 brief D4's "no derivation logic is re-implemented in `PipeInputs`".** Reason: the alternative is to keep pulling those 29 fields per verb, which is the cost P2 exists to remove. The derivations are one to four lines each and are mechanical transcriptions of `RenderState`'s getters (`RenderState.cpp:53-971`); two are non-trivial and must be transcribed exactly: `GetViewport()` returns viewport 0 **rounded to integers** (`RenderState.h:27-31`), and `IsCapabilityEnabled`/`IsCapabilityEnabledIndexed` are the 35-way and 2-way switches at `RenderState.cpp:387-432` and `:434-…`, including `Blend → BlendStates[0].Enabled` and `ScissorTest → ScissorTestEnabledMask & 1`.
- **The guard is the oracle P1 built.** `MOBILEGL_PIPE_VERIFY`'s compare-at-read (`PipeInputs.h:62-63` → `PipeFill.cpp:439-463`) re-reads each of these from the live context at *every backend read* and compares field-wise. A transcription error is caught on the first draw that reads it, across 79 retraces and 818 integration-verify entries. On top of that, a unit test (`RenderStateSpansTest.DerivationMatchesTheFrontendGetters`) drives every setter and compares all 29 derived values against the `GLContext` getters.

`gen_pipe.py` gains a generated table `kMGPipeFieldEmittedBy[]` (from a new `MGP_COVERAGE_EMITTED_LIST` in `Coverage.def`) naming, per `MGPipeInputField`, the P2 call that now supplies it. The residual fill loop skips any field whose entry is non-`kNone` **and** whose subsystem bit is on; that is one array lookup per field and it is what makes the runtime bitmask a true per-subsystem A/B.

### D6 — The chunk table: the split, written in exactly one place

New files `MobileGL/MG_Pipe/MGPipeRenderStateSpans.{h,cpp}`, compiled **only under `MOBILEGL_PIPE_PUSH`** (appended to `SOURCE_FILES` inside the existing `if (MOBILEGL_PIPE_PUSH)` block, `CMakeLists.txt:462-468`), which is how the pull build gains no symbol (G1). `generated/PipeSpanTable.inc:66-67` already declares `kMGPipePipelineChunks` / `kMGPipeDynamicChunks`; a declaration costs the pull build nothing.

**The rule that decides the split, and it is the only rule:**

> **A byte of `RenderStateParameters` is in the pipeline half if and only if some public `RenderState` setter that calls `BumpVersions()` writes it. Every other byte is in the dynamic half. There is no third set.**

That makes `ARCHITECTURE.md:186`'s G7 invariant — *pipeline-subset hash moves ⟺ `m_pipelineStateVersion` moves* — true **by construction** rather than by inspection, and it resolves all six of the violations `scout-render-state.md` §2.5 found, in the direction of *growing the subset* rather than demoting setters:

- **[deviation] the pipeline subset is a strict superset of the 24 members `ComputePipelineStateHash` hashes today.** It adds `SampleCoverageValue`, `SampleCoverageInvert`, `FrontFaceModeSetting`, `ProvokingVertexModeSetting`, `ScissorTestEnabledMask`, `PolygonModeBack`, the 11 unhashed capability bools (`DebugOutput`, `DebugOutputSynchronous`, `Dither`, `LineSmooth`, `PolygonOffsetLine`, `PolygonOffsetPoint`, `PolygonSmooth`, `SampleAlphaToCoverage`, `SampleAlphaToOne`, `SampleCoverage`, `ProgramPointSize`) and the three new bools of D3. Reason: the alternative — demoting those setters to `++m_version` — changes `MG_State` semantics in the **pull** build and breaks G1, and it is a behaviour change to the pull path for the sake of the push path. Growing the subset costs nothing measurable: hashing happens only when `m_pipelineStateVersion` moves, which is exactly when Magma already recomputes `ComputePipelineStateHash` today, and a scissor or sample-coverage toggle now mints a *second cached* CSO whose handle is reused thereafter, the same shape as the Blaze3D blend toggle the CsoCache is built for.
- **[deviation] against `MGPipeTypes.h:232-235`**, whose comment puts "sample coverage" in the dynamic half. `SetSampleCoverage` (`RenderState.cpp:778-784`) calls `BumpVersions()`, so under the rule it is pipeline. The implementer fixes that comment in the same commit (hints, the point-size family, clear values, blend colour, line width, polygon offset, stencil ref/write mask, viewport, scissor and depth range all stay dynamic and the comment is right about them).
- **[deviation] the patch trio is in the pipeline half *and* is carried by `set_patch_state`.** `Coverage.def:62-64` routes the three patch accessors to `SetPatchState`; `kMGPipePipelineStateMembers` lists all three. Both are right for different reasons: they are pipeline state (`VkPipelineTessellationStateCreateInfo::patchControlPoints`, hashed at `VulkanRenderer.cpp:4864-4878`) **and** a shader-variant input both backends bake into the synthesized control stage (`MGPipeTypes.h:498-499`, read from a shader-build path at `Managers.cpp:7194-7205`, not a draw path). So the bytes travel twice — 28 bytes on a state that changes about once per program — and the applier asserts under verify that the two carriers agree. Redundancy here is a trip wire, not waste.

**The table.** `MGPipeRenderStateSpans.cpp` computes every boundary with `offsetof`/`sizeof` — never a literal — and carries `static_assert`s that the chunks are sorted, non-overlapping and cover exactly `[0, sizeof(RenderStateParameters))`. The offsets below are what this brief computes; **the file must derive them, and if it disagrees the file is right and this table is wrong.**

| # | set | range | size | members |
|---|---|---|---|---|
| D0 | dynamic | `[0, 264)` | 264 | `Viewports[16]`, `LineWidth`, `PointSize` |
| P0 | pipeline | `[264, 292)` | 28 | `PatchVertices`, `PatchDefaultOuterLevel`, `PatchDefaultInnerLevel` |
| D1 | dynamic | `[292, 312)` | 20 | `PolygonOffsetFactor/Units/Clamp`, `ClipOrigin`, `ClipDepthMode` |
| P1 | pipeline | `[312, 584)` | 272 | `BlendStates[8]`, `LogicOp`, `DepthTestEnabled`, `DepthFunc`, `DepthMask`, `ColorMasks[8]`, **`FramebufferSrgbEnabled`, `DepthClampEnabled`, `TextureCubeMapSeamlessEnabled`** |
| D2 | dynamic | `[584, 752)` | 168 | `ClearColor`, `ClearDepth`, `ClearStencil`, `BlendColor`, `DepthRanges[16]` |
| P2 | pipeline | `[752, 772)` | 20 | `SampleCoverageValue`, `SampleCoverageInvert`, `SampleMaskValue`, `MinSampleShadingValue` |
| D3 | dynamic | `[772, 784)` | 12 | `StencilStates[0].{Ref, ValueMask, WriteMask}` |
| P3 | pipeline | `[784, 800)` | 16 | `StencilStates[0].{FailOp, PassDepthFailOp, PassDepthPassOp}`, `StencilStates[1].Func` |
| D4 | dynamic | `[800, 812)` | 12 | `StencilStates[1].{Ref, ValueMask, WriteMask}` |
| P4 | pipeline | `[812, 840)` | 28 | `StencilStates[1].{FailOp, PassDepthFailOp, PassDepthPassOp}`, `CullFaceEnabled`, `CullFaceModeSetting`, `FrontFaceModeSetting`, `ProvokingVertexModeSetting` |
| D5 | dynamic | `[840, 868)` | 28 | the four hints, `PointFadeThresholdSize`, `PointSpriteCoordOrigin`, `ClampReadColor` |
| P5 | pipeline | `[868, 876)` | 8 | `PolygonModeFront`, `PolygonModeBack` |
| D6 | dynamic | `[876, 880)` | 4 | `PrimitiveRestartIndex` |
| P6 | pipeline | `[880, 904)` | 24 | the 20 capability bools `ColorLogicOpEnabled`…`ProgramPointSizeEnabled`, `ScissorTestEnabledMask` |
| D7 | dynamic | `[904, 1168)` | 264 | `ScissorBoxes[16]`, `ScissorBoxWrittenMask`, `ClipDistanceEnabledMask` |

**7 pipeline chunks totalling 396 bytes; 8 dynamic chunks totalling 772 bytes; 396 + 772 = 1168.** Both counts fit the `Uint32 ChunkMask` of `MGPRenderStateDesc` (`MGPipeTypes.h:215-221`) and `MGPDynamicState` (`:236-241`) with room to spare. `StencilStates` is the one member that straddles, at sub-member granularity, exactly as `scout-render-state.md` §7.2 predicted; **`StencilFaceState` is not reordered** — reordering would move Espryt's shadow bytes for no gain and would invalidate the offsets above.

Note the two splits are orthogonal and coexist (`ARCHITECTURE.md:188`): Espryt's head `[0,312)` / blend `[312,536)` / tail `[536,1168)` cuts across this table, and D0/P0/D1 all live in Espryt's head while P1 spans Espryt's blend and tail. Nothing about Espryt's spans changes.

`MGPipeComputePipelineSubsetHash(const RenderStateParameters&)` is `XXH64` over the seven pipeline chunks in ascending order, seeded with a compile-time table version so a chunk-table change invalidates every persisted key. 396 bytes ≈ 49.5 eight-byte words — **[deviation]** the doc estimates "~25-30 字" (`ARCHITECTURE.md:189`), which was the 24 hashed members without padding and without the 15 members added above; the integrator records 396 bytes / 49.5 words in `MEASUREMENTS.md`.

### D7 — The CsoCache

`MobileGL/MG_Impl/Pipe/CsoCache.{h,cpp}`, one instance per context, held by the tracker.

- Storage: `ska::flat_hash_map<Uint64, Uint32>` (hash → cache-entry index) plus a `Vector<Entry>` of at most **64** entries and an intrusive LRU list. `Entry = { Uint64 Hash; MGPipeHandle Cso; Array<Uint8, 396> PipelineBytes; }` — 64 × ~412 B ≈ 26 KB per context.
- Capacity **64** (`ROADMAP.md:18`, `ARCHITECTURE.md:63`). `ROADMAP.md:77` says 64 is provisional and the counters retune it; P2 ships 64 and publishes the mint/bind/evict counters that a P13 retune reads.
- **[deviation] a hash hit is confirmed with a `memcmp` of the 396 pipeline bytes before the handle is reused.** `ARCHITECTURE.md:63` says content addressing on an xxHash; a bare 64-bit hash equality would let a collision alias two different render states onto one CSO, which is a silent wrong-pixels bug with no gate that can see it. Mesa's `cso_cache` memcmps for the same reason. The memcmp runs only when `m_pipelineStateVersion` moved, i.e. never in the steady state.
- Eviction is LRU and emits `DeleteRenderState` (`PipeCalls.def:94`), which frees the applier's slot and bumps its `Gen`.
- **The lookup algorithm** (`ARCHITECTURE.md:189` verbatim, implemented):
  1. `m_pipelineStateVersion` (widened) unchanged → reuse `m_lastCso`, **zero hashing, zero probing**; emit nothing unless `m_version` also moved.
  2. changed → hash the seven pipeline chunks → probe → **hit**: `memcmp` confirms, emit `MGPBindRenderState` (12 B). **miss**: mint a slot, emit `MGPRenderStateDesc` with the changed chunks (`ChunkMask`; all-ones for a brand-new CSO, else the chunks that differ from `BaseCso`), then the 12-byte bind.
  3. `m_version` moved but the pipeline subset did not → emit `MGPDynamicState` only.
- Counters: `PipeStats::AddCalls(CallClass::RenderStateCsoMints, 1)` and `…CsoBinds, 1)` (two new call classes, D17).

### D8 — What `set_dynamic_state` carries

`MGPDynamicState{ ChunkMask, Version, Pad0, Blob }` (`MGPipeTypes.h:236-241`, 32 B) plus a blob that is the concatenation, in ascending chunk index, of the dynamic chunks whose bit is set in `ChunkMask`. `Version` is `m_version` (widened `Uint16`, sent as the raw `Uint16` the wire type declares).

**Granularity: per chunk, eight bits, chosen by `memcmp` of each dynamic chunk against the tracker's staging copy.** So a `glViewport` sends chunk D0 (264 B), a `glClearColor` sends D2 (168 B), a `glScissor` sends D7 (264 B), a `glStencilMask` on the front face sends D3 (12 B). `ROADMAP.md:77` defers the final granularity to the counters; P2 ships eight chunks and gives `PipeStats::RecordDrawPayloadBytes` (`PipeStats.h:154`, implemented, unit-tested and **called by nothing** today) its first emitter, so the 24-bucket histogram answers the retune question with data. `ARCHITECTURE.md:189`'s "~200 B" estimate is in the right range; the measured per-chunk sizes above go into `MEASUREMENTS.md`.

The tracker keeps `RenderStateParameters m_staged{}` (value-initialised) as the "what the server has" mirror; a chunk that memcmp-matches is not sent, which is the chunk-level suppressor.

### D9 — The residual value block, its ratchet and its tripwire

After P2, `MGPipeTypes.h:516-525` becomes:

```cpp
    struct ResidualValueBlock {
        Uint64 CapabilityBits; // the 35 CapabilityInput bits, packed in enum order
    };
```

`#define MGL_RESIDUAL_BLOCK_SIZE 8` (down from 1248 — `RenderState` 1168 retires to the CSO, `Pack` 28 to `set_pixel_pack_state`, the patch quintet 52 to `set_patch_state`). The ratchet comment at `MGPipeTypes.h:527-535` stays; the number only ever goes down.

**The three disciplines (`ARCHITECTURE.md:359`), implemented:**
1. **Retirement is a compile error.** The `static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE, …)` at `:537` is what fires if a field is removed without lowering the number, or added at all.
2. **Per-member `offsetof` assertions, and field-wise serialisation under split.** `gen_pipe.py` emits `static_assert(offsetof(ResidualValueBlock, CapabilityBits) == 0);` from `MGP_FIELDS_ResidualValueBlock` (`PipeFields.def:147-148`, which loses five of its six rows).
3. **`MOBILEGL_PIPE_STATS` counts its bytes as their own class** — `ByteClass::ResidualValueBlock` (`PipeStats.h:73-75`), the placeholder that "stays at 0 until P2". P2 makes it non-zero (G10).

**The tripwire.** `CapabilityBits` is now *redundant*: every one of the 35 capabilities is answerable from the working `RenderStateParameters` once D3 closes the three storage holes. That redundancy is the point. `MGPipeApplySetResidualValueState` compares, bit by bit, the block's answer against `MGPipeDeriveRenderStateFields`' answer, and a disagreement is `Fatal{PipeResidualDiverged, "<CapabilityName>"}` in a poison or verify build (counted and logged otherwise). So the day a later call takes a capability over and forgets to carry it, the residual block says so on the next draw. The block is emitted once per context and again whenever the capability set changes.

### D10 — `set_pixel_pack_state`, `set_patch_state`, `set_vertex_attrib_defaults`

| call | payload | shutter | emitted when |
|---|---|---|---|
| `SetPixelPackState` (`PipeCalls.def`, `kCtxState`) | `MGPPixelPackState{ PixelStoreParameters Pack; }`, 28 B (`MGPipeTypes.h:488-496`) | `BitwiseEqual` against the tracker's copy | `NEW_PIXEL_PACK`. Applier writes `PipeInputs::m_pixelStore[0]`. **PACK only** — there is deliberately no unpack counterpart (`ARCHITECTURE.md:107`, `MGPipeTypes.h:485-487`). |
| `SetPatchState` | `MGPPatchState{ Vertices, Pad0, Outer[4], Inner[2], Pad1[2] }`, 40 B (`:500-507`) | `BitwiseEqual`, **NaN legal** (`ARCHITECTURE.md:162`: a NaN outer level is a legal `glPatchParameterfv` value and must compare equal to itself) | `NEW_PATCH_STATE`. Applier writes the three members of the working block **and** asserts they match what the pipeline chunk P0 delivered (D6). |
| `SetVertexAttribDefaults` (`kVarTail`) | `MGPVertexAttribDefaults{ Mask, Count }`, 8 B (`:479-483`), plus a tail of `Count` `MGPAttribValue`s for the attributes named by `Mask` | `ContentHash` (xxHash over the emitted set) through the set-hash suppressor (D11) | `NEW_VERTEX_ATTRIB_DEFAULTS`. Applier writes `PipeInputs::m_currentVertexAttribute[i]` for the named attributes. |

`BitwiseEqual` means `std::memcmp` over the value's bytes, which is why D3's `{}` matters for `PixelStoreParameters` (2 padding bytes at offsets 2–3).

### D11 — The set-hash suppressor skeleton

`MobileGL/MG_Impl/Pipe/SetHashSuppressor.h` — header-only:

```cpp
    // Coalescing rule 4 (ARCHITECTURE.md:198): every kVarTail set_* hashes the RESOLVED set on
    // the client and does not emit when the hash has not moved. This is the carrier for the
    // ~175 lines of debounce that move off the backends in P3b/P4b; P2 lands the mechanism and
    // one real consumer so the shape is pinned by a test rather than by a plan.
    class SetHashSuppressor {
        Array<Uint64, kMGPipeSuppressorSlotCount> m_lastEmitted{};   // 0 == "never emitted"
    public:
        Bool ShouldEmit(MGPipeSuppressorSlot slot, Uint64 contentHash);   // and latches
        void Invalidate(MGPipeSuppressorSlot slot);                        // context change, server reset
        void InvalidateAll();
    };
```

Slots, one per `kVarTail` `set_*` (`ARCHITECTURE.md:145`): `SetVertexBuffers`, `SetSamplerViews`, `BindSamplerStates`, `SetShaderImages`, `SetShaderBuffers`, `SetStreamOutputTargets`, `SetVertexAttribDefaults`. **P2 wires exactly one — `SetVertexAttribDefaults` — and unit-tests the class on all seven slots.** The other six are P3b/P4b (`ROADMAP.md:23`). A hash of 0 is reserved for "never emitted" so the first emission always goes out (the class remaps a computed 0 to 1).

### D12 — Magma re-sourced from the CSO and the dynamic payload

Nothing here is required for correctness — after D2 the working block is byte-identical to what Magma reads today, so an unmodified Magma is already correct under push. Everything in D12 is a **cost** deliverable, which is what makes it safe to land last.

1. **`GetOrCreatePipeline`'s memo key** (`VulkanRenderer.cpp:4997-5013`): `entry.pipelineStateHash == pipelineStateHash` becomes `entry.renderStateCso == boundCso` (an `MGPipeHandle` compare). The version→hash gate at `:4985-4995` and the three cached-hash fields (`VulkanRenderer.h:855,856,860,861,862`) **are deleted**: the client already did the hashing, and `m_pipelineStateHashValid`/`…Version`/`…ColorCount`/`…SampleCount` existed only to avoid re-hashing. `InvalidatePipelineMemo()` (`:881-885`) keeps clearing the memo and loses the three hash fields.
   **The render-pass facts stay in the key.** `entry.renderPassHash` already separates draws that differ only in `colorAttachmentCount`/`sampleCount` (correction 4 of the preamble), so collapsing `pipelineStateHash` to the CSO handle loses no discrimination. `ResolveEffectiveSampleMask` (`:4813-4819`) is **not** deleted — it is a *payload* computation that depends on `rasterizationSamples`, and it keeps reading `Multisample`/`SampleMask`/`SampleMaskValue` out of the working block.
2. **`ComputePipelineStateHash`** (`:4821-4910`) is retained only under `MOBILEGL_PIPE_LEGACY_MEMOS` and is not called on the new arm. Its 20-line enumeration comment (`:4780-4794`) moves to `MGPipeRenderStateSpans.cpp` as the provenance of the pipeline chunk set, since that is now the contract it was.
3. **`ApplyDynamicDrawStateTail`** (`:5902-6004`): the version gate at `:5920-5930` keeps reading `GetRenderStateParametersVersion()` — that value now comes from `MGPDynamicState::Version`, so it moves exactly when a dynamic chunk moved and no longer moves on a pipeline-only change. The `DynamicTailKey` build and compare (`:5944-5982`) stay; the `struct DynamicTailKey` (`:355-395`) and its input inventory (`:341-354`) are already an exact enumeration of the dynamic half and are the cross-check that D6's dynamic chunks are complete — the implementer asserts, in a unit test, that every field `DynamicTailKey` reads lies inside `kMGPipeDynamicChunks` (the two exceptions being `extentX`/`extentY`/`preTransform`/`isDefaultFbo`, which are backend facts, not GL state).
4. **Subsystem 4** (Track H): `VertexInputStateFactory::ComputeHash`'s `const Uint64 bufferKey = attr.Buffer ? attr.Buffer->GetLifetimeId() : 0;` (`VertexInputStateFactory.cpp:48`) becomes `bufferHandle.Slot | (Uint64(bufferHandle.Gen) << 32)` — `ARCHITECTURE.md:363`'s "`lifetimeId` → `gen` 混进 server 侧每个 content hash". `VaoDrawMemo` (`VulkanRenderer.h:1244-1264`) drops `vaoKey` (raw pointer) and `vaoLifetimeId` for one `MGPipeHandle vaoHandle`; `LookupVaoDrawMemo` (`:3540-3579`) becomes a direct slot index into a grow-on-demand `Vector` with a `Gen` compare — no Fibonacci mix (`:3547-3548`), no 2-way probe, no frame-serial recycling (`:3564-3577`).
5. **The frontend VAO loses its backend pointers.** `VertexArrayObject::{Get,Set}BackendStateMemo` and `m_backendStateMemo{,Epoch,Version}` (`VertexArrayObject.h:121-131`, `:192-194`) are **deleted outright** (`ARCHITECTURE.md:284`), and with them the eviction-epoch dance (`VertexInputStateFactory.cpp:78`, `:318`, `VertexInputStateFactory.h:141`, `s_evictionEpochSource`) — a slot-indexed table has stable entries. `{Get,Set}BackendHashMemo` and `{Get,Set}BackendAuxMemo` (`:106-114`, `:140-150`, storage `:190-191`, `:195-197`) become fields of the slot-indexed `VaoDrawMemo` entry. `SetBackendAuxMemo` has **no live reader** today (verified: its only writer is `VertexInputStateFactory.cpp:83-84`; the values moved into `VaoDrawMemo::layoutHash`/`layoutAuxMasks`, `VulkanRenderer.h:1257-1259`), so it is deleted rather than moved.
   **Out of scope, do not touch:** `ProgramObject::{Get,Set}BackendHashMemo` (`ProgramObject.h:798-826`, used by `ProgramFactory.cpp:3446-3448`), and `SetupDrawSnapshot`'s wider re-key (`VulkanRenderer.h:958-979`) beyond replacing its `vao`/`vaoLifetimeId` pair with the handle.

### D13 — Espryt 0b: what the first Track H slice replaces and what it must not break

**New**: `MobileGL/MG_Impl/Pipe/SlotAllocator.{h,cpp}` (client side, in the contract commit because Magma needs it too) and `MobileGL/MG_Backend/DirectGLES/SlotTables.h` (per-kind dense twin arrays).

`SlotAllocator` obeys `MGPipeHandles.h` exactly: per-`MGPipeKind` free list + high-water mark, first allocatable slot `kMGPipeFirstAllocatableSlot = 1` (`:81`), **`Gen` bumps only on slot reuse, never on respecify** (`:44-48`), a `MOBILEGL_ASSERT` on `Gen` wrap in a debug allocator, and a client-side `lifetimeId → slot` map per kind (`ARCHITECTURE.md:40`) so `GetLifetimeId()` stays the client's identity and a GL name never enters a key.

**Replaced / deleted** (all in `MG_Backend/DirectGLES/`):

| what | where | replacement |
|---|---|---|
| the six `StateBackendObjectRegistry` instances | `Managers.h:271-400`; instances at `Managers.h:803,1124,1215,1733,1833,1860`, definitions `Managers.cpp:2747,5075,5828,6139,8657,8769` | per-kind dense arrays indexed by `MGPipeHandle::Slot`, `Gen`-validated |
| `Find`'s erase-on-expiry and the "a reference dies on the next call" hazard | `Managers.h:326-349` | O(1) indexed read; **`SyncTextureObjectToBackend`'s by-value copy and second `Find` (`DirectGLES.cpp:1310-1345`) are deleted in the same change**, not left as harmless |
| `CollectGarbage` / `CollectGarbageIfNeeded` (7 call sites: `DirectGLES.cpp:1223,1533,1898,1899,1900,2728,2729`) | `Managers.h:356-391` | explicit destroy: `MarkXForDeletion` on the frontend emits `delete_*`, and the applier frees the slot |
| `OwnerEquals` and the three `TwinLookupMemo`s | `DirectGLES.cpp:63-66`, `:83-132`; use sites `:1206,1214,2769,2777,2874,2879` | direct slot indexing — the memo existed only to avoid the hash probe |
| `UnitSamplerLookupMemo`'s `WeakPtr` test | `DirectGLES.cpp:3157-3177` | `{slot, gen}` compare; the "a miss is never cached" contract (`:3148-3156`) survives verbatim |
| `g_fbSlotCache` / `GetFramebufferBindingSlotFast` and its five callers (`:1613, 1907, 2745, 2860, 2935`) | `DirectGLES.cpp:134-157` | an ordinary `MGB_CTX->GetFramebufferBindingSlot(target)` read of pushed state. **This closes the P1 poison bypass** (`scout-track-h.md` §1.6): the fast getter ran the checked accessor once per context change and then handed out a raw pointer forever, so the per-verb poison stamp and the verify read-hook were skipped at those five sites. |
| the four `pDefaultFramebufferInfo->defaultFBO` identity compares (`:1939, 2874, 2898, 6420`) | | `handle == kMGPipeDefaultFramebuffer` (`MGPipeHandles.h:71`) |

**Must not break** (each gets a named assertion or an existing test):

- `EnsureProcessTeardownSentinel()` (`Managers.h:59-60`, `Managers.cpp:161-170`) is armed by the first twin creation, today from `GetOrCreate` (`Managers.h:295`). The arming site moves to the slot table's first insertion. A registry destructor hook is **wrong** and the comment at `Managers.h:52-57` says why.
- The twin destructors' `g_backendContextGeneration` compare (`Managers.h:64-70`) so a twin outliving its ES context does not `glDelete*` a recycled name. Pinned by `SanityTest.cpp:2650-2665, 2765-2773, 2786-2790`.
- The whole-registry save / reset / restore fixture `ScopedDirectGLESTextureBindings` (`SanityTest.cpp:145-197`, `:160,164,178,195`) must keep working — the slot table needs the same copy-assign-and-restore shape.
- The one direct-iteration site, `ScopedDetachedTextureFramebufferAttachments`' scan over all framebuffer twins (`DirectGLES.cpp:6413-6425`), needs a "which slots are live" bit per kind.
- `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged` (`DirectGLES.cpp:1366-1400`) **stays on the backend** — moving that debounce to the client is P3b/P4b (`ROADMAP.md:23`) — but its two `OwnerEquals` calls (`:1392`, `:1394`) become `{slot, gen}` compares.
- `g_attachmentBackendIdGeneration` (`DirectGLES.cpp:1891`), `BufferImpl::g_bufferBackendIdGeneration` (`Managers.h:800`) and `InProcessTeardown()` are *driver-object* epochs, not identity, and survive untouched.
- Purity gate C: `grep -rc pGLContext MobileGL/MG_Backend` stays empty. The `g_fbSlotCache` rewrite is the one place tempted to break it.

**Explicit destroy.** Today nothing tells the backend an object died except buffers (`BufferBackendOps::OnDestroy`, `BufferObject.h:103`, fired from `BufferObject.cpp:39-41`); `Managers.h:306-314` quantifies the resulting dead gigabytes. P2 adds the death notification for the six kinds by emitting `delete_*` from the frontend's `Mark*ForDeletion` path (`Core.cpp:116-140, 167-169, 279-307, 346-352, 1188-1190, 1213-1225, 1252-1254`) when the object's last `SharedPtr` drops — **not** when the name is marked, because a still-bound object keeps living (`TextureState.cpp:118-155`). The hook follows `BufferBackendOps`' shape.

### D14 — `MOBILEGL_PIPE_LEGACY_MEMOS`: the compile-time arm and the runtime selector

`ARCHITECTURE.md:367` is right that after phase C the `MOBILEGL_PIPE_PUSH` bitmap alone is not a valid A/B, because with a bit clear the backend still runs the re-keyed memo code. So:

| switch | kind | default | meaning |
|---|---|---|---|
| `MOBILEGL_PIPE_LEGACY_MEMOS` | **new CMake `option()`** + `-DMOBILEGL_PIPE_LEGACY_MEMOS=1` | **ON** | compiles the pre-handle arm (the six registries, `OwnerEquals`, the three `TwinLookupMemo`s, `g_fbSlotCache`, `ComputePipelineStateHash`, the address-keyed `VaoDrawMemo`) alongside the handle arm, both behind the same `PipeInputs` interface. Forced ON with a `message(STATUS)` when `MOBILEGL_PIPE_PUSH=OFF`, where it is the only arm. |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | runtime, `Features.PipeLegacyMemos` (`Config.h:356-360`, `ConfigLoader.cpp:258-259`) — **already exists**, tri-state, only an explicitly falsy value turns it off | true | true: a Track-H subsystem whose bitmask bit is clear falls back to the legacy arm. false: the legacy arm is never entered, and a Track-H subsystem whose bit is clear is a startup `Fatal{PipeLegacyMemosDisabled}`. This is the lever `HandleRecycleScenario`'s arms use. |
| `MOBILEGL_PIPE_PUSH` | runtime `Uint64` bitmask, `Features.PipePush` (`Config.h:319-326`, `ConfigLoader.cpp:245`) — **first consumed in P2** | see below | per-subsystem selector. |

**The subsystem bit enum** (new, `MobileGL/MG_Pipe/MGPipe.h`, next to the call-class enum at `:31`):

```cpp
    inline constexpr Uint64 kMGPipeSubsystemRenderState          = 1ull << 0;
    inline constexpr Uint64 kMGPipeSubsystemPixelPack            = 1ull << 1;
    inline constexpr Uint64 kMGPipeSubsystemPatchState           = 1ull << 2;
    inline constexpr Uint64 kMGPipeSubsystemVertexAttribDefaults = 1ull << 3;
    inline constexpr Uint64 kMGPipeSubsystemResidualValues       = 1ull << 4;
    inline constexpr Uint64 kMGPipeSubsystemEsprytSlots          = 1ull << 5;  // Espryt 0b
    inline constexpr Uint64 kMGPipeSubsystemMagmaVertexInput     = 1ull << 6;  // Magma subsystem 4
    // bits 7..62 reserved for the later phases, allocated in ROADMAP order.
    // NOT a subsystem, a BEHAVIOUR: turn OFF client-side content addressing of CSOs, so every
    // pipeline-version change mints a fresh CSO and the map is never probed. This is the
    // negative control the CSO design is measured against (ROADMAP.md:18, ARCHITECTURE.md:367).
    inline constexpr Uint64 kMGPipeBehaviourNoCsoContentAddressing = 1ull << 63;
    inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP2 = 0x7full;  // bits 0..6
```

**Default.** `Config.h:322-323` documents `0` as "pull everything", and that stays literally true. `ConfigLoader.cpp:245` becomes, under `#if MOBILEGL_PIPE_PUSH`, `QueryEnvUint64("MOBILEGL_PIPE_PUSH", kMGPipeSubsystemsMigratedAtP2)` and, in the pull build, keeps its `0` (where it is meaningless). So a push build with the knob unset runs every migrated subsystem, and `MOBILEGL_PIPE_PUSH=0` in the environment is the all-subsystems-pull control that reproduces P1's behaviour exactly. `QueryEnvUint64`'s decimal/`0x` contract (`ConfigLoader.cpp:167-192`) is unchanged; the Config.h comment gains the bit list because operators pass it as hex.

### D15 — How the pull build stays symbol-identical

Every P2 artefact lands in one of four places, and none of them touches the pull build:

1. **New sources compiled only under `MOBILEGL_PIPE_PUSH`**, appended inside the existing `if (MOBILEGL_PIPE_PUSH)` block (`CMakeLists.txt:462-468`): `MG_Pipe/MGPipeRenderStateSpans.cpp`, `MG_Pipe/PipeApply.cpp`, `MG_Impl/Pipe/{Tracker,CsoCache,SlotAllocator}.cpp`. `generated/PipeSpanTable.inc:66-67`'s two `extern` declarations cost the pull build nothing (a declaration emits no symbol).
2. **`#if MOBILEGL_PIPE_PUSH` arms inside existing backend files** for the Track H re-keys, with the legacy arm under `#if MOBILEGL_PIPE_LEGACY_MEMOS` (D14). In the pull build only the legacy arm is compiled and it is textually today's code.
3. **`MGP_NOTE_AGGREGATE` and the five aggregate generations** in `MG_State`, guarded the same way, so the pull build's state objects do not change size (D4).
4. **Three new members in `RenderStateParameters` plus three `SET_CAPABILITY`/`RETURN_CAPABILITY` arms plus three `{}`**, which *are* in the pull build. This is the one deliberate exception, and it is bounded: the members fit an existing padding hole so no offset and no `sizeof` moves; the switch arms grow two functions; the `{}` grows one constructor. **G1 therefore admits exactly the resizes of `RenderState::SetCapability`, `RenderState::IsCapabilityEnabled` and `RenderState::RenderState()`, named in the commit message, and zero added/removed/renamed symbols.**

The check after every merge is `symbol_report.py --threshold 0`; anything outside that named set is a defect, not a rounding error.

### D16 — The dirty-surface mapping file and its gate

**New file `MobileGL/MG_Pipe/DirtySurface.def`**, one row per distinct mutator the scanner finds (73 today):

```
// X(Mutator, Answer) - what publishes this frontend mutation to the backend.
// Answer is one of the MGPipeDirty bit names, or:
//   kImmediate      - the mutating function also reaches the backend in the same body, so the
//                     mutation is published inline and needs no aggregate generation
//   kReverseChannel - not state: a write INTO the frontend from the backend's side (RecordError)
//   kNoBackendRead  - no backend read point observes this state at all (with the read-point
//                     inventory row that proves it)
#define MGP_DIRTY_SURFACE_LIST(X) \
    X(RecordError,                  kReverseChannel) \
    X(SetActiveTextureUnit,         kImmediate)      \
    X(SetPatchVertices,             kImmediate)      \
    X(SetBlendColor,                NEW_RENDER_STATE) \
    X(SetCapability,                NEW_PIPELINE_STATE) \
    ...
```

`scripts/gen_pipe_dirty_surface.py` grows:
- `--check`: parse `DirtySurface.def`, and exit 1 if any scanned mutator has no row, or any row names a mutator the scan no longer finds. Both directions, so a deleted mutator leaves no stale row.
- `--self-test`: two canned negative controls (a mutator withheld from the def, a row naming a nonexistent mutator), each of which must trip; `trips == 0` is an error — the shape `check_include_closure.py:541-543` and `gen_pipe.py --self-test` already use.
- the human report writes the mapped answer instead of `UNMAPPED`, and the summary line is regenerated into `docs/Disaggregated/MEASUREMENTS.md`'s §5 by the integrator.

`RecordError` is **exempted explicitly by a row**, not by a filter: it is 836 of the 926 calls (90 % of the surface) and it is a reverse channel (`Coverage.def:99`, `PipeInputs.h:574`). That leaves 72 real mutators over 90 real calls.

The scanner's three self-declared blind spots (`gen_pipe_dirty_surface.py:195-197` and its scan root) are recorded in the def's header comment as known gaps: a mutator inside a lambda is attributed to the enclosing function; a mutation published through a helper the entry point calls reads as deferred; and the scan root is `MG_Impl/GLImpl` only, so the four `MGP_NOTE_MUTATION` sites in `MG_State/GLState/TextureState/TextureState.h:85,98,111,133` are outside it entirely.

CI: `.github/workflows/test.yml:1524-1528` loses its "Informational" comment and becomes `python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test`. Cost: the `pipe-gates` job runs in ~10 s and has no build dependency (`test.yml:1480-1481`); this adds nothing measurable.

### D17 — Counters, and the rule against hot-path instrumentation

`ROADMAP.md:7` forbids committing hot-path instrumentation. **The tracker gets no timer.** The absolute-ns number comes from `DriverBench`'s `ns_per_op` (D.4), which times whole frames from outside the library.

Two new `CallClass` values only — `RenderStateCsoMints` and `RenderStateCsoBinds` — with short names `csom` / `csob`, added in the contract commit together with the `PipeStatsTest.cpp:260 CounterNamesAreStable` update that pins them. Everything else P2 needs already exists:

- `ByteClass::ResidualValueBlock` (`PipeStats.h:73-75`) — the placeholder P2 makes non-zero.
- `RecordDrawPayloadBytes` + the 24-bucket histogram (`PipeStats.h:129,154`) — implemented, unit-tested, **called by nothing**; the tracker becomes its first emitter, one call per emitted draw payload.
- the six memo gates (`PipeStats.h:107-122`) — unchanged names, and they are the honest reading of the before/after (`PipeStats.cpp:93-99`).
- `CallClass::AccessorCalls` — **a warning for the report**: it is a set of static tallies at ~10 hot entry points (`PipeStats.cpp:76-88`), so a tracker that *relocates* reads out of those functions scores a lower `acc/draw` without having removed any work. P2's report must therefore lead with the gate hit/miss pairs and the CPU-time series, and quote `acc/draw` only alongside a re-audit of the tally constants (`DirectGLES.cpp:1421,1524,2043,2053,2977`; `VulkanRenderer.cpp:5009,5016,5274,5927,5934,6416,6449,6462`).

Every counting site keeps the `if (PipeStats::Enabled())` shape (`PipeStats.h:29`) so an off build pays one predicted branch.

### D18 — `HandleRecycleScenario`, and how it goes red before the re-key

New `MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp`, registered in `MG_IntegrationTest/CMakeLists.txt` (source list from `:55`), following `PipeVerifyArmingScenario` / `PoisonOmissionScenario` as the template for an always-on negative control (`CMakeLists.txt:700-725`).

**What it reproduces**, through public GL only: delete a VAO that a backend memo slot holds; immediately create a replacement the allocator is very likely to place at the same heap address (the `void* volatile` sink trick from `MG_Test/State/ObjectLifetimeIdTest.cpp:44`) **and** give it a byte-identical attribute configuration so the content hash matches too; bind a *different* buffer to it; draw; read back and assert the pixels come from the new buffer (the red/green quad readback of `Scenarios/CrossFrameBufferScenario.cpp`). The same shape is then repeated for a texture (against `g_backendTextureObjects`) and a framebuffer.

**Three ctest arms**, all always-on:

| arm | environment | expected |
|---|---|---|
| `…HandleRecycleScenario.Handles` | `MOBILEGL_PIPE_PUSH` default (bits 5 and 6 set), `MOBILEGL_PIPE_LEGACY_MEMOS=0` | **green** — the `{slot, gen}` key cannot alias |
| `…HandleRecycleScenario.Legacy` | `MOBILEGL_PIPE_PUSH=0`, legacy arm | **green** — today's `lifetimeId` + `weak_ptr` guards work |
| `…HandleRecycleScenario.AbaControl` | `MOBILEGL_PIPE_PUSH=0` **and** `MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1` | **green by asserting the corruption** — the knob makes `VertexInputStateFactory::ComputeHash` hash `attr.Buffer.get()` instead of `GetLifetimeId()` and makes `LookupVaoDrawMemo` skip the `vaoLifetimeId` compare, i.e. reverts exactly the two guards the re-key replaces; the scenario asserts the *wrong* pixels, so if the scenario ever stops reproducing the ABA it fails |

`MOBILEGL_PIPE_HANDLE_ABA_CONTROL` is a new `Bool Features.PipeHandleAbaControl` under `#if MOBILEGL_PIPE_PUSH` (so it cannot exist in a shipping pull build), parsed with `QueryEnvFlag`, documented next to `MOBILEGL_PIPE_VERIFY_CORRUPT` as negative control C. That third arm is what makes "`HandleRecycleScenario` 绿且重键前红" (`ROADMAP.md:18`) an always-on CI fact instead of a one-off manual demonstration.

### D19 — G7: the setter-consistency test and its negative control

`MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp` (contract commit creates it; package A fills it):

- `ChunkTablePartitionsTheBlock` — the sorted/non-overlapping/complete assertion is a `static_assert` in `MGPipeRenderStateSpans.cpp`; the test re-asserts it at run time so a reader sees it, and additionally asserts `kMGPipePipelineChunks` covers every member named in `kMGPipePipelineStateMembers` (`generated/PipeSpanTable.inc:34-59`).
- `SetterConsistency` — the G7 test. Drive **every public setter** of `RenderState` (the 25 Group-A setters and 22 Group-B setters catalogued in `scout-render-state.md` §2.1–2.4, plus `SetPixelStoreParam`, which must move neither counter), each with a value that differs from the current one, and assert `MGPipeComputePipelineSubsetHash` moved **⟺** `GetPipelineStateVersion()` moved. Special cases the test must drive individually, or it does not test what it claims:
  - `SetStencilFunc` **twice**: once changing only `Ref` (version moves, hash must not — `RenderState.cpp:629-642`, the `++m_pipelineStateVersion` at `:641` is conditional on `Func` moving at `:636`), once changing `Func` (both move).
  - `SetPolygonMode` with only the back face changing (both move: `PolygonModeBack` is in P5).
  - `SetCapability(ClipDistance0..7)` (`RenderState.cpp:360-380`): version moves, pipeline version does **not**, hash must not.
  - All 25 `SET_CAPABILITY` names including D3's three new ones.
  - `SetScissorBox` including the `ScissorBoxWrittenMask` transition (`:918-941`).
- `DerivationMatchesTheFrontendGetters` — D5's 29 derived values against `GLContext`'s getters after each setter.
- `DynamicChunksCoverMagmasDynamicTailKey` — every field `DynamicTailKey` (`VulkanRenderer.cpp:341-395`) reads is inside `kMGPipeDynamicChunks`.

**The negative control (G7 in the gate list, `scripts/g7_negative_control.sh`)**: move `ColorMasks` out of chunk P1 into a new dynamic chunk. The partition stays complete so it still compiles; `SetColorMask` then bumps `m_pipelineStateVersion` without moving the hash and `SetterConsistency` fails naming `SetColorMask`. The script patches, builds, expects a non-zero `ctest` rc, and reverts. It is run by the integrator at D.3 and is not a CI lane (it rebuilds the library).

### D20 — Naming, layering and generated-file discipline

- New namespaces: none. Everything is `MobileGL::MG_Pipe`.
- `MG_Pipe/PipeApply.h` forward-declares only; `PipeApply.cpp` may include `MG_State/GLState/RenderState/RenderState.h` (a `.cpp` in the shared directory, no cycle: `RenderState.h` includes `MGPipeValueTypes.h`, not `PipeApply.h`). Run `python3 scripts/check_include_closure.py` after adding it; if it objects, the file moves to `MG_Impl/Pipe/` (the client side is unrestricted) and nothing else changes.
- `PipeCalls.def` is **not** edited: every call P2 emits already has an opcode (`:92-94` `CreateRenderState`/`BindRenderState`/`DeleteRenderState`, `:106` `SetDynamicState`, `:125` `SetResidualValueState`, plus `SetPixelPackState`/`SetPatchState`/`SetVertexAttribDefaults`). Opcodes are file positions, so new calls may only be appended and retired calls keep their slot (`ARCHITECTURE.md:75`); `MGP_CALL_LIST_DOCUMENTED_COUNT = 71` stays pinned by `PipeCatalogueTest.cpp`.
- Generated files are regenerated and committed; CI diffs them (`test.yml:1492-1495`).
- Commit messages: single-line `[Type] (Scope): description`. Never add `Co-Authored-By` or any other attribution line.
- Logging: `MGLOG_D` for anything non-critical; `MGLOG_I` only where `PipeStats.cpp:225-229` already justifies it. No stdio anywhere under `MG_Backend`/`MG_State` (the `pipe-gates` grep, `test.yml:1515-1522`).


---

## C. Packages

**Five packages on five branches, five WSL worktrees.** The split is chosen by the code, not by P1's precedent: the value types + the chunk table + the applier are one shared contract everything else compiles against; the tracker and its caches are one client-side unit that touches no backend file; the two Track H slices are per-backend and share only the `SlotAllocator` (which is therefore in the contract); the gates, the scenario, the CI file and the measurement plumbing are cross-cutting tooling that must be able to land before the thing it gates.

**Branch points.** `p2/contract` from `feat/disaggregated@48268068`. Its single commit `c0` is the contract; `p2/{tracker, espryt, magma, gates}` all branch from the tag `p2/contract`, so their push builds compile and link while A finishes. `p2/spans` continues on from `c0`.

**Tree creation.** `scratchpad/wf4/wsl_p1_tree.sh` copied to `wsl_p2_tree.sh` with `p1`→`p2` throughout, then per slug:

```sh
bash ~/w7/wsl_p2_tree.sh contract 48268068 verify        # first, and it must finish before the rest
bash ~/w7/wsl_p2_tree.sh spans   p2/contract verify
bash ~/w7/wsl_p2_tree.sh tracker p2/contract verify
bash ~/w7/wsl_p2_tree.sh espryt  p2/contract
bash ~/w7/wsl_p2_tree.sh magma   p2/contract
bash ~/w7/wsl_p2_tree.sh gates   p2/contract verify
```

Each tree gets three build directories, flags identical to `~/w7/pipe/build-linux`:

```sh
# pull (the G1 build)
cmake -S . -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_BUILD_TYPE=Release \
  -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON \
  -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
# push
cmake -S . -B build-push   ... same ... -DMOBILEGL_PIPE_PUSH=ON
# verify
cmake -S . -B build-verify ... same ... -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON
# benchmark (package E and the integrator only; DriverBench is Linux-desktop only, CMakeLists.txt:33-35
# force-disables the whole benchmark tree on Android)
cmake -S . -B build-bench  ... same ... -DMOBILEGL_BUILD_BENCHMARK=ON
```

### C.0 Package A — `p2/contract` then `p2/spans` (trees `~/w7/p2-contract`, `~/w7/p2-spans`)

**Commit `c0` — the contract. Nothing in it may change after the tag without telling the other four packages.**

| action | file | what |
|---|---|---|
| MODIFY | `MobileGL/MG_Pipe/MGPipeValueTypes.h` | D3.1 — the three new `Bool`s in the `[581,584)` hole, between `ColorMasks` and `ClearColor`. The `sizeof == 1168` assert at `:541` is the check. |
| MODIFY | `MobileGL/MG_Pipe/PipeFields.def` | D3.2 — three rows in `MGP_FIELDS_RenderStateParameters` (`:230-247`); `MGP_FIELDS_ResidualValueBlock` (`:147-148`) drops to `F(CapabilityBits)` |
| MODIFY | `MobileGL/MG_State/GLState/RenderState/RenderState.{h,cpp}` | D3.3 (three `SET_CAPABILITY` + three `RETURN_CAPABILITY` arms, in `CapabilityInput` order) and D3.4 (`{}` on `m_parameters`, `m_pixelStorePackParameters`, `m_pixelStoreUnpackParameters`) |
| MODIFY | `MobileGL/MG_Pipe/MGPipeTypes.h` | D9 — `ResidualValueBlock` down to `Uint64 CapabilityBits`, `MGL_RESIDUAL_BLOCK_SIZE` 1248 → 8; fix the `MGPDynamicState` comment (`:232-235`) to move "sample coverage" to the pipeline half (D6) |
| MODIFY | `MobileGL/MG_Pipe/MGPipe.h` | D14 — the subsystem / behaviour bit constants |
| CREATE | `MobileGL/MG_Pipe/MGPipeRenderStateSpans.{h,cpp}` | D6 — the chunk table computed with `offsetof`, the partition `static_assert`s, `MGPipeComputePipelineSubsetHash()`, and `VulkanRenderer.cpp:4780-4794`'s enumeration comment moved in as provenance |
| CREATE | `MobileGL/MG_Pipe/PipeApply.{h,cpp}` | D2 + D5 — the seven `MGPipeApply*` entry points, the per-context CSO store, `MGPipeDeriveRenderStateFields()` |
| CREATE | `MobileGL/MG_Impl/Pipe/SlotAllocator.{h,cpp}` | D13 — per-kind `{slot, gen}` allocator, free list + high-water, `lifetimeId → slot` maps, debug wrap assert (in the contract because both C and D need it) |
| MODIFY | `scripts/gen_pipe.py` | extend `PIPELINE_STATE_MEMBERS` (`:75-99`) to the D6 set and rewrite `PipeSpanTable.inc`'s "deliberately absent" block (`gen_span_table()`, `:1040-1072`) to record what P2 decided; emit the `ResidualValueBlock` `offsetof` asserts; emit `kMGPipeFieldEmittedBy[]` from `MGP_COVERAGE_EMITTED_LIST` |
| MODIFY | `MobileGL/MG_Pipe/generated/*.inc` | regenerate and commit |
| MODIFY | `MobileGL/MG_Pipe/Coverage.def` | `MGP_COVERAGE_EMITTED_LIST` (D5); the `GetProvokingVertexMode` note at `:74-77` is answered ("in the pipeline half as of P2") |
| MODIFY | `CMakeLists.txt` | the three new sources inside `if (MOBILEGL_PIPE_PUSH)` (`:462-468`); `option(MOBILEGL_PIPE_LEGACY_MEMOS … ON)` + `-DMOBILEGL_PIPE_LEGACY_MEMOS=1`, forced ON with a `message(STATUS)` when `MOBILEGL_PIPE_PUSH=OFF` (D14) |
| MODIFY | `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | D14 — the bitmask default under `#if MOBILEGL_PIPE_PUSH`, the bit list in the comment; D18 — `Features.PipeHandleAbaControl` |
| MODIFY | `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | D17 — `CallClass::{RenderStateCsoMints, RenderStateCsoBinds}` with short names `csom` / `csob` |
| MODIFY | `MobileGL/MG_Test/Util/PipeStatsTest.cpp` | pin the two new names in `CounterNamesAreStable` (`:260`) and in `SummaryLineCarriesEveryClassAndGate` (`:137`) |
| CREATE (stubs) | `MobileGL/MG_Test/Pipe/{RenderStateSpansTest, TrackerTest, SlotAllocatorTest, CsoCacheTest}.cpp` | each a compiling file with one placeholder `TEST`, so the packages that own their contents never edit `MG_Test/Pipe/CMakeLists.txt` |
| MODIFY | `MobileGL/MG_Test/Pipe/CMakeLists.txt` | register the four stubs, `LABELS unit` |
| MODIFY | `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | `:111-112` residual size 1248 → 8; `PipelineSubsetMembersArePinned` (`:354-358`) count 24 → the D6 count |

Ordered steps for `c0`: value types and `RenderState` first (build pull, confirm `sizeof` is still 1168 and `symbol_report --threshold 0` shows only the three named resizes) → `MGPipeTypes.h` / `PipeFields.def` / generator / regenerate → the three new source files with complete signatures and **stub bodies for `PipeApply`'s derivation** so B/C/D link → CMake / Config → tests. Then **tag `p2/contract`** and tell the other packages.

Message: `[Feat] (Pipe): land the P2 contract - real storage for the three swallowed capabilities, the render-state chunk table and its subset hash, the in-process applier, the slot allocator, the subsystem bitmask and the residual ratchet down to 8`.

**Then `p2/spans` continues** with the work only A can do:

- `c1`: `MGPipeDeriveRenderStateFields()` in full — the 29 derivations of D5, each transcribed from its `RenderState` getter. Message: `[Feat] (Pipe): derive every render-state PipeInputs field from the assembled working block instead of pulling it again from GLContext`.
- `c2`: `RenderStateSpansTest.cpp` in full — D19's four tests. Message: `[Test] (Pipe): walk every RenderState setter and assert the pipeline-subset hash moves exactly when the pipeline version does`.
- `c3`: `SlotAllocatorTest.cpp` — `Gen` moves on slot reuse and only on slot reuse; a freed slot is handed back before the high-water grows; `lifetimeId → slot` survives a recycled heap address (the `void* volatile` sink of `MG_Test/State/ObjectLifetimeIdTest.cpp:44`); slot 0 is never handed out; the composite `ShaderCso` band (`MGPipeHandles.h:83-97`) is never handed out by the ordinary allocator. Message: `[Test] (Pipe): pin the slot allocator's identity contract - gen moves only on reuse and a recycled address never reproduces a handle`.

**Verification (`~/w7/p2-spans`)**

```sh
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/check_include_closure.py
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0
#   expect added 0 / removed 0 / renamed 0; resized == exactly RenderState::{RenderState,SetCapability,IsCapabilityEnabled}
cmake --build build-push --parallel 28 && ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L unit --no-tests=error
bash scripts/g7_negative_control.sh build-push        # must report "negative control tripped" (G7)
sha_of_syncrenderstate () { awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' "$1" | sha256sum; }
sha_of_syncrenderstate MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp   # equal to ~/w7/p2-before-syncrenderstate.sha (G5)
```

### C.1 Package B — `p2/tracker` (tree `~/w7/p2-tracker`, from `p2/contract`)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MobileGL/MG_Impl/Pipe/Tracker.{h,cpp}` | D4 — `MGPipeDirty`, the tracker struct, `m_lastPushed[]` with the widened `Uint16` shutters, the per-class dirty walk, the per-bit fire counters |
| CREATE | `MobileGL/MG_Impl/Pipe/CsoCache.{h,cpp}` | D7 — 64-entry LRU, hash → probe → `memcmp` → handle, `DeleteRenderState` on evict, the `kMGPipeBehaviourNoCsoContentAddressing` arm |
| CREATE | `MobileGL/MG_Impl/Pipe/SetHashSuppressor.h` | D11 |
| MODIFY | `MobileGL/MG_Impl/Pipe/PipeFill.{h,cpp}` | D1 — `MGPipeFillForVerb` → `MGPipeValidateForVerb`, the five-step body, the residual fill restricted by `kMGPipeFieldEmittedBy[]`, the `MGP_FILL` expansion |
| MODIFY | `MobileGL/MG_Pipe/PipeMutation.h` | D4 — `MGP_NOTE_AGGREGATE(Aggregate)`, `((void)0)` in the pull build |
| MODIFY | `MobileGL/MG_State/GLState/{VertexArrayState/VertexArrayState.*, FramebufferState/FramebufferState.*, TextureState/TextureState.*, BufferState/BufferState.*}` | D4 — the five aggregate generations, all `#if MOBILEGL_PIPE_PUSH` |
| MODIFY | `MobileGL/MG_State/GLState/{VertexArrayState/VertexArrayObject.cpp, FramebufferState/FramebufferObject.cpp, TextureState/TextureObject.cpp, TextureState/TextureObject2DCube.cpp, SamplerState/SamplerObject.cpp, BufferState/BufferObject.cpp}` | the ~20 `MGP_NOTE_AGGREGATE` statements at the bump points D4 lists |
| CREATE | `MobileGL/MG_Pipe/DirtySurface.def` | D16 — 73 rows |
| MODIFY | `scripts/gen_pipe_dirty_surface.py` | D16 — `--check`, `--self-test`, mapped output |
| MODIFY | `MobileGL/MG_Pipe/FillPoints.def` | tighten the fill table: **re-check the 8 statically over-approximated rows first** (`MEASUREMENTS.md:114-115` names the three groups: `kReadback` + `IsTransformFeedbackActive`/`IsTransformFeedbackPaused`; `kTextureOp` and `kDispatch` + `IsCapabilityEnabled`; `kBlitOrCopy`/`kTextureOp` + the shader blit's viewport and vertex/buffer bindings), and record the verdict per row in the def's comment |
| MODIFY | `MobileGL/MG_Test/Pipe/{TrackerTest, CsoCacheTest}.cpp` | contents (the stubs are A's) |

**Ordered steps**

1. `b1`: the five aggregate generations + `MGP_NOTE_AGGREGATE`, with a unit test that each bump point moves its aggregate and no other. Build pull; `symbol_report --threshold 0` unchanged. Message: `[Feat] (State): give the frontend five aggregate generations so a tracker can answer "did any bound texture, buffer, attachment or attribute move" with one Uint64 compare`.
2. `b2`: `Tracker.{h,cpp}` + the `MGPipeValidateForVerb` rewrite, emitting **nothing** yet — dirty computed and counted, then the full P1 residual fill runs unchanged. This step is the safety net: it proves the walk is semantically free before anything is removed. Message: `[Feat] (Pipe): compute the per-verb dirty mask at the validate point and count how often each bit fires`.
3. `b3`: `CsoCache` + `create/bind_render_state` + `set_dynamic_state` emission; the residual fill drops the 29 derived fields. **This is where the render-state family stops being pulled.** Message: `[Feat] (Pipe): mint render-state CSOs on the pipeline subset and send only the dynamic chunks that moved - the steady-state cost of the whole render-state family is now two Uint16 compares`.
4. `b4`: `set_pixel_pack_state`, `set_patch_state`, `set_vertex_attrib_defaults` + `SetHashSuppressor` wired to the last of the three. Message: `[Feat] (Pipe): push pixel-pack, patch and vertex-attribute-default state as their own calls, the last one behind the set-hash suppressor`.
5. `b5`: `set_residual_value_state` + the tripwire (D9). Message: `[Feat] (Pipe): carry what has no call of its own in the residual value block and abort when it disagrees with the assembled state`.
6. `b6`: `DirtySurface.def` + the scanner's `--check` / `--self-test`; the `FillPoints.def` tightening and the 8-row re-check. Message: `[Feat] (Pipe): map every frontend mutator onto the aggregate generation that publishes it, and make the dirty-surface scanner a gate`.
7. `b7`: `TrackerTest.cpp` and `CsoCacheTest.cpp`.

**Tests to add**

- `TrackerTest.SteadyStateEmitsNothing` — two identical draws in a row move no `PipeStats` call counter.
- `TrackerTest.BlendToggleReusesTwoCsos` — `enable / draw / disable / draw` × N mints exactly **2** CSOs and issues 2N binds. This is the Blaze3D shape `ARCHITECTURE.md:151` names as the reason push happens at validate and not in the setter.
- `TrackerTest.ViewportDoesNotMintACso` — `glViewport` in a loop mints 0 CSOs and emits N `set_dynamic_state`s carrying only chunk D0. This is the regression `RenderState.h:165-170` records.
- `TrackerTest.WrapAroundRePushesButNeverMisses` — drive `m_version` past 65535 and assert no missed emission.
- `TrackerTest.AggregateGenerationCatchesABoundTextureMoving` — bind a texture, draw, `glTexSubImage` it, draw again; `NEW_SAMPLER_VIEWS` fires on the second draw. **This is the one direction the P1 verify comparator cannot see**, because it compares O-class fields by identity only (`PipeInputs.h:248-249`) and `ARCHITECTURE.md:502` names under-firing as the dangerous direction.
- `CsoCacheTest.LruEvictsTheOldestAndEmitsDelete`; `CsoCacheTest.HashCollisionDoesNotAliasTwoStates` (force a collision through a test seam and assert the `memcmp` rejects it); `CsoCacheTest.ContentAddressingOffMintsEveryTime`.

**Constraints**: B touches **no** file under `MG_Backend/`. B does not edit `MG_Pipe/{MGPipeValueTypes.h, MGPipeTypes.h, MGPipeRenderStateSpans.*, PipeApply.*, Coverage.def, PipeFields.def}` or `scripts/gen_pipe.py` (A owns them), and does not edit `.github/workflows/test.yml` (E owns it) — so B's scanner change must keep `--summary` working until E lands.

**Verification (`~/w7/p2-tracker`)**

```sh
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
python3 scripts/gen_pipe.py --check
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0
cmake --build build-push  --parallel 28 && ctest --test-dir build-push  -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4
MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4   # the all-pull arm, same names, same result
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4 --output-on-failure
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/p2-tracker -j 4
grep -l 'Fatal{' ~/w7/retrace-out/p2-tracker/*/mobilegl.log       # empty
```

### C.2 Package C — `p2/espryt` (tree `~/w7/p2-espryt`, from `p2/contract`)

**Files**: `MobileGL/MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.h, Managers.cpp}`, new `MobileGL/MG_Backend/DirectGLES/SlotTables.h`, and `MobileGL/MG_Test/SanityTest.cpp` (the `ScopedDirectGLESTextureBindings` fixture, `:145-197`).

**Ordered steps**

1. `e1`: `SlotTables.h` — per-kind dense arrays with a live bitset, whole-table save / reset / restore, and the `EnsureProcessTeardownSentinel()` arming moved onto first insertion (`Managers.h:295`, with `:52-57` explaining why a destructor hook is wrong). Both arms compile; nothing uses the new one yet.
2. `e2`: the explicit-destroy hook — `delete_*` emitted when a frontend object's last `SharedPtr` drops, following `BufferBackendOps`' shape (`BufferObject.h:76-103`, fired at `BufferObject.cpp:39-41`). **This must land before the tables switch over**, because a slot table has no garbage collector; today the only death signal is the registry's `weak_ptr` expiring (`Managers.h:306-314` quantifies the dead gigabytes that costs).
3. `e3`: switch the six kinds over behind `kMGPipeSubsystemEsprytSlots`, deleting `OwnerEquals` (`DirectGLES.cpp:63-66`), the three `TwinLookupMemo`s (`:83-132`, use sites `:1206, 1214, 2769, 2777, 2874, 2879`), `UnitSamplerLookupMemo`'s `WeakPtr` test (`:3157-3177`), the seven `CollectGarbageIfNeeded` calls (`:1223, 1533, 1898, 1899, 1900, 2728, 2729`), `SyncTextureObjectToBackend`'s by-value copy and second `Find` (`:1310-1345`), and the four `pDefaultFramebufferInfo->defaultFBO` compares (`:1939, 2874, 2898, 6420`). Keep every one of them compiled under `MOBILEGL_PIPE_LEGACY_MEMOS`.
4. `e4`: `g_fbSlotCache` / `GetFramebufferBindingSlotFast` (`:134-157`) → a plain `MGB_CTX->GetFramebufferBindingSlot(target)` read at all five call sites (`:1613, 1907, 2745, 2860, 2935`), closing the P1 poison bypass.
5. `e5`: the `SanityTest.cpp` fixture.

Messages: `[Refactor] (Espryt): key every backend twin on {slot, gen} instead of the frontend object's heap address - six registries become slot arrays and eleven identity memos go away`; `[Fix] (Espryt): tell the backend when a frontend object dies instead of discovering it in a garbage sweep`; `[Refactor] (Espryt): read the framebuffer binding slot through MGB_CTX at every call site instead of caching a raw pointer past the poison`.

**Must not break** — the D13 list. Each item has a named existing test: `SanityTest.cpp:2575-2596` (scratch-FBO scrub), `:2650-2665`, `:2757-2790` (context-generation guards on texture/framebuffer/renderbuffer twins), `:3020-3062` (sampled-set staleness), and `Scenarios/{SampledSetStalenessScenario, CrossFrameBufferScenario, VertexArrayEnableDisableScenario, VertexAttribBindingScenario, MultiDrawScenario, ResidentIndexScenario}.cpp`.

**Verification (`~/w7/p2-espryt`)**

```sh
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'      # empty (G13)
grep -n 'OwnerEquals\|TwinLookupMemo\|g_fbSlotCache\|CollectGarbageIfNeeded' MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
#   every remaining hit must be inside a MOBILEGL_PIPE_LEGACY_MEMOS arm
cmake --build build-push --parallel 28
ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 -R DirectGLES
MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 -R DirectGLES   # legacy arm
python3 ~/w7/retrace_gate.py --tree ~/w7/p2-espryt --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p2-espryt -j 4 --backend DirectGLES --ssim 0.99
```

### C.3 Package D — `p2/magma` (tree `~/w7/p2-magma`, from `p2/contract`)

**Files**: `MobileGL/MG_Backend/DirectVulkan/Renderer/{VulkanRenderer.h, VulkanRenderer.cpp, VertexInputStateFactory.h, VertexInputStateFactory.cpp}` and `MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h` (deleting the three backend-memo triples — a different file from the four state containers B edits).

**Ordered steps**

1. `m1`: re-key `GetOrCreatePipeline`'s memo on the bound CSO handle (`VulkanRenderer.cpp:4997-5013`) and delete the cached-hash fields (`VulkanRenderer.h:855, 856, 860, 861, 862`; `InvalidatePipelineMemo()` at `:881-885` loses them), behind `kMGPipeSubsystemRenderState`. `renderPassHash` stays in the key — it is what separates draws that differ only in `colorAttachmentCount` / `sampleCount`. Message: `[Refactor] (Magma): key the pipeline memo on the render-state CSO handle and stop recomputing a hash the client already computed`.
2. `m2`: `ApplyDynamicDrawStateTail` (`:5902-6004`) re-sourced from the pushed dynamic version, plus the `DynamicTailKey`-coverage unit test (D12.3). Message: `[Refactor] (Magma): drive the dynamic tail from the pushed dynamic version so a pipeline-only change stops invalidating it`.
3. `m3`: subsystem 4 — `VertexInputStateFactory::ComputeHash`'s `bufferKey` (`VertexInputStateFactory.cpp:48`) and a slot-indexed `VaoDrawMemo` (`VulkanRenderer.h:1244-1272`, `VulkanRenderer.cpp:3540-3579`), behind `kMGPipeSubsystemMagmaVertexInput`. Message: `[Refactor] (Magma): key the vertex-input cache and the VAO draw memo on {slot, gen} instead of a lifetime id and a heap address`.
4. `m4`: delete `SetBackendStateMemo` / `SetBackendAuxMemo` / `SetBackendHashMemo` and their storage from the frontend VAO (`VertexArrayObject.h:106-150`, `:190-197`) and the eviction-epoch dance with them (`VertexInputStateFactory.cpp:78`, `:318`, `VertexInputStateFactory.h:141`). Message: `[Refactor] (State, Magma): take the backend's raw pointers out of the frontend VAO - the state memo is deleted, the hash and aux memos become server-side per-slot fields`.

**Out of scope, do not touch**: `ProgramObject::{Get,Set}BackendHashMemo` (`ProgramObject.h:798-826`, sole user `ProgramFactory.cpp:3446-3448`) and the wider `SetupDrawSnapshot` re-key (`VulkanRenderer.h:958-979`) beyond collapsing its `vao` / `vaoLifetimeId` pair into the handle.

**Verification (`~/w7/p2-magma`)**

```sh
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0
grep -rn 'BackendStateMemo\|BackendAuxMemo\|m_backendHashMemo' MobileGL/MG_State MobileGL/MG_Backend   # only ProgramObject's, which is out of scope
cmake --build build-push --parallel 28 && ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 -R DirectVulkan
MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 -R DirectVulkan
python3 ~/w7/retrace_gate.py --tree ~/w7/p2-magma --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p2-magma -j 4 --backend DirectVulkan --ssim 0.99
```

### C.4 Package E — `p2/gates` (tree `~/w7/p2-gates`, from `p2/contract`)

Cross-cutting tooling. E branches from the contract and integrates **last**, but writes `HandleRecycleScenario` **first**, so its `AbaControl` arm is proven red before C and D exist — that is the "重键前红" evidence, recorded with the run log.

| action | file | what |
|---|---|---|
| CREATE | `MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` | D18 |
| CREATE | `MobileGL/MG_IntegrationTest/Scenarios/CsoContentAddressingScenario.cpp` | G12 — two entries, `kMGPipeBehaviourNoCsoContentAddressing` set vs clear, asserting `csom` / `csob` move in the expected direction and the pixels do not |
| MODIFY | `MobileGL/MG_IntegrationTest/CMakeLists.txt` | register both scenarios (source list from `:55`) and their environments, following the negative-control precedent at `:695-725` |
| MODIFY | `android-plugin/app/src/trace/cpp/trace_benchmark.{hpp,cpp}` | D.4.1 — a second per-frame series taken with `clock_gettime(CLOCK_THREAD_CPUTIME_ID, …)` beside the wall delta at `trace_benchmark.cpp:74-76`; `Report` gains `std::vector<double> frameCpuMs` next to `frameMs` (`trace_benchmark.hpp:32-39`) |
| MODIFY | `android-plugin/app/src/trace/cpp/trace_replay_core.{hpp,cpp}` | `SummarizeBenchmark` (`:832-865`) reused verbatim for the CPU series; `WriteBenchmarkJson` (`:867-899`) gains `meanFrameCpuMs`, `medianFrameCpuMs`, `p95FrameCpuMs` and `frameCpuTimesMs[]` beside `frameTimesMs[]` (`:889-896`) |
| MODIFY | `tools/trace_replay/run_android_retrace_local.py` | `format_benchmark` (`:228-237`) prints the CPU numbers |
| CREATE | `tools/device_bench/devices/{xiaomi-adreno830.env, oppo-mali.env}` | the two missing device profiles (only `odinlite.env` exists) |
| MODIFY | `MobileGL/MG_Benchmark/Driver/CMakeLists.txt` | a second ctest entry `DriverBenchStateToggle` running `mc_state_toggle` — `:14` currently runs only `draw_tiny`, and `mc_state_toggle` is already in `kBenchCases` (`DriverBenchCases.inc:558-570`, `:627`), so exposing it costs ~1.2 s in an existing 3 min job |
| CREATE | `scripts/g7_negative_control.sh` | D19 |
| MODIFY | `.github/workflows/test.yml` | `pipe-gates` (`:1477`): `gen_pipe_dirty_surface.py --check && --self-test` replacing the informational `--summary` (`:1524-1528`); a `ctest -L unit` step on the verify runtime inside `integration-verify` (`:473`) so G6 / G10's push-only unit tests actually run in CI; the two new scenario entries |

Messages: `[Test] (Pipe): reproduce the handle ABA through public GL and prove the pre-rekey guards are what stops it`; `[Feat] (Trace): record per-frame thread CPU time beside wall time so a paired A/B can be read as CPU cost`; `[CI] (Pipe): make the dirty-surface report a gate and run the push-only unit tests on the verify runtime`.

**Verification (`~/w7/p2-gates`)**

```sh
cmake --build build-verify --parallel 28
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure
#   on the contract tree, before C/D land: .Legacy and .AbaControl pass, .Handles SKIPs with
#   "subsystem not implemented on this tree" - a visible skip, never a vanishing test
cmake --build build-bench --parallel 28 && ctest --test-dir build-bench -L benchmark --no-tests=error
cmake --build build-retrace --parallel 28 && ./build-retrace/trace_replay_cli --benchmark --benchmark-no-finish ...
#   the desktop CLI shares the same core (tools/trace_replay/CMakeLists.txt:229,254-255), so the
#   CPU-series change is pre-flighted on WSL before any device time is spent
```

### C.5 File-ownership table

No two packages edit the same file. The contract is one commit that all four other branches descend from, so the integrator's rebase never sees an edit on both sides of a file.

| file / glob | A contract+spans | B tracker | C espryt | D magma | E gates |
|---|---|---|---|---|---|
| `MobileGL/MG_Pipe/{MGPipeValueTypes.h, MGPipeTypes.h, MGPipe.h, MGPipeRenderStateSpans.*, PipeApply.*, Coverage.def, PipeFields.def, generated/*.inc}` | **owner** | – | – | – | – |
| `MobileGL/MG_Pipe/{FillPoints.def, PipeMutation.h, DirtySurface.def}` | – | **owner** | – | – | – |
| `scripts/gen_pipe.py` | **owner** | – | – | – | – |
| `scripts/gen_pipe_dirty_surface.py` | – | **owner** | – | – | – |
| `scripts/g7_negative_control.sh` | – | – | – | – | **owner** |
| `MobileGL/MG_Impl/Pipe/SlotAllocator.*` | **owner** (contract) | – | – | – | – |
| `MobileGL/MG_Impl/Pipe/{Tracker.*, CsoCache.*, SetHashSuppressor.h, PipeFill.*}` | – | **owner** | – | – | – |
| `MobileGL/MG_State/GLState/RenderState/RenderState.{h,cpp}` | **owner** | – | – | – | – |
| `MobileGL/MG_State/GLState/{VertexArrayState/VertexArrayState.*, VertexArrayState/VertexArrayObject.cpp, FramebufferState/*, TextureState/*, BufferState/*, SamplerState/*}` | – | **owner** | – | – | – |
| `MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h` | – | – | – | **owner** | – |
| `MobileGL/MG_Backend/DirectGLES/**` | – | – | **owner** | – | – |
| `MobileGL/MG_Backend/DirectVulkan/**` | – | – | – | **owner** | – |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}`, `MobileGL/MG_Test/Util/PipeStatsTest.cpp` | **owner** (contract) | – | – | – | – |
| root `CMakeLists.txt`, `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | **owner** (contract) | – | – | – | – |
| `MobileGL/MG_Test/Pipe/CMakeLists.txt` and the four stubs' creation | **owner** (contract) | – | – | – | – |
| `MobileGL/MG_Test/Pipe/{RenderStateSpansTest, SlotAllocatorTest}.cpp` contents | **owner** | – | – | – | – |
| `MobileGL/MG_Test/Pipe/{TrackerTest, CsoCacheTest}.cpp` contents | – | **owner** | – | – | – |
| `MobileGL/MG_Test/SanityTest.cpp` | – | – | **owner** | – | – |
| `MobileGL/MG_IntegrationTest/**`, `.github/workflows/test.yml`, `tools/device_bench/**`, `tools/trace_replay/run_android_retrace_local.py`, `android-plugin/app/src/trace/cpp/**`, `MobileGL/MG_Benchmark/**` | – | – | – | – | **owner** |
| `docs/Disaggregated/*.md` | – | – | – | – | – (integrator only) |
| everything else | nobody | nobody | nobody | nobody | nobody |

The one file two packages both *touch conceptually* is `VertexArrayObject`: B edits `VertexArrayState.h` / `VertexArrayObject.cpp` (the aggregate generation), D edits `VertexArrayObject.h` (deleting the memo triples). They are different files, so the rebase is clean; the integrator still lands B before D so a compile error, if the split is wrong, surfaces at D's rebase and not at C's.

---

## D. Integration order, and what the integrator runs

### D.0 Preconditions

- The baseline captures of §A (`~/w7/p2-before-libMobileGL.so`, `~/w7/p2-before-ctest-names.txt`, `~/w7/p2-before-syncrenderstate.sha`, `~/w7/p2-before-dirty-surface.txt`), taken in `~/w7/pipe` at `48268068` **before any package lands**.
- `~/w7/pipe` clean at `48268068`; fixture binaries hidden the way `wsl_p2_tree.sh` does it (`git update-index --assume-unchanged` on `tools/trace_replay/fixtures`).
- `~/w7/retrace_gate.py` copied from `scratchpad/wf2/retrace_gate.py`.
- Both devices reachable and **not** locked by the GL4.6 Wave-7 campaign (`ls .../8ab6b3cb-.../scratchpad/w7/locks/`).

### D.1 Order: **contract → tracker → magma → espryt → gates**

Reason for that order, and it is not P1's by default:

- `contract` first because everything else compiles against it.
- `tracker` second because it is the only package that changes semantics; after it, both backends are still reading the same bytes they read before, through unmodified code. If `tracker` is wrong, the verify lane says so before any backend has been touched.
- `magma` third rather than last, because D's `m1`/`m2` (the render-state re-key) is the deliverable the day-43 number depends on and it is the one most likely to need iteration; `m3`/`m4` (subsystem 4) then land against a Magma that is already CSO-keyed.
- `espryt` fourth: Espryt needs **zero** render-state change by construction (G5), so C is purely Track H and is independent of everything except the contract.
- `gates` last, so its lanes assert against the finished tree — except `HandleRecycleScenario`, whose `AbaControl` red is recorded on the contract tree at D.2.

Copy `scratchpad/wf2/wsl_integrate_p05.sh` → `wsl_integrate_p2.sh` (`p05`→`p2`). Per slug: rebase `p2/<slug>` onto `feat/disaggregated` in its own worktree, `git merge --ff-only` in `~/w7/pipe`, then:

```sh
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/gen_pipe_dirty_surface.py --check          # from `tracker` onward
python3 scripts/check_include_closure.py
python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --markdown ~/w7/p2-symbol-<slug>.md
#   0 added / 0 removed / 0 renamed after every merge; resized ⊆ the three RenderState symbols of D15
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'                                  # empty
awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp | sha256sum
#   equal to ~/w7/p2-before-syncrenderstate.sha after EVERY merge (G5)
ctest --test-dir build-linux -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort | comm -23 ~/w7/p2-before-ctest-names.txt -   # empty
```

### D.2 On the contract tree, before `tracker`: record the "red before" evidence

```sh
cd ~/w7/p2-gates && cmake --build build-verify --parallel 28
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure | tee ~/w7/p2-handlerecycle-before.log
#   .AbaControl PASSES (it asserts the corrupted pixels), .Legacy PASSES, .Handles SKIPs
```
That log is the artefact `ROADMAP.md:18`'s "重键前红" asks for. Keep it; it goes into `MEASUREMENTS.md`.

### D.3 After every package has landed: the five-part gate on `~/w7/pipe`

**Part 1 — interface purity.**
```sh
python3 scripts/check_include_closure.py                                   # gate A
nm --undefined-only build-linux/libMobileGL.so | grep -E 'MG_State::GLState::|glslang' ; echo rc=$?   # gate B (n/a in monolith; recorded)
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'                  # gate C: empty
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure   # the {slot,gen} memo-key assertion (G8)
```

**Part 2 — the semantic shadow comparison (the decisive one).**
```sh
cmake --build build-verify --parallel 28
ctest --test-dir build-verify -L unit --no-tests=error --output-on-failure
ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4 --output-on-failure
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-verify/libMobileGL.so \
   --out ~/w7/retrace-out/p2-verify -j 4
grep -L 'MGPipe verify:' ~/w7/retrace-out/p2-verify/*/mobilegl.log        # empty: every case armed
grep -l 'Fatal{'         ~/w7/retrace-out/p2-verify/*/mobilegl.log        # empty
```
A `Fatal{UnmigratedPipeInput, "F@V"}` is a missing `FillPoints.def` row — add the row (never a sticky flag, `FillPoints.def:24-27`), regenerate, rerun. A `Fatal{PipeVerifyDiffer, …, where=read}` is a real push/pull divergence — most likely a D5 derivation transcribed wrongly; it names the field, and it is fixed, never silenced. A `Fatal{PipeResidualDiverged}` means a capability lost its carrier (D9).

**Part 3 — behavioural A/B.**
```sh
# {pull, push} name-for-name on both backends (G2)
for d in build-linux build-push; do ctest --test-dir $d -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort > ~/w7/p2-names-$d.txt; done
diff ~/w7/p2-names-build-linux.txt ~/w7/p2-names-build-push.txt            # empty
ctest --test-dir build-linux -L integration-gpu --no-tests=error -j 4
ctest --test-dir build-push   -L integration-gpu --no-tests=error -j 4
MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4
# 40 traces (79 desktop cases) under push, both backends, SSIM >= 0.99 (G3)
python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-push/libMobileGL.so --out ~/w7/retrace-out/p2-push -j 4 --ssim 0.99
# the render-state-sensitive integration scenarios, called out by name because they are the ones a
# chunk-table mistake shows up in first:
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 \
  -R 'ClipDistance|SampleMaskScope|SampleVariables|DualSourceBlend|ViewportArray|PrimitiveRestart'
# the negative controls, always-on entries (G7, G8, G12)
bash scripts/g7_negative_control.sh build-push
ctest --test-dir build-push   -R CsoContentAddressing --no-tests=error --output-on-failure
ctest --test-dir build-verify -R 'PoisonOmitted\.|VerifyCorrupted\.|HandleRecycle' --no-tests=error
```
**CTS**: P2 is **not** one of the five architecture boundaries that need the full `gl44to46` caselist (`ROADMAP.md:34`) and no named block is assigned to it. The full 56,271-case run happens once, on both devices, when P2 merges to `dev` — scheduled off the critical path.

**Part 4 — performance non-regression.** D.4 below.

**Part 5 — coverage, poison, handle discipline.**
```sh
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test          # G6 regenerates, 0 UNMAPPED
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test   # G9
ctest --test-dir build-push -R 'RenderStateSpans\.|Residual' --no-tests=error --output-on-failure            # G6, G10
```

**CI.** `gh workflow run test.yml --ref feat/disaggregated`, then once green `gh workflow run test.yml --ref feat/disaggregated -f baseline_sha=48268068` for `monolith-symbol-report`. Record both run URLs. Expected green: `pipe-gates`, `include-graph-check`, `test`, `integration`, `benchmark`, `build-linux-verify`, `integration-verify`, `retrace`, `retrace-verify`, `monolith-symbol-report`. **Note for the schedule**: the three verify lanes have never yet produced a green CI run — they were added at `0fe7bf82` / `97b997d5` and the only run whose head contained them was still in progress when the scouts were written, so their CI wall time is unknown. The only cost datum is local (`MEASUREMENTS.md:142`): 818 `integration-verify` entries at `-j 4` are about the same order as `integration-gpu`, and 79 retrace cases at `-j 4` take ~20 minutes on a 28-core WSL box. Budget accordingly and do not put the device runs behind them.

**A P2 decision the CI file forces**: `test.yml:520-526` records that `integration`'s second, `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` filtered pass (186 entries) is not run under the comparator and says "P2 can take it once the comparator's cost is known". **Decision: P2 does not take it.** The comparator's cost is now known to be 5–10× and the 186 entries are a buffer/readback filter that P2 changes nothing in; revisit at P3b, and the integrator records that decision in `MEASUREMENTS.md` rather than leaving the note dangling.

### D.4 The measurement runs P2 owes the day-43 GO/NO-GO

Four numbers, in the order they must be produced. Items 2–7 of `ROADMAP.md:46-52` are all here; item 1 is P1's and is already recorded at `MEASUREMENTS.md:126-142`.

#### D.4.1 Prerequisite: a per-thread CPU series in the retrace harness (package E)

**There is no per-thread CPU-time instrumentation anywhere in the tree** — greps for `CLOCK_THREAD_CPUTIME_ID`, `CLOCK_PROCESS_CPUTIME_ID`, `getrusage`, `RUSAGE_THREAD` and `/proc/self/task` across `MobileGL/`, `tools/`, `android-plugin/` and `scripts/` return nothing. The metric the whole GO/NO-GO hangs on (`ROADMAP.md:56`) therefore has no first-party collector today, and the cheapest way to get one is inside the retrace benchmark itself: retrace runs `--singlethread` (`trace_replay_core.cpp:459`) and `trace_benchmark.hpp:12-13` states that `Begin` / `OnFrameBoundary` / `End` are only ever reached from that one thread, so **that thread's CPU time is the client-side CPU cost** — no sampling, no root, no debuggable build, no profiler.

E's four edits (D.4's cost is one afternoon, and it is pre-flighted on the desktop CLI, which shares the same core):
`trace_benchmark.cpp:74-76` takes `clock_gettime(CLOCK_THREAD_CPUTIME_ID, …)` beside `Clock::now()`; `trace_benchmark.hpp:32-39`'s `Report` gains `frameCpuMs`; `SummarizeBenchmark` (`trace_replay_core.cpp:832-865`) is reused verbatim for the CPU series; `WriteBenchmarkJson` (`:867-899`) emits `meanFrameCpuMs` / `medianFrameCpuMs` / `p95FrameCpuMs` and the full `frameCpuTimesMs[]` array beside `frameTimesMs[]`. **p99 needs no device change even today** — the full per-frame array is dumped, so any percentile is computed host-side from `benchmark.json`; only the headline fields stop at p95.

**Do not** put a timer inside `MGPipeValidateForVerb`. `ROADMAP.md:7` forbids committing hot-path instrumentation, and D17 explains where the ns number comes from instead.

#### D.4.2 The paired two-device A/B (GO/NO-GO items 4 and 3)

Protocol, per device, one lock hold per arm, `scratchpad/wf2/stats_baseline.sh:16-29` as the template:

- **reboot-clean**: `adb -s $SER reboot`, wait for boot completion, wake and unlock (`input keyevent KEYCODE_WAKEUP; input keyevent 82`), let the device settle to the thermal floor before the first arm, and run the two arms **back to back in the same session** so they share a thermal window (`tools/device_bench/README.md:45-58`).
- **CPU/GPU pinned** per the project protocol (big 1.96 GHz / little 1.55 GHz, GPU maxed, 40 °C gate), the pin re-checked at the end of each window and the window discarded if it drifted off-pin.
- **serially from one tree**: `run_android_retrace_local.py` shares one `.trace-work/android-retrace-result` root per tree and `rmtree`s it per invocation (`:14`, `:354-356`), so the two devices **must not** be driven concurrently from one worktree (`MEASUREMENTS.md:93`).
- **device lock** held ≤ 25 min per acquisition, `owner` file written, `rm -rf` on the way out; a lock held > 60 min is not to be broken — report "blocked by device lock".
- **ColorOS traps** on `3B159D009VZ00000`: the first install of a not-yet-installed package blocks on `com.oplus.appdetail InstallGuideActivity` until "继续安装" is tapped (`input tap 353 2349` on the 1272×2772 panel); a foreign-signed APK must be uninstalled first.
- `MSYS_NO_PATHCONV=1` on every invocation, because a `/data/...` inside an `--env` value is path-converted otherwise.

Arms: **{pull APK, push APK} × {`--benchmark-no-finish`, finish} × 4 cases × 2 backends**, per device.

```sh
LOCKS=.../8ab6b3cb-.../scratchpad/w7/locks
for SER in 35d0befa 3B159D009VZ00000; do
 for c in minecraft-1.21.4-in-world minecraft-1.21.4-fabric-iris-bsl-in-world improved-transparency-minecraft-26.3 minecraft-1.21.4-startup; do
  for b in DirectGLES DirectVulkan; do
   for arm in pull push; do
    until mkdir "$LOCKS/$SER" 2>/dev/null; do sleep 20; done; echo "p2-ab $(date)" > "$LOCKS/$SER/owner"
    ANDROID_SERIAL=$SER MSYS_NO_PATHCONV=1 python3 tools/trace_replay/run_android_retrace_local.py \
      --case "$c" --backend "$b" --benchmark --benchmark-no-finish --benchmark-repeats 3 --benchmark-tail-frames 200 \
      --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
    #   the same invocation yields BOTH artefacts: benchmark.json (frameCpuTimesMs[]) and
    #   mobilegl.log (the 'MGPipe stats:' windows) - trace-replay-ci.sh:489,491 copies both
    rm -rf "$LOCKS/$SER"
   done; done; done; done
```

Then, host-side, reduce `frameCpuTimesMs[]` over the trailing 200 frames to **p50 and p99** per arm and publish the delta `push − pull` per (device, case, backend). `--benchmark-no-finish` is the **primary** arm because P2's question is CPU cost (`trace_benchmark.hpp:16-24`); the finish-ON arm is the sanity check that GPU time did not move.

Also read out of the same runs, because they survive the accessor-tally relocation problem (D17): the six memo gate hit/miss pairs (`ers etl eub mfp mpm mdt`) from the **last complete window** of `grep 'MGPipe stats:' mobilegl.log`, the `resid=` byte class (G10), and the two new `csom` / `csob` call classes.

**Excluded**: `minecraft-1.21.1-neoforge-create-indirect-in-world` — it fails on both devices on the `dev@81b17c0b` baseline APK itself (Espryt black frame after ~4.5 min on Adreno 830, Magma `VK_ERROR_DEVICE_LOST` during a texture-upload submit; Mali SSIM 0.85 / 0.45), so it is not this branch's regression (`MEASUREMENTS.md:96`, open question 17). It stays in the 40-trace desktop SSIM corpus and out of the device A/B.

**Trap**: `MOBILEGL_PIPE_STATS_FILE` never produces a file on device — the JSON is written from `PipeStats::Shutdown()` (`PipeStats.cpp:271-278`) which runs from `MobileGL::Destroy` (`Init.cpp:50`), and the trace app never reaches that teardown. Only the periodic `mobilegl.log` lines exist, and a run shorter than one period yields nothing at all; hence `MOBILEGL_PIPE_STATS_PERIOD=120` (or lower for a short fixture).

#### D.4.3 The tracker's absolute ns per draw (GO/NO-GO item 5)

From `DriverBench`'s `ns_per_op` (`MobileGL/MG_Benchmark/Driver/DriverBench.c:31-32`, printed at `:288-289`), on the desktop, against the same library built three ways. `DriverBench` is a headless EGL client that is **not** linked against MobileGL: it `dlopen`s exactly one provider from `$DRIVERBENCH_EGL_LIB` (`DriverBench.c:9-18`), so the same binary measures the native driver and both MobileGL backends. It loops 30 warmup + 120 timed frames, each closed with a real fence wait rather than `glFinish` (MobileGL implements `glFinish` as a no-op, `DriverBench.c:245-266`).

```sh
cd ~/w7/pipe && cmake --build build-bench --parallel 28
for lib in build-linux build-push; do            # pull vs push, same tree, same flags
  for be in espryt magma; do
    DRIVERBENCH_EGL_LIB=$PWD/$lib/libMobileGL.so \
    bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh $be $PWD/$lib/libMobileGL.so
  done
done
bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh native      # the control
```

Two deltas, both published, on `mc_vanilla_draw` (5495 draws/frame — the per-draw denominator):

- **T1, the gate number** = `ns_per_op(push, default bitmask) − ns_per_op(pull)`. This is the whole boundary cost per draw.
- **T2** = `ns_per_op(push, MOBILEGL_PIPE_PUSH=0) − ns_per_op(pull)`. This is P1's residual fill alone, so **T1 − T2** isolates exactly what P2 added and what it removed.

**The ceiling, and it is pinned in `MEASUREMENTS.md` before the run** (`ARCHITECTURE.md:504` demands an absolute threshold precisely because a relative one would pass trivially). Derivation: the pull baseline is **6.5–9.3 accessor calls plus memo probes per draw** (`MEASUREMENTS.md:64`, table `:50-58`: MC 1.21.4 in-world Espryt 9.28 / Magma 8.56 at 91.6 draws/frame; improved-transparency 26.3 Espryt 8.44 / Magma 6.53 at 1320 draws/frame). Priced at a non-inlined accessor call plus a load — 6–10 cycles, ~4 ns at 1.96 GHz — plus six memo probes at ~2 ns, that is **~44 ns/draw of work push has to be no worse than**. So:

> **T1 ≤ 45 ns/draw on Adreno 830 (`35d0befa`) and ≤ 60 ns/draw on the Mali device (`3B159D009VZ00000`)**, pinned before the A/B is run. If the pre-run calibration (one DriverBench run of the pull library, reporting `ns_per_op` for `mc_vanilla_draw`, minus the `native` control) says the per-accessor price on these devices is different, the *measured* price replaces the estimate and the ceiling is re-derived by the same arithmetic — but it is pinned before the push arm is measured, not after.

`DriverBench` is Linux-desktop only (`Driver/CMakeLists.txt:7-9`) and the whole benchmark tree is force-disabled on Android (`CMakeLists.txt:33-35`). That is acceptable: P2's ns question is a client-side CPU question and the desktop run answers it, while the two-device requirement (`ROADMAP.md:49`) applies to the per-thread CPU deltas, which come from D.4.2. **Do not** enable the google-benchmark tree on Android to change this; if an on-device blend-toggle is ever wanted the cheap route is a new Android-only target modelled on `DriverBench.c`, which already `dlopen`s EGL and falls back to surfaceless (`DriverBench.c:294-300, 417`).

#### D.4.4 The Blaze3D blend-toggle microbenchmark (GO/NO-GO item 6)

**The case already exists**: `case_mc_state_toggle` (`DriverBenchCases.inc:558-570`) is `glEnable(GL_BLEND); glBlendFuncSeparate(...); glDrawElements(...); glDisable(GL_BLEND); glDrawElements(...)` × 46, registered as `{"mc_state_toggle", case_mc_state_toggle, 46, 0, 46}` (`:627`) at exactly the vanilla-frame rate (46 toggle pairs and 28 `glBlendFuncSeparate` per frame). Note the case is `enable/draw/disable/draw`, which is what `ROADMAP.md:51` writes verbatim.

```sh
for be in native espryt magma; do
  bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh $be ... | grep '^mc_state_toggle'
done
```
Published as a table of `ns_per_op` for `{native, espryt-pull, espryt-push, magma-pull, magma-push}`, alongside `mc_vanilla_draw` and `mc_pass_switch` so the toggle number is read against a non-toggling baseline. This is the workload the whole "push at validate, not in the setter" decision (`ARCHITECTURE.md:151`) was made for, and it is where a per-setter design would have shown up as a loss.

#### D.4.5 The CSO negative control (GO/NO-GO item 7)

The knob is `kMGPipeBehaviourNoCsoContentAddressing` (bit 63 of the runtime `MOBILEGL_PIPE_PUSH` bitmask, D14). **It disables the map probe and the handle reuse, not the CSO records** — otherwise it measures a different thing: with it set, every pipeline-version change mints a fresh CSO, binds it and evicts, which is precisely "whole-block content addressing" and reproduces the regression `RenderState.h:165-170` records.

```sh
# desktop: the microbenchmark and the per-draw case, with and without
MOBILEGL_PIPE_PUSH=0x800000000000007f bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh espryt ...
MOBILEGL_PIPE_PUSH=0x7f               bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh espryt ...
# device: one extra arm of D.4.2 on minecraft-1.21.4-in-world only, both backends, one device
ANDROID_SERIAL=35d0befa MSYS_NO_PATHCONV=1 python3 tools/trace_replay/run_android_retrace_local.py \
  --case minecraft-1.21.4-in-world --backend DirectGLES --benchmark --benchmark-no-finish \
  --env 'MOBILEGL_PIPE_PUSH=0x800000000000007f' --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
```
The point of the control is to separate **"push is slower"** from **"the CSO design is slower"**: if T1 is over the ceiling *and* the control is not, the cost is in the tracker; if both are over, the cost is in the wire shape. And because a dead switch would pass silently, `CsoContentAddressingScenario` (E) is an always-on ctest pair asserting that `csom` / `csob` move in opposite directions with and without the bit while the pixels do not move at all.

#### D.4.6 The Track H unit costs (GO/NO-GO item 3)

`ROADMAP.md:56`'s criterion is "Track H 单位成本不超出估计的 50%", against the deleted `PLAN.md`'s estimates of **Espryt 0b = 5–7 days** and **Magma subsystem 4 = 2–3 days**. So the measurement is calendar, not nanoseconds: record the actual elapsed days for packages C and D, the diff sizes, and the count of memos actually retired against `ARCHITECTURE.md:363`'s census (11 direct deletions, 2 moved to client debounce, 7 re-keyed, 1 unchanged). P2 pays into that census: **11 of the 11 direct deletions and 2 of the 7 re-keys**. The remaining slices are P3a/P3b/P4a/P4b/P7 and their unit cost is extrapolated from these two.

### D.5 Docs, and the merge to `dev` (integrator, one commit)

`scripts/check_doc_citations.py docs/Disaggregated/*.md` must stay clean — every `file:line` a doc edit adds has to be true.

- `README.md:3` — status → P2 landed with the hash.
- `ROADMAP.md:18` — ✅, with the real numbers: 7 pipeline chunks / 396 B and 8 dynamic chunks / 772 B; CsoCache 64; the residual ratchet 1248 → 8; 11 memos deleted and 2 re-keyed.
- `ROADMAP.md:74` (open question 1) — answered with T1/T2 and the two-device p50/p99 table; `:77` (Q4) — answered with the chunk granularity and the note that 64 is retuned at P13 with the counters live; `:78` (Q5) — answered, and **corrected to three capabilities** (the preamble's correction 1).
- `ARCHITECTURE.md:153` — eight validate entries → nine, named (D1).
- `ARCHITECTURE.md:172` and `:568` — "P1 起成为门" → P2, matching what `test.yml` actually runs.
- `ARCHITECTURE.md:186` — the split's one place is now real; `:189` — the "~25-30 字" estimate → 396 B / 49.5 words; `:190` — the `FramebufferSrgb`/`DepthClamp` open question is closed, and `TextureCubeMapSeamless` is added.
- `ARCHITECTURE.md:504` — `tools/bench.sh` → `tools/device_bench/bench.sh`.
- `ARCHITECTURE.md:580`, `:588` — `MOBILEGL_PIPE_LEGACY_MEMOS` gains its CMake row; the `MOBILEGL_PIPE_PUSH` bitmask row gains the bit list and the new default.
- `MEASUREMENTS.md` — a new §8 with: the D.4.2 two-device p50/p99 table, T1/T2 and the pinned ceiling with its derivation, the blend-toggle table, the CSO negative control, the D.2 "red before" log, the six gate hit/miss pairs before and after, the `resid=` and `csom`/`csob` numbers, the per-dirty-bit fire rates, the payload histogram, the verdict on the 8 over-approximated fill rows, and the `integration-verify` filtered-pass decision of D.3.

Then `[Merge]` to `dev` per the milestone flow, and schedule the full `gl44to46` caselist on both devices off the critical path (`ROADMAP.md:34`).

---

## E. Risks and mitigations

| risk | mitigation |
|---|---|
| **The `[581,584)` padding hole is not where this brief computes it**, so the three new capability bools grow `RenderStateParameters` past 1168 — which breaks the `static_assert` at `MGPipeValueTypes.h:541`, `MGL_RESIDUAL_BLOCK_SIZE`, and every offset in D6's table. | It is a **compile error on the first build of `c0`**, not a silent drift, and it is the very first thing the contract package builds. If it fires, find the real hole with a temporary `static_assert(offsetof(...))` — do **not** grow the struct: the residual ratchet only goes down and a growth is a deliberate build break by design (`MGPipeTypes.h:527-535`). |
| **The hand-derived chunk offsets in D6 are wrong.** They were computed from declarations, not compiled. | `MGPipeRenderStateSpans.cpp` computes every boundary with `offsetof`/`sizeof` and `static_assert`s that the chunks are sorted, non-overlapping and cover exactly `[0, sizeof(RenderStateParameters))`. A mistake in this brief is a build break in that file, and the file is right. |
| **The pipeline subset is now a superset of what Magma hashes, so more state mints CSOs** — a scissor or sample-coverage toggle that used to cost nothing now costs a hash and a probe. | Hashing runs only when `m_pipelineStateVersion` moves, which is exactly when Magma recomputes `ComputePipelineStateHash` today, so the work is moved, not added. `TrackerTest.BlendToggleReusesTwoCsos` pins that a two-state ping-pong mints two CSOs and then reuses them forever, and D.4.4's blend-toggle table is the measurement. If the number disappoints, the escape hatch is demoting the offending setters to `++m_version` — but that is a **pull-build** semantic change and therefore a P3 decision with its own gate, not an in-flight P2 fix. |
| **A D5 derivation is transcribed wrongly**, so a backend reads a value that is subtly not what `GLContext` would have returned. `GetViewport`'s float→int rounding and the 35-way `IsCapabilityEnabled` switch are the two candidates. | This is exactly what the P1 verify comparator was built for and why the entry compare stops being tautological here: the compare-at-read re-reads every one of those fields from the live context at every backend read, field-wise, across 79 retraces and 818 integration-verify entries. On top of it, `RenderStateSpansTest.DerivationMatchesTheFrontendGetters` drives every setter and compares all 29. |
| **A dirty bit fires too rarely** — the dangerous direction (`ARCHITECTURE.md:502`), and one the current harness is blind to for object-class state, because the verify comparator compares O-class fields **by identity only** (`PipeInputs.h:248-249`). | P2 emits for value-class bits 0–4 only; bits 5–17 are computed and *counted* but nothing depends on them yet, so an under-firing object-class bit cannot render anything stale in P2. `TrackerTest.AggregateGenerationCatchesABoundTextureMoving` is the first real test of the direction, and the per-bit fire rates go into `MEASUREMENTS.md` so P3a starts from data. The verify "保留模式" extension for consume-and-clear groups is P3b work and is recorded as such. |
| **A slot table has no garbage collector, so an object nobody destroys leaks its twin forever** — worse than today's lazy `weak_ptr` sweep. | C's `e2` lands the explicit `delete_*` notification **before** `e3` switches the tables over, and `e3` is not merged without it. `SanityTest.cpp:2575-2596` (deleted-texture scrub) and the ~100 CTS cases `Managers.h:306-314` describes are the existing coverage; a leak check on the 79-case retrace (peak RSS, push vs pull) goes into `MEASUREMENTS.md`. |
| **`EnsureProcessTeardownSentinel()` stops being armed** because the arming site moved off `GetOrCreate`, and a twin then `glDelete*`s a recycled name during process teardown. | The arming moves onto the slot table's first insertion in the same commit, and `Managers.h:52-57` documents why a destructor hook is the wrong answer. `SanityTest.cpp:2650-2665, 2765-2790` pin the context-generation guards that back it up. |
| **The `g_fbSlotCache` rewrite reintroduces `pGLContext` in `MG_Backend/`** (it is the one place tempted to, since it caches a pointer into frontend storage). | Purity gate C (`grep -rc pGLContext MobileGL/MG_Backend`) runs after every merge, and the replacement is a plain `MGB_CTX->` read, which is also what closes the P1 poison bypass at those five sites. |
| **`HandleRecycleScenario` is green for the wrong reason** — the allocator never actually reproduces the address, so the scenario proves nothing. | The `AbaControl` arm exists precisely to prove the reproducer reproduces: it defeats the two guards and asserts the *wrong* pixels. If the ABA stops happening, that arm fails. `ObjectLifetimeIdTest.cpp:120-121,130` shows the precedent of skipping rather than passing when the allocator refuses to repeat — the scenario does the same, with a visible skip. |
| **P2 changes `glEnable(GL_FRAMEBUFFER_SRGB)` behaviour**: Espryt now issues a real `glEnable(GL_FRAMEBUFFER_SRGB)` where it previously always disabled. | `MEASUREMENTS.md:84` records that none of the 41 fixtures enables it, so G3's SSIM ≥ 0.99 over 40 traces on both backends is the gate, plus the full `gl44to46` caselist at the `dev` merge. The change is recorded in `MEASUREMENTS.md` as a deliberate conformance fix (it was a silently-swallowed `glEnable` and a lying `glIsEnabled`), not as a side effect. |
| **The pull build's `.text` moves more than the three named symbols.** | `symbol_report.py --threshold 0` after every merge (D.1). Anything outside the named set is a defect. If a compiler schedules the `{}` differently than expected, the delta is attributed line by line in the commit message; the gate admits an attributed delta, never an unexplained one. |
| **`acc/draw` improves for the wrong reason.** `CallClass::AccessorCalls` is a static tally at ~10 hot entry points (`PipeStats.cpp:76-88`), so a tracker that merely *relocates* reads out of `SyncRenderState` / `GetOrCreatePipeline` / `ApplyDynamicDrawStateTail` scores a lower number without removing work. | The report leads with the **gate hit/miss pairs** (which survive relocation, `PipeStats.cpp:93-99`) and the **CPU-time series**, and quotes `acc/draw` only alongside a re-audit of the tally constants at `DirectGLES.cpp:1421,1524,2043,2053,2977` and `VulkanRenderer.cpp:5009,5016,5274,5927,5934,6416,6449,6462`. This caveat is written into `MEASUREMENTS.md` above the numbers, not below them. |
| **Device time is the schedule's long pole**, and both devices are shared with the GL4.6 Wave-7 campaign under the mkdir lock. | The desktop numbers (T1/T2, blend toggle, CSO control, all of the correctness gates) come first and can NO-GO the phase without touching a device. The device A/B is 2 devices × 4 cases × 2 backends × 2 arms ≈ 32 lock holds of ~10 min; budget two days and hold ≤ 25 min per acquisition. A lock held > 60 min is not broken — report "blocked by device lock". |
| **The three verify CI lanes have never produced a green run**, so their wall time is unknown and could dominate. | The P2 exit does not depend on them: the local WSL verify run (D.3 part 2) is the evidence, exactly as it was for P1 (`MEASUREMENTS.md:129-142` is local `~/w7/pipe` evidence, not CI evidence). CI green is required before the `dev` merge, not before the GO/NO-GO. |
| **`MOBILEGL_PIPE_LEGACY_MEMOS` doubles the Espryt and Magma sync code and rots.** `ARCHITECTURE.md:367` budgets +1 day per phase for it. | Both arms sit behind the same `PipeInputs` interface, and every A/B command in §C runs the suite twice (`MOBILEGL_PIPE_PUSH=0` and default) so the legacy arm cannot silently stop compiling or stop passing. It retires with the pull path at P13. |
| **A collision in the 64-bit CSO key aliases two render states.** | The cache `memcmp`s the 396 pipeline bytes on a hash hit before reusing the handle (D7), and `CsoCacheTest.HashCollisionDoesNotAliasTwoStates` forces a collision through a test seam. The memcmp only runs when the pipeline version moved. |
| **The patch trio travels twice** (pipeline chunk P0 and `set_patch_state`), so the two carriers could disagree. | The applier asserts under verify that they agree, which turns the redundancy into a trip wire (D6). The bytes are 28 and the state changes about once per program, so the cost is nil. |
| **`gen_pipe_dirty_surface.py`'s three blind spots** (a mutator inside a lambda is attributed to the enclosing function; a mutation published through a helper reads as deferred; the scan root is `MG_Impl/GLImpl` only, so the four `MGP_NOTE_MUTATION` sites in `TextureState.h` are outside it) let an unmapped mutation slip past a green gate. | The gaps are written into `DirtySurface.def`'s header comment rather than left implicit, and the gate is a *completeness* gate over what the scanner does see — the semantic proof stays the verify lane, which is blind to none of them. Widening the scan root is recorded as a P3a item. |
| **A test that drives a backend helper directly aborts on the poison** because no verb filled the block — the P1 F3 class of failure. | `MG_Test/ScopedPipeVerb.h` already exists for exactly this and eleven tests already use it (`ef6227e1`). Any new test C or D writes that calls a backend helper directly declares its verb the same way; the poison is never weakened and no test name changes. |
| **A/B/C/D/E land against an outdated contract.** | The contract is one commit, tagged `p2/contract`. A must not change any signature in `MGPipeRenderStateSpans.h`, `PipeApply.h`, `SlotAllocator.h`, the subsystem bit constants or `RenderStateParameters` after the tag without telling the other four; a rebase-time compile error is the backstop. |
