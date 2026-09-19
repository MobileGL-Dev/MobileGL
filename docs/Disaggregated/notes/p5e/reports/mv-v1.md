# mv-v1 — the multi-draw path stops reading the frontend VAO

Branch `p5e/mv`, head `5c777b49`, base `7c6f6886`, tree `~/w7/p5e-mv`. Two commits:
`98ea1604` (the migration) and `5c777b49` (the tier lanes, added on the coordinator's ruling —
§6b). **`integration-split` is now 176, not 116; `integration-gpu` 1354, not 1294.**

## 1 Site inventory — what I actually found in `MultiDraw.cpp`

The task named `MultiDraw.cpp:93-98` and its caller. The read is one helper, but it had **five**
call sites, not the two the Kimi audit's row 128 lists (`MD:909, MD:930`):

| line (base) | caller | what it asked of the bound VAO |
|---|---|---|
| 413 | `RunIndirect` | presence only (`if (!indexBuffer) return false;`) |
| 505 | `RunRebasedDrawElements` | presence + `SyncPersistentMappedRange` / `SyncGpuWrites` / `MappedData()` / `GetSize()` |
| 553 | `RunRebasedDrawElements` | `BoundIndexBufferId()` — the restore name |
| 717/724 | `FlattenWithCompute` | presence + `EnsureBufferResource(...)->id` + `GetSize()` |
| 909 | `DrawElementsBatch` | presence (`BoundIndexBuffer() != nullptr`) |
| 930 | `DrawElementsBatch` | `BoundIndexBufferId()` — the restore name |

So the helper answers exactly **three** questions: *is one bound*, *what GL name is bound (and how
big is its store)*, *give me its bytes on the CPU*. That is the shape the migration took.

**Audit of the other apply-thread frontend reads in this file** (§2 of the task), all left alone:
`:46` `GetPrimitiveRestartIndex()` and `:52-53` `IsCapabilityEnabled(PrimitiveRestart /
…FixedIndex)` are filled for the draw verbs (no marker at base, none now). `:88`
`GetBufferBindingSlot(BufferTarget::DrawIndirect)` in `BoundDrawIndirectBufferId()` is a **latent
row and not mine**: `FieldOwnership.def:78` carries it as `BARRIER_PULLED, "P8 (indirect), P9
(readback), P13 (transfer)"`. It is unreachable in the lane because this driver resolves the `ext`
tier and `RunIndirect` never runs; forcing `MOBILEGL_ESPRYT_MULTIDRAW_MODE=indirect` under the
strict lane would abort `GetBufferBindingSlot@DrawArrays`. It retires with P8's indirect row.

## 2 What changed, per file (commit `98ea1604`; the test registration is §6b / `5c777b49`)

**`MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp`** (+150/-25, the block at `:94-250` plus the five
call sites at `:553`, `:644-670`, `:849-862`, `:1046`)

* `enum class IndexBufferQuestion { Presence, DriverName, DriverNameAndSize, HostBytes }` +
  `struct BoundIndexBufferView { Present, Id, Size, HostBytes }` + one resolver,
  `ResolveBoundIndexBuffer(question, entry)`.
* The four question values are **deliberately not nested levels**: each is exactly the work its
  original call site did, in its original order, so the monolith arm issues the same calls in the
  same sequence (requirement 3). `DriverName` does not read `GetSize()` because `BoundIndexBufferId`
  never did; `HostBytes` does not `Ensure` because `RunRebasedDrawElements` never did.
