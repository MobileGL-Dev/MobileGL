# Adversarial review — package `fix-core` (P1 lane findings F1 + F2)

Reviewer ran everything independently in WSL `~/w7/p1-fix-core` (branch `p1/fix-core`, HEAD
`9bd6d394`, three commits on top of `feat/disaggregated@97b997d5`). Tree left exactly as found
(`git status --porcelain` clean apart from the three untracked build directories, HEAD unchanged).

**Verdict: approved — 0 majors, 7 minors.** Every claim in `fix-core-v1.md` reproduced. The
central attack ("zero divergences was bought by disabling the read hook / by silencing the
comparator") is refuted by direct experiment below.

---

## 1. What was re-run, and what came back

Diff under review (`git diff 97b997d5..HEAD`, 371 lines, 6 files, nothing under `MG_Backend/`,
nothing under `tools/trace_replay/fixtures/`):

```
MobileGL/MG_Impl/Pipe/PipeFill.cpp                       |  28 ++
MobileGL/MG_Pipe/FillPoints.def                          |  31 +-
MobileGL/MG_Pipe/PipeMutation.h                          |  42 ++ (new)
MobileGL/MG_Pipe/generated/PipeFillPoints.inc            |  12 +-
MobileGL/MG_State/GLState/TextureState/TextureState.h    |  31 +-
MobileGL/MG_Test/Pipe/PipeInputsTest.cpp                 | 105 ++
```

| check | command | observed |
|---|---|---|
| G1 pull build | `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `.text 10792579 -> 10792579 (+0, +0.000%)`, `.data/.bss/.rodata +0`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`; file size identical (19100448 both sides) |
| generator | `python3 scripts/gen_pipe.py --check` | `71 calls, 69 verify payloads, 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes`, `0 UNMAPPED`, `generated files are up to date`; `git status` clean afterwards |
| generator self-test | `python3 scripts/gen_pipe.py --self-test` | `6 negative-control trip(s), positive control OK` |
| unit, pull | `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | `100% tests passed, 0 tests failed out of 1485` |
| unit, push | `ctest --test-dir build-push -L unit ...` | `11 failed out of 1485` — exactly the F3 list owned by `p1/fix-tests` (`DirectGLESSanity` ×2, `DirectGLESTextureSync` ×1, `FramebufferTest` ×7, `TextureTest` ×1) |
| unit, verify | `ctest --test-dir build-verify -L unit ...` | same 11, same names |
| `ctest -N` | `comm` vs `~/w7/p1-before-ctest-names.txt` | 0 removed, 29 added; vs `~/w7/pipe/build-linux` names: 0 removed, **exactly 3 added**, all `PipeInputsTest.*` from this package. `build-linux` and `build-push` name lists are identical (`diff` empty) |
| integration-verify | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | `rc=0`, `100% tests passed, 0 tests failed out of 818`, `grep -c "Fatal{UnmigratedPipeInput"` = 0, `grep -c "Fatal{PipeVerifyDiffer"` = 0. All eight formerly-aborting `DirectVulkan.Verify.{UnboundImageDescriptorScenario,SampledSetStalenessScenario}.*` entries pass |
| the four controls | `ctest --test-dir build-verify -R 'PoisonOmitted\.\|VerifyCorrupted\.'` | 4/4 pass (`DirectGLES/DirectVulkan.VerifyCorrupted.PipeVerifyArmingScenario.CorruptedFieldIsReported`, `...PoisonOmitted.PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb`) |
| 79-case retrace | `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p1-fix-core --lib .../build-verify/libMobileGL.so --out ~/w7/fcr-retrace -j 4` | `passed 79 / 79; failed: []` |
| retrace arming | in `~/w7/fcr-retrace`: `ls *.log` = 79; `grep -l "armed, zero divergences, zero unmigrated reads" *.log` = **79**; `grep -l "Fatal{" *.log` = 0; `grep -rn "UnmigratedPipeInput\|PipeVerifyDiffer" .` = nothing |
| include closure | `python3 scripts/check_include_closure.py` | `3 probes, 0 skipped, 0 problem(s)` |

The arming assertion is not decorative: `tools/trace_replay/run_trace_case.cmake:164` does
`string(FIND "${pipe_verify_log}" "MGPipe: verify armed" ...)` and `:174` greps the case log for
`Fatal\{(PipeVerifyDiffer|UnmigratedPipeInput|PipeVerifyBadKnob)`. Spot-checked a case log
directly: `improved-transparency-minecraft-26.3/DirectGLES/output/mobilegl.log:329` carries
`MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`.

Note the `build-push` incremental build had 50 targets to relink when I started
(`[50/50] Linking ... MobileGLIntegrationTest`), i.e. the implementer's `build-push` was one step
stale when their table (c) was produced. Re-running after a full rebuild reproduces their numbers
exactly, so the claim stands.

---

## 2. The attacks that were tried and failed

### 2a. "Zero divergences was achieved by disabling the read hook"

Refuted three ways.

1. `MobileGL/MG_Impl/Pipe/PipeFill.cpp` gains **only** the new `MGPipeNoteFrontendMutation`
   (+ one `#include`). `MGPipeVerifyReadHook` (`PipeFill.cpp:439-462`) is byte-identical to
   `97b997d5`: it still re-reads the whole field out of the live context
   (`MGPipeFillAccess::CopyField(g_readScratch, *ctx, field)`) and still calls
   `ReportDivergence(field, "read")`.
2. Every `integration-verify` entry still carries `MOBILEGL_PIPE_VERIFY=1` in its ctest
   `ENVIRONMENT` property (verified in
   `build-verify/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest[17]_tests.cmake`), so the
   comparator is armed for all 818.
3. **Direct falsification.** I inserted `return;` as the first statement of
   `MGPipeNoteFrontendMutation` (`PipeFill.cpp:479`), rebuilt `build-verify`, and re-ran:
   ```
   ctest --test-dir build-verify -L integration-verify \
         -R 'UnboundImageDescriptorScenario|SampledSetStalenessScenario' -j 4
     -> 8 tests failed out of 30, all Subprocess aborted:
        DirectVulkan.Verify.SampledSetStalenessScenario.{AFilterChange,ASamplerObject}CompletesTheTexture
        DirectVulkan.Verify.UnboundImageDescriptorScenario.* (6)
   ctest --test-dir build-verify -R 'PipeInputsTest\.(AFrontendMutation|TheMutationNotice)'
     -> 2 of 3 fail; child log carries
        MGPipe: Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@DrawArrays", verb=1, where=read}
   ```
   That is the lane failure from `P1-LANE-FINDINGS.md` §F2, reproduced exactly, and it proves the
   read hook is live and that the notice is the thing carrying the fix. The edit was reverted
   (`git checkout --`), `build-verify` rebuilt, and the three tests are green again;
   `git status --porcelain` is clean.

### 2b. "The F2 mechanism silences the comparator / makes it compare a value against itself"

The notice refreshes the pushed **value at the point of the frontend write**, not at read time.
The read hook still compares the stored value against a fresh live read at every accessor call, so
any mutation path added later without a notice still reds the lane — demonstrated by 2a.3. No
compare is skipped, no field is excluded, `kMGPipeInputFieldSticky[]` is untouched, and the poison
stamp is deliberately not written (`PipeFill.cpp:479-491` copies the value only), so negative
control B and `Fatal{UnmigratedPipeInput}` both survive — confirmed by the four control entries
passing and by `PipeInputsTest.TheMutationNoticeRefreshesTheValueButNotTheStamp`.

This is the findings' explicitly preferred option, so it is not an undeclared deviation.

### 2c. "The enumeration missed fields" — re-derived from the code, independently

The result file's Step 1–3 was not taken on trust. Re-derivation:

* **Backends cannot reach any `GLContext` mutator.** Gate G2 holds (`grep -rc pGLContext
  MobileGL/MG_Backend` is empty on the integrated tree), so the only context-level calls a backend
  can make are the `PipeInputs` accessors, and the only non-const ones there are the three
  forwarders `RecordError`, `InvalidateCompileEnv`, `ValidateProgramName`
  (`MG_Impl/Pipe/PipeFill.cpp:537-551`) — all F-class/sticky with no storage. So a backend can only
  move a pushed value *through a frontend object that calls back into the context*.
* **That callback surface is exactly five call sites, on three methods.**
  `grep -rn "pGLContext->" MobileGL/MG_State/ | grep -v Core.cpp` yields, excluding comments:
  - `MG_State/GLState/SamplerState/SamplerObject.cpp:34` → `BumpSamplingResolutionGeneration()`
  - `MG_State/GLState/TextureState/TextureObject.cpp:37` → `BumpSamplingResolutionGeneration()`
  - `MG_State/GLState/TextureState/TextureObject.cpp:96` → `BumpTextureBindGeneration()`
  - `MG_State/GLState/TextureState/TextureUnit.cpp:39` → `BumpTextureBindGeneration()`
  plus `MG_State/GLState/TextureState/TextureState.cpp:142` → `BumpTextureBindGeneration()`.
  All five land in `TextureState::BumpSamplingResolutionGeneration` (`TextureState.h:131`) or
  `TextureState::BumpTextureBindGeneration` (`TextureState.h:109`), both hooked. The only other
  writes to the three counters in the whole tree are inside `TextureState::NoteUnitTouched`
  (`TextureState.h:78-100`), also hooked on both branches. Verified by
  `grep -rn "m_textureBindGeneration\|m_samplingResolutionGeneration\|m_maxTouchedUnit"`: no write
  outside `TextureState.h`.
* **Three fields re-derived, one field ruled out.**
  1. `GetSamplingResolutionGeneration` — moved by `SamplerObject::BumpVersion`
     (`SamplerObject.cpp:34`) and `TextureObjectBase::BumpShapeVersion` (`TextureObject.cpp:37`);
     hook at `TextureState.h:131-134`. Covers all sampler-setter and storage-shape writers.
  2. `GetTextureBindGeneration` — moved by `TextureUnit.cpp:39` (sampler unbind),
     `TextureObject.cpp:96` (a context default texture becoming defined) and
     `TextureState.cpp:142` (delete-unbind), plus the `bindingChanged` branch of `NoteUnitTouched`;
     hooks at `TextureState.h:97-99` and `:109-112`.
  3. `GetMaxTouchedTextureUnit` — moved only by the high-water branch of `NoteUnitTouched`
     (`TextureState.h:79-86`); hook is inside that branch, after the assignment.
  4. **`GetTouchedBufferBindingPointCount` ruled out** (the one V-class high-water counter the
     result file does *not* discuss by name): its only mutator is
     `GLContext::TouchBufferBindingPoint` (`Core.h:83`) and its only callers are
     `MG_Impl/GLImpl/Buffer/GL_Buffer.cpp:1499,1622` — frontend entry points, never a backend.
* **Everything else checks out.** `GetRenderStateParameters*`/`GetPipelineStateVersion` move only
  through `RenderState` setters (`RenderState.cpp`), and `RenderState` is a private member of
  `GLContext` with no backend-reachable handle; `DirectGLES`'s `ScopedEmulationDrawState`
  (`DirectGLES.cpp:5226-5345`) saves/restores **native** GL through `g_GLESFuncs.glGetIntegerv`
  only — no frontend write, so the `IsTransformFeedbackPaused` worry the `kReadback` comment
  raises is not a mutation. The transform-feedback counters move only through `GLContext` methods
  (`Core.h:308-357`), unreachable from a backend. The `*Impl::` namespaces the backends use
  (`BufferImpl`, `TextureImpl`, `SamplerImpl`, `FramebufferImpl`, `RenderStateImpl`) are
  **backend-local** (`MG_Backend/DirectGLES/Managers.cpp:494,2751,5078,8490`,
  `DirectGLES.cpp:352,1299,1882,1983`), not `MG_Impl`. The one genuine `MG_Impl` helper a backend
  calls, `MG_Impl::GLImpl::CopyTextureImageToClientOrPBO_State` (`VulkanRenderer.cpp:10768`,
  defined `GL_Texture.cpp:5375`), only reads and `RecordError`s. No `MGP_FILL` is reachable from
  inside a verb (no nested fill).

Conclusion: the enumeration is complete for this tree. I could not find a fourth field.

### 2d. "A fill row that hides a divergence instead of fixing it"

A `FillPoints.def` row can only suppress `Fatal{UnmigratedPipeInput}` for that (class, field); the
value it then makes available is still compare-at-read verified against the live context, so a row
cannot hide a `PipeVerifyDiffer`. The two lane-driven rows are correctly derived:
`VkClearManager.cpp:58` reads `MGB_CTX->IsCapabilityEnabled(FramebufferSrgb)` inside
`PreCompensateSrgbClearColor`, reached from `VulkanRenderer::MaterializePendingClearForTexture`
(`:7865`, calls at `:7967,7995`), which `GenerateMipmap` calls at `:11000` (kTextureOp) and
`PrepareStorageImageTextures` (`:5763`) calls from `DispatchCompute` (`:7054`) /
`DispatchComputeIndirect` (`:7106`) (kDispatch). Verified. The other `IsCapabilityEnabled`
consumers (`MaterializeMultisamplePendingClear` `:8038`, `MaterializePendingClearForRenderbuffer`
`:8109`) are reached from `CopyImageSubData` (`:9513`), `ReadPixels` (`:9928`) and
`ReadDepthStencilPixels` (`:10307`) — classes `kBlitOrCopy`/`kReadback`, which already carry the
row. The `kReadback` "reformat" is genuinely row-neutral: the generated `kReadback` mask
(`0x5f50f3a041300541`) is unchanged in the `.inc` diff, and the class own-counts move by exactly
+1/+4/+3 = the 8 added rows.

### 2e. "A test helper that makes tests pass unconditionally / tests that cannot fail"

No helper was added or changed; the three tests use the existing `PipeInputsTest` fixture,
`Fresh()`, `RunInChild()`, `ExitedWith()`. Two of the three fail under 2a.3's perturbation. Both
value tests carry their own "the mutation actually took" guards
(`ASSERT_NE(ctx.GetSamplingResolutionGeneration(), samplingBefore)` at `PipeInputsTest.cpp:551`,
and `::_exit(7)` in the child at `:610`), so they cannot pass for the wrong reason. All three exist
as visible `GTEST_SKIP`s in the pull arm (`:173,176,179`) and skip visibly where their switch is
off — confirmed per build: `build-linux` 3 skipped, `build-push` 1 pass + 2 skipped (POISON and
VERIFY are both off there), `build-verify` 3 pass.

### 2f. "Something moved the pull build"

`.text`, `.data`, `.bss`, `.rodata`, total size and all 27799 symbols identical, and the `.so` is
the same number of bytes on disk. The `MG_State` header edit is three `MGP_NOTE_MUTATION`
statements plus an include that expands to nothing under `MOBILEGL_PIPE_PUSH=OFF`.

---

## 3. Majors

**None.**

---

## 4. Minors

1. **The notice keeps firing between verbs.** `m_currentVerb` is never reset at verb end
   (`PipeInputs.h:616` initialises it to `kVerbCount`, and only `MGPipeFillForVerb` writes it), so
   after any verb the last verb's mask stays installed and every subsequent frontend
   `glBindTexture` / sampler edit made by the *application* runs the full notice
   (`PipeFill.cpp:479-491`, including the `CopyField` switch) even though no verb is in flight.
   Harmless for correctness — the value only becomes *more* equal to the live context and the stamp
   is untouched — but `PipeMutation.h:33` ("a verb has been filled … in that verb class's may-read
   mask") reads as if the guard meant "inside a verb", which it does not, and the push build now
   pays the notice on the application's bind hot path. A `MGP_FILL`-side reset (or a
   `verb-in-flight` flag) would make the comment true and the cost proportional. Worth naming for P2.

2. **Six of the eight added `FillPoints.def` rows are static over-approximation, not lane-driven.**
   Only `X(kDispatch, IsCapabilityEnabled)` (`:179`) and `X(kTextureOp, IsCapabilityEnabled)`
   (`:246`) were forced by an observed Fatal. `X(kBlitOrCopy, GetViewportIndexed|
   GetDepthRangeIndexed|GetProvokingVertexMode|GetBufferBindingPoint)` (`:228-231`) and
   `X(kTextureOp, GetFramebufferBindingSlot|GetBufferBindingPoint)` (`:247-248`) come from a
   hand-verified but unexercised path (`TryBlitToDefaultFramebufferWithShader`,
   `GenerateDepthMipmapWithShader`). Each such row permanently weakens the poison for that
   (class, field) pair. The result file admits this for one row; it applies to five more.

3. **The notice's mask guard is untested.** `if (!MGPipeFieldMaskHas(mask, field)) return;`
   (`PipeFill.cpp:489`) has no covering assertion.
   `TheMutationNoticeRefreshesTheValueButNotTheStamp` (`PipeInputsTest.cpp:563-587`) says its
   `FenceSync` half proves "the notice stamped a field the verb class never fills", but the notice
   never stamps *any* field, so that half re-tests the stamp invariant rather than the mask branch;
   removing the mask guard entirely would leave all three tests green.

4. **`MG_State` now has a link-time dependency on a symbol defined in `MG_Impl`.**
   `TextureState.h:11` includes `MG_Pipe/PipeMutation.h`, whose `MGPipeNoteFrontendMutation` is
   defined in `MG_Impl/Pipe/PipeFill.cpp`. Fine today (one static library, one `SOURCE_FILES`
   list), and the header itself only depends on `MG_Pipe`, but it inverts the `MG_State < MG_Impl`
   layering at link time and `PipeMutation.h` is not one of `check_include_closure.py`'s three
   probed headers, so nothing gates what that include may drag in later.

5. **"Every case armed" is not provable per entry for the 818 integration entries.** All
   `Verify` entries share one log file per backend
   (`MOBILEGL_LOG_FILE_PATH=…/pipe-verify-DirectGLES.log`); after the full lane run that file
   contains no `MGPipe` line at all (each process truncates it). Per-entry arming rests on the two
   dedicated `PipeVerifyArmingScenario` entries plus the fact that the eight F2 aborts reproduce on
   demand (2a.3). Pre-existing lane design, not this package's doing, but the result file's "(d)"
   should not be read as per-entry arming evidence. The 79-case retrace *is* per-case armed (79/79).

6. **`ctest -L integration-verify` also reports 77 `Skipped` entries** among the 818 (741 real
   passes). Not caused by this package (no `MG_IntegrationTest` file is touched), but the "818
   passed" figure in the result file is 741 executed + 77 skipped.

7. **Two small undeclared deviations.** (a) `build-linux/CMakeCache.txt` in this tree carries
   `CMAKE_CXX_COMPILER:UNINITIALIZED=clang++` while the reference `~/w7/pipe/build-linux` and this
   tree's own `build-push`/`build-verify` carry `CMAKE_CXX_COMPILER:STRING=/usr/sbin/clang++`; the
   `.text` byte-identity shows it resolves to the same compiler, but the G1 comparison was made
   across a differently-spelled cache entry and deviation 2 does not mention it.
   (b) `MobileGL/MG_Pipe/PipeMutation.h` is a new file in a directory the P1 brief's D5 table
   enumerates; the new row is not called out as a deviation (it is implied by the findings'
   preferred option, so this is bookkeeping only).

---

## 5. Follow-ups for the integrator (not blocking)

- The F2 enumeration in `fix-core-v1.md` still has to land in `docs/Disaggregated/MEASUREMENTS.md`
  (the package correctly left it alone; the findings asked for the enumeration to exist, and it does).
- Minor 1 (verb-end reset) and minor 3 (mask-guard coverage) are natural P2 items when the tracker
  takes over the filler.
- `GetFramebufferBindingSlotFast`'s five bypassing reads (`DirectGLES.cpp:1611,1905,2743,2858,2933`)
  remain outside both the poison and the compare-at-read; unchanged, and correctly recorded.