* Record arm: `MGPipeApplier().IndexBuffer.Res` via `BufferImpl::ResolveDrawIndexBufferFromRecord`
  (vi's own accessor, not a new one) → `EnsureBufferResourceForHandle(nullptr, res)->id` for the
  name, `ResourceWidthForHandle(res)` for the size, `FindBufferResourceForHandle(res)->hostBytes` +
  `RequireStagedCoverage` for the CPU bytes. **No wire row was added.** This is the identical set of
  calls `ScopedRestartIndexSubstitution`'s transport arm (`DirectGLES.cpp:8317-8372`) already makes
  for the single-draw restart rewrite, including the `HasDefinedContent` M-3 rule.
* `ResolveSubDrawIndices` takes `Bool hasIndexBuffer` instead of the frontend `SharedPtr`:
  presence was the only thing it ever asked of it. `FlattenWithCompute` asks twice (Presence, then
  DriverNameAndSize) so the ensure stays **behind** the `IsCaptureSpanOpen()` check, where it was.

**`MobileGL/MG_Backend/DirectGLES/Managers.h`** (+32 at `:1096`) and
**`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`** (-25/+4 at `:524`)

`BufferImpl::VertexInputReadsRecords()` **moved** from `DirectGLES.cpp:542` to `Managers.h`, body
and rationale unchanged, plus a paragraph saying why. `MultiDraw.cpp` is a second translation unit
on the same draw path and has to select the *same* arm; the addendum's rule ("state the transport
test where you rely on it") is met by a **shared** definition rather than by a second copy of the
conjunction that can drift. This is the only delta to shared code.

### Arm selection, against the addendum's checklist

* Decided by `BufferImpl::VertexInputReadsRecords()` ⇒ `Transport != Monolith && VertexInputSubsystemEnabled()`.
  Never by "a handle was noted", never by a null pointer. The null-`Res` test inside the record arm
  is *"no element buffer bound"*, the same statement as a null frontend slot on the other arm — and
  it runs **after** the arm has already been chosen.
* A missing record on the handle arm is a named refusal, `RefuseMissingIndexBufferRecord` →
  `Fatal{RoleViolation, "multidraw-index-buffer-arm"}`, shaped on
  `RefuseNullFrontendTextureOffTheHandleArm`, and entirely under
  `#if MOBILEGL_BUILD_DISAGGREGATED` so the pull build gains no call (D-P).
* Nothing passes a null object into a body that can still read one: on the record arm no frontend
  object is produced at all, and the three answers are values, not pointers into frontend state.

## 3 Rows of my family — retired / barriered / not found

* **Retired:** Kimi audit **row 128** (`MD:909`, `MD:930`, `BoundIndexBuffer()` / `BoundIndexBufferId()`,
  carrier `IndexBuffer` at `AP:620`) — retired, and with it the three sites the audit did not list
  (`MD:413`, `MD:505`, `MD:717`), which are the same helper.
* **Left barriered (named, with an owner):** `BoundDrawIndirectBufferId`'s
  `GetBufferBindingSlot(DrawIndirect)` — P8's indirect row, §1 above.
* **Not found:** the audit's rows 12/13 (`D:6729`, `D:6768/6777`) are the *arrays* multi-draw and
  never enter this file, and `PipeApplier.cpp:628` collapses `drawcount == 1` onto the single-range
  arm — so the nine entries are exactly "an indexed multi-draw whose record carries `NumDraws != 1`".

## 4 Red-once, verbatim

**(1) Revert the arm selection.** `if (false /* P5E-MV-RED1 */ && BufferImpl::VertexInputReadsRecords())`,
rebuilt, censused. Exactly the nine entries come back, nothing else moves
(`~/w7/p5e-logs/strict-census-mv/red1/census.tsv`: 9 × `GetBoundVertexArray@DrawArrays`,
7 × `GetProgramForDraw@DrawArrays` — the seven are other `MultiDraw*` entries that were already pa's):

```
[11:41:05] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetBoundVertexArray@DrawArrays"} [BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in P5e (Espryt unbarriered), P7 (Magma)]
```

**(2) Make the record lookup miss on the handle arm.** `resource = nullptr` after each by-handle
lookup, `MultiDrawScenario.PlainBatchMatchesUnrolledDraws`, split-lane env,
`MOBILEGL_LOG_FILE_PATH` set (`~/w7/p5e-logs/strict-census-mv/red2/`). Both entry points abort by
name, rc=134:

```
[11:41:59] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{RoleViolation, "multidraw-index-buffer-arm"} - RunRebasedDrawElements found no backend resource for the index buffer {2, 0} this batch's record named. The index buffer of this family is MGPipeApplier().IndexBuffer.Res (CONTRACT-P5E §5.1) and the arm is selected by Transport != Monolith AND the vertex-input subsystem bit (§5.8, ID-81); falling back to the frontend VAO's element slot here would draw this multi-draw from whatever the CLIENT has bound now
[11:41:59] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{RoleViolation, "multidraw-index-buffer-arm"} - FlattenWithCompute found no backend resource for the index buffer {2, 0} this batch's record named. The index buffer of this family is MGPipeApplier().IndexBuffer.Res (CONTRACT-P5E §5.1) and the arm is selected by Transport != Monolith AND the vertex-input subsystem bit (§5.8, ID-81); falling back to the frontend VAO's element slot here would draw this multi-draw from whatever the CLIENT has bound now
```

The `FlattenWithCompute` line does double duty: `FlattenWithCompute` returns before any of this
unless `ResolvedTier() == Compute`, and `Auto` never yields `Compute`. It is therefore the proof
that `MOBILEGL_ESPRYT_MULTIDRAW_MODE` really selects the tier in §6's sweep below.

**(3) Both put back.** `git diff` empty against `98ea1604`; rebuild rc=0; the nine entries are back
on pa's row (`~/w7/p5e-logs/strict-census-mv/green/census.tsv`), `isplit` 116/116.

## 5 Strict marker census, before and after

`~/w7/p5e-logs/strict-census/census2.tsv` (base) vs `~/w7/p5e-logs/strict-census-mv/census2.tsv`
(head). Copied to a per-slug directory so pa's reads of the shared one are undisturbed.

| marker | before | after | owner |
|---|---|---|---|
| `GetProgramForDraw@DrawArrays` | 61 | **70** | **pa** |
| `GetFramebufferBindingSlot@ReadPixels` | 21 | 21 | allowlist (readback) |
| `GetBoundVertexArray@DrawArrays` | **9** | **0** | **mv — retired** |
| `GetProgramForDispatch@DispatchCompute` | 7 | 7 | **pa** |
| `GetTextureObject@CopyImageSubData` | 6 | 6 | allowlist (P7) |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | 2 | allowlist (P9) |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | 2 | allowlist (CopyTex) |
| `<no-marker>` (census artifact, addendum §4) | 1 | 1 | — |
| total | 109 | 109 | |

The full `diff` of the two tables is exactly 9 lines out of my row and the **same 9 entries** into
`GetProgramForDraw@DrawArrays`: `MultiDrawElementsBaseVertexPaintsEverySubDraw` and the eight
`MultiDrawScenario` cases now get past the index buffer and stop on pa's row instead. **No row that
is not mine changed count for any other reason, and no new marker appeared.** When pa lands, these
nine go green with no further work here.

## 6 Gate

Final numbers, at head `5c777b49` (i.e. **with** the new lanes):

| step | result |
|---|---|
| `build` | rc=0 |
| `unit` | **2250/2250** |
| `isplit` | **176/176** (was 116/116; +60 new entries, §6b) |
| `gpu` (ID-109) | **1354/1354** (was 1294/1294; the same +60) |
| `gens` | all seven rc=0; doc citations only the pre-existing `CONTRACT-P5.md:524` ambiguity |
| `strict` (count) | 7/**176** (was 7/116): the 9 moved to pa's row, the 60 new ones land on it too |
| `strict` (per-entry census) | table in §5, unchanged — no backend source moved after it was taken |

## 6b The tier lanes (coordinator's ruling — done, commit `5c777b49`)

**Evidence first, because an entry that silently resolves to `ext` is worse than no entry.** I
temporarily promoted `MultiDraw.cpp`'s two `MGLOG_D` tier lines to `MGLOG_I` (`ResolveTierOnce`'s
resolution line and `NoteTierExecuted`'s "first batch executed via tier"), rebuilt, swept, reverted.
Every mode resolves to **itself** on this driver — no fallback:

| mode | `ResolveTier` says | tiers a batch actually executed | cases |
|---|---|---|---|
| `ext` | `ext -> ext` | `ext` | 17/17 |
| `basevertex` | `basevertex -> basevertex` | `basevertex` | 17/17 |
| `multiindirect` | `multiindirect -> multiindirect` | `multiindirect` | 17/17 |
| `indirect` | `indirect -> indirect` | `indirect` | 17/17 |
| `drawelements` | `drawelements -> drawelements` | `drawelements` | 17/17 |
| `compute` | `compute -> compute` | `compute` + `basevertex` | 17/17 |

Each line ends `(driver supports: ext, basevertex, multiindirect, indirect, drawelements, compute
(opt-in))`, so **no tier is refused here and none is skipped**. The `compute` row executing
`basevertex` as well is `ResolveTierForBatch` demoting the batch shapes the flattening tier declines
by design (strips, primitive restart, per-sub-draw base vertex) — the tier was selected, and three
of its cases fell through to the unrolled floor, which is the documented behaviour.

**Registered:** five blocks in `MobileGL/MG_IntegrationTest/CMakeLists.txt:2017-2087`, one per tier
`auto` cannot reach, prefix `DirectGLES.Split.MultiDrawTier{BaseVertex,MultiIndirect,Indirect,DrawElements,Compute}.`,
labels `integration-gpu;integration-split`, and `MOBILEGL_ESPRYT_MULTIDRAW_MODE` pinned as a ctest
`ENVIRONMENT` property (verified on a live entry with `ctest -N -V`, which prints the full
split environment plus `MOBILEGL_ESPRYT_MULTIDRAW_MODE=compute`). Filter:
`MultiDrawScenario.*:IndexedDrawFamilyScenario.MultiDraw*` minus the client-indices case the d1
block already excludes by name — 12 entries × 5 tiers = **60 new entries**, all green.

* **`ext` is deliberately not a sixth block:** `auto` already resolves to it, so the d1 block *is*
  the ext lane and a sixth would run it twice. Stated in the block comment so nobody re-adds it.
* **The filter is narrowed to the tier-sensitive workload**, this file's own stated rule for an
  armed `Split.` entry: `IndexedDrawFamilyScenario`'s other five cases are single draws that never
  enter `MultiDrawImpl`, so five copies of them would be five copies of the d1 lane. One word in
  `MGL_SPLIT_MULTIDRAW_TIER_FILTER` widens it to the whole scenario (+25 entries) if you prefer.
* **`Harness/SplitLogPaths.cmake.in`** gains a matching loop: every `DirectGLES.Split.` entry must
  own one private log path or `SplitLogPaths.PrivateAndDistinct` is red — it caught this on my
  first run, the only thing that failed, and it was the guard doing its job. For these lanes it also
  **repairs addendum §4's blind spot**: their third name segment means `strict_census2.sh` would
  re-run them without the very pin under test, so their markers are read from
  `build-split/…/split-logs/<entry>.log` instead (`~/w7/p5e-mv/.p5e/strict-new.sh`).
* **Strict-lane effect of the 60:** all 60 abort on `GetProgramForDraw@DrawArrays` — pa's existing
  row, because `PrepareForDraw` runs before the tier dispatch. **No new marker class, mv's row stays
  at 0.** Per-entry table: `~/w7/p5e-logs/strict-census-mv/tiers.tsv`.

## 7 G1 / monolith impact

* Every record-arm line is under `#if MOBILEGL_BUILD_DISAGGREGATED`. The monolith arm's body is the
  original text, and the four question values exist so that its *call sequence* is unchanged too.
* `gpu` (the `Transport == Monolith` runtime arm, ID-109) is 1294/1294.
* **Finding for the integrator, pre-existing and not mine:** the pull flavour
  (`MOBILEGL_PIPE_PUSH=0`, `MOBILEGL_BUILD_DISAGGREGATED=0`) does not compile at `7c6f6886` either.
  Syntax-checking the touched TUs with the split build's own compile line and the two flavour
  defines flipped (`~/w7/p5e-mv/.p5e/pullcheck.sh`): `MultiDraw.cpp` rc=0, `Managers.cpp` rc=0,
  `DirectGLES.cpp` rc=1 on two identifiers that predate this package —
  `DirectGLES.cpp:5554` calls `ComputeShaderStorageBlockBindingSignatureOf`, declared only inside
  `#if MOBILEGL_PIPE_PUSH` (`Managers.h:3154-3160`, pg's), and `DirectGLES.cpp:6675` declares
  `const Bool keysMatch` inside `#if MOBILEGL_BUILD_DISAGGREGATED` while the `:` else-arm and the
  use at `:6695` are outside it. Both are verbatim at the base commit (`git show
  7c6f6886:…DirectGLES.cpp` → `:5575` and `:6696`). So G1 for this package is "no new break, and my
  own TU is clean"; the tree's pull build is already red and nothing in the gate notices.

## 8 Seams, assumptions, and what I need from the integrator

1. **Seam with pa.** My nine entries now stop on `GetProgramForDraw@DrawArrays`. I touched no
   program row and no file pa works in; my only shared-code edit is `VertexInputReadsRecords()`
   moving out of `DirectGLES.cpp:524-549` into `Managers.h`, ~4000 lines from pa's region. If pa
   also moved something out of that `BufferImpl` block, that is the one place to check at merge.
2. **Assumed from vi/sb:** `ResolveDrawIndexBufferFromRecord`, `EnsureBufferResourceForHandle`,
   `FindBufferResourceForHandle`, `ResourceWidthForHandle`, `RequireStagedCoverage` and
   `GLESBufferResource::hostBytes` mean on the multi-draw path exactly what they mean in
   `ScopedRestartIndexSubstitution` and in `SyncNeccessaryBuffers`' record arm. Minted nothing.
3. **Tier coverage — ruled on, done (§6b).** The five unreachable tiers now have gated entries.
   Two consequences for the merge: `integration-split` is **176** and `integration-gpu` **1354**,
   and the strict lane's denominator moves with them (7/176). One judgement call is mine to flag:
   I narrowed `IndexedDrawFamilyScenario` to its `MultiDraw*` cases — one word to widen.
4. **Ruling wanted — the pull build.** §7. Noted as yours to verify; I have not touched it.
5. **Not done, deliberately:** `BoundDrawIndirectBufferId` (§1, P8's row — confirmed out of phase
   scope); no wire row; no change to the tier ladder, the scratch ring, or the restore semantics at
   `:102-105`, which the migration preserves by answering the *same* GL name from the record.

Scripts used, all under `~/w7/p5e-mv/.p5e/` (gitignored): `strict_census_mv.sh` (per-slug census,
output `~/w7/p5e-logs/strict-census-mv/`), `census9.sh`, `red2.sh`, `tier-sweep.sh`, `pullcheck.sh`,
`strict-new.sh` (markers of the 60 new entries, read from their own lane logs).
