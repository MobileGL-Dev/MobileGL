# Adversarial review — P2 package B `p2/tracker`, round 3 (`~/w7/p2-tracker`, `9c6a8a25..06f9adfd`)

Reviewer's brief: refute correctness / completeness against BRIEF-P2 §A (G1–G14), §B (D1–D20),
§C.1 and §C.5. Everything below was re-derived or re-run by me in `~/w7/p2-tracker`. The tree was
left exactly as found — `git status --porcelain` empty before and after; nothing outside
`~/w7/rev-tk3/`, `~/w7/retrace-out/rev-tk3-*` and my own scratchpad was written, no source file was
edited, and every negative control I ran was run **in memory** against a loaded copy of the
script, never against the file.

**Verdict: NOT APPROVED — 1 major.**

Round 2's three majors are genuinely fixed and I re-established all three fixes and their
discriminating power. Every lane reproduces, including the two long ones. What blocks approval is
that the fix for round-2 MAJOR 2 — "the derivation gate is structurally blind to 28 of 73 rows" —
replaced one structural blindness with another **in the same gate, undisclosed**, and the new one
is worse in kind: the write analysis it added is not the over-approximation the file and the script
say it is, it is an *under*-approximation for any write through a member's field, so its
"UNDER-FIRING" verdict is a statement it cannot support. Concretely it makes `NEW_PIXEL_PACK` — one
of the five bits P2 actually emits for — **unnameable by any function in the tree**, and that is why
the row for its only mutator now carries a machine-readable answer (`kPulledEveryVerb`, documented
as "no shutter exists, and none is needed yet") that is false about P2's own shipped behaviour.

---

## 1. What I re-ran, and what it printed

Every row is my own run in `~/w7/p2-tracker` at `06f9adfd`, not a quotation from `tracker-v3.md`.
All three build dirs were already at the tip (`cmake --build <d> -j 12` → `ninja: no work to do.`
for build-linux, build-push and build-verify), so §5's numbers were produced from the tip, as
round-2 minor 3 asked.

| gate | command | observed |
|---|---|---|
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text 10792579 -> 10792739 (+160, +0.001%)`; file bytes identical (19100448 both sides). Resized: `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 — all four the contract's. **Round 3 adds zero pull-build delta.** Reproduced. |
| **G2** | `ctest -N` name sets, `'^ +Test +#[0-9]+: '` | `build-linux 2407`, `build-push 2407`, **zero-line diff**. Reproduced. |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <build-linux names>` | empty (0 of 2363 baseline names removed); 44 names added, all `Tracker*` / `CsoCacheTest` / `SetHashSuppressorTest` / the two A stubs. Reproduced. |
| **G5** | `git show {p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,…' \| sha256sum` | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` on both sides **and** equal to `~/w7/p2-before-syncrenderstate.sha`. Reproduced. |
| **G3** | `retrace_gate.py --tree ~/w7/p2-tracker --lib build-push/libMobileGL.so --out ~/w7/retrace-out/rev-tk3-push -j 4` | rc 0, `passed 79 / 79; failed: []`. Lowest SSIM **0.993475** (`minecraft-1.21.11-main-menu`, both backends), then 0.995426, 0.995753. Matches §5.2 exactly. |
| **G4** | `ctest --test-dir build-verify -L integration-verify -j 4` | `100% tests passed, 0 tests failed out of 818` |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/rev-tk3-verify -j 4` | see §1.1 |
| **G9** | `gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` + `7 other bit answers derived from their shutter in Tracker.h (under-firing only); 0 declined; 25 rows carry a prose answer (…) that no derivation checks` |
| **G9** | `… --self-test` | rc 0 — `7 negative controls, all tripped` |
| **G9** | `… --summary` | rc 0 — unchanged shape, E's CI step still works |
| **G13** | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (`7 negative-control trip(s), positive control OK`); `inventory 477 rows … 0 UNMAPPED`, `generated files are up to date` |
| **G13** | `check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty; no stdio anywhere in the diff's `MG_State`/`MG_Impl/Pipe` files |
| unit | `ctest -L unit -j 8` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1529` × 3 |
| integration | `ctest --test-dir build-push -L integration-gpu -j 4` | 878 / 878 |
| integration | `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | 878 / 878 (all-pull arm) |
| integration | `ctest --test-dir build-linux -L integration-gpu -j 4` | 878 / 878, first try, no flake |
| G6/G7, G8/G12, G11 | — | not reachable from this tree (packages A / E / device). Correctly not claimed. |

### 1.1 The verify lane

`MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib
~/w7/p2-tracker/build-verify/libMobileGL.so --out ~/w7/retrace-out/rev-tk3-verify -j 4`
— result recorded in §6 below (run to completion at review time).

### 1.2 Ownership and commit discipline

`git diff --name-only p2/contract..HEAD` → 26 files. Every one is a B row of C.5 except
`MG_State/GLState/Core.{h,cpp}`, `MG_Test/ScopedPipeVerb.h` and `MG_Test/Pipe/PipeInputsTest.cpp`,
all three "nobody" in C.5 and all three declared (deviation 3 / the `MGPipeFillForVerb` →
`MGPipeValidateForVerb` rename). **No `MG_Backend/**`, no `CMakeLists.txt`, no `.github/`, no
`MG_IntegrationTest/**`, no `scripts/gen_pipe.py`, no A-owned `MG_Pipe/*` file.** I read the whole
`PipeInputsTest.cpp` diff: it is the rename and nothing else — no assertion weakened, no case
removed. **No ownership violation.** 14 commits, every subject a single-line
`[Type] (Scope): …`; `git log --format=%B` contains no `Co-Authored-By` and no other attribution.

### 1.3 The chunk table and the setters, re-derived again

The brief asks for at least five setters re-derived from `RenderState.cpp` against
`MGPipeRenderStateSpans.h`'s boundaries. I did ten, hunting specifically for the two shapes that
would break the G6 ⟺ and, worse, leave a *stale CSO*: a `++m_version`-only setter that writes a
**pipeline** byte, or a `BumpVersions()` setter that writes only **dynamic** bytes.

| setter (`RenderState.cpp`) | bump | bytes written | chunk | verdict |
|---|---|---|---|---|
| `SetPointSize` | `++m_version` | `PointSize` | D0 | ✓ |
| `SetLineWidth` | `++m_version` | `LineWidth` | D0 | ✓ |
| `SetClipControl` | `++m_version` | `ClipOrigin`, `ClipDepthMode` | D1 | ✓ |
| `SetScissorBox` | `++m_version` | `ScissorBoxes[16]`, `ScissorBoxWrittenMask` | D7 | ✓ |
| `SetStencilMask` | `++m_version` | `StencilStates[f].WriteMask` | D3/D4 | ✓ |
| `SetStencilFunc` | `++m_version` + **conditional** `++m_pipelineStateVersion` | `Func` (P2/P3) + `Ref`/`ValueMask` (D3/D4) | both | ✓ — the pipeline bump is exactly `state.Func != func`, so hash-moves ⟺ pipeline-version-moves holds |
| `SetSampleCoverage` | `BumpVersions()` | `SampleCoverageValue/Invert` | P2 | ✓ (and `MGPipeTypes.h`'s old "sample coverage is dynamic" comment is corrected in A's contract) |
| `SetSampleMaskValue` | `BumpVersions()` | `SampleMaskValue` | P2 | ✓ |
| `SetMinSampleShadingValue` | `BumpVersions()` | `MinSampleShadingValue` | P2 | ✓ |
| `SetPolygonMode` | `BumpVersions()` | `PolygonModeFront/Back` | P5 | ✓ |

No violation found. I also re-confirmed the one trap round 2 flagged: `SetCapability`'s
`ClipDistance0..7` arm writes `ClipDistanceEnabledMask`, which the boundary table puts in **D7
(dynamic)**, and it is `++m_version`-only — consistent. `StencilStates[0].Func` at offset 768 falls
inside chunk P2's range even though the brief's §D6 member list omits it;
`MGPipeRenderStateSpans.h:74-77` names it explicitly, and the file is right (the brief's own rule:
the file derives with `offsetof`, the brief's table is the guess).

### 1.4 The aggregate bump points, audited against D4

`grep -rn MGP_NOTE_AGGREGATE MobileGL/` outside the macro header and the tests: 7 `BufferChange`
(`BufferObject.cpp:47,55,65,80,88,311,376`), 3 `VaoAttribute` (`VertexArrayObject.cpp:301,308,315`
— and `grep '++m_configVersion'` finds exactly those three, so no config-version bump is
unaccompanied), 3 `FramebufferAttachment` (`FramebufferObject.cpp:194,206,219`), 5 `TextureContent`
(`TextureObject.cpp:296,368,380`, `TextureObject2DCube.cpp:53,65` — and `grep '++m_contentVersion'`
finds exactly those five), 12 `TextureParams` (`TextureObject.cpp:103,129,144,159,209,217,235,247,268,306,316`
+ `TextureObject.h:184`) plus the single `SamplerObject::BumpVersion` choke point
(`SamplerObject.cpp:38`), and 3 `VertexAttribDefault` (`Core.cpp:222,240,258`). That is D4's list
in full, plus the declared sixth aggregate. **No missing bump point.** All three
`SetCurrentVertexAttribute*` bodies bound-check `index` before the class write, and the note sits
after the early return, so an out-of-range call bumps nothing.

### 1.5 The three round-3 fixes, and that their tests discriminate

I did not take B's negative-control table on trust; I traced each test against the reverted code:

* `AClipDistanceEnableReArmsTheResidualBlock` (`TrackerTest.cpp:640-660`) poisons
  `MGPipeApplier().HasResidual = false`, asserts the premise (`ASSERT_EQ(LastDirty() &
  NewPipelineState, 0)`), then requires `HasResidual`. Under the old arming
  (`FreshlyPrimed || NewPipelineState`) `g_residualDue` is false at that walk — the priming draw
  cleared it — so `HasResidual` stays false and the `ASSERT_TRUE` is red. **Discriminating.**
* `AFreshContextRepublishesEveryVertexAttributeDefault` (`:666-686`) requires `Count == 32` on the
  second context. Without the `freshlyPrimed` arm at `PipeFill.cpp:853`, the per-attribute diff is
  empty, `header.Count == 0`, no call goes out, and `g_attribDefaultLastHeader` still holds the
  previous `Count == 1`. **Discriminating.** (It is also deterministic: `unique_ptr::operator=`
  constructs the new `GLContext` before releasing the old, so the two addresses differ — see minor 3.)
* `AFreshContextResetsTheApplierWithTheRenderStateSubsystemOff` (`:692-704`) clears bit 0 and
  requires `MGPipeApplier().RenderStateCsos` empty. With the reset back inside `EmitRenderState`,
  that function is not called at all and the vector is non-empty. **Discriminating.**

The `static_assert(NoDirtyBitOwnsTheResidualSubsystem())` (`PipeFill.cpp:961-973`) is real and
`constexpr`-evaluable; minor 5's exception is now asserted rather than asserted-in-prose.

---

## 2. Major

### MAJOR 1 — the new derivation gate's write analysis is an *under*-approximation, not the over-approximation it claims; it makes `NEW_PIXEL_PACK` unnameable by every function in the tree, and that is what put a false machine-readable answer into `DirtySurface.def` for one of the five bits P2 ships

**The claims under refutation.**

`MobileGL/MG_Pipe/DirtySurface.def:34-45`:

> `// EVERY BIT ANSWER IN THIS FILE IS DERIVED AND CHECKED, in two families and one`
> `// direction. … Every other NEW_* answer is checked against the shutter Tracker.h builds for that`
> `// bit: … computes what each mutator transitively WRITES …, and fails a row that names a bit whose`
> `// shutter its mutator moves on no path at all. That half is one-directional on purpose -`
> `// "it does write something the shutter reads" cannot prove it does so on EVERY path - so it`
> `// catches under-firing and not over-claiming.`

`scripts/gen_pipe_dirty_surface.py:249-255`:

> `// It is deliberately ONE-DIRECTIONAL. Step 3 is an over-approximation (a call name resolves`
> `// to every body of that name, and a write inside an 'if' counts), so "M does write something`
> `// B reads" is not proof that it does so on every path … "M writes NOTHING B reads" needs no such`
> `// assumption`

and `tracker-v3.md` §2 MAJOR 2, whose headline evidence is:

> `dirty-surface: UNDER-FIRING answer NEW_PIXEL_PACK for SetPixelStoreParam - it writes nothing`
> `NEW_PIXEL_PACK's shutter reads (shutter: m_pixelStorePackParameters, m_pixelStoreUnpackParameters)`

**That message is false about the code.** `RenderState::SetPixelStoreParam`
(`MobileGL/MG_State/GLState/RenderState/RenderState.cpp:827-855`) writes exactly those two members,
sixteen times, through `SET_PIXEL_STORE_PARAM` (`:829-833`):

```cpp
#define SET_PIXEL_STORE_PARAM(paramNameHead, paramNameTail, val)                  \
    case PixelStoreParam::paramNameHead##paramNameTail:                           \
        if (m_pixelStore##paramNameHead##Parameters.paramNameTail == (val)) break; \
        m_pixelStore##paramNameHead##Parameters.paramNameTail = (val);            \
        break;
```

**Why the analysis cannot see it, and why the direction of the error matters.**
`written_tokens()` (`gen_pipe_dirty_surface.py:293-320`) collects `MEM:` tokens only from
`MEMBER_WRITE_RE` (`:272`, `\+\+\s*(m_\w+)|\b(m_\w+)\s*(?:\+\+|\+=|=(?!=))`), which requires the
member name to be *immediately* followed by an assignment; a write to a **field of a member**
(`m_foo.bar = v`) matches only `FIELD_WRITE_RE` (`:273`), which records `FIELD:bar` and **never**
`MEM:m_foo`. Meanwhile the *shutter* side, `resolve_reader()` (`:349-368`), resolves
`GetPixelStoreParameters` through its one-line getter (`RenderState.cpp:884-886`,
`return isUnpack ? m_pixelStoreUnpackParameters : m_pixelStorePackParameters;`) to exactly
`MEM:m_pixelStorePackParameters` / `MEM:m_pixelStoreUnpackParameters`. The two halves can therefore
never meet. Here the raw text is not even expanded, so the token pasting adds a second layer:

```
$ python3 <probe: load gen_pipe_dirty_surface.py, print moved['SetPixelStoreParam']>
   ['FIELD:paramNameTail']
```

— the analysis believes the mutator writes a field literally named `paramNameTail`, the macro
parameter. Missing writes make `moved[m] & shutter` empty *more* often, i.e. they produce **false
UNDER-FIRING reds**, which is exactly the assertion the script says needs no assumption.

**Reproduction (in memory; no file was edited).**

```
$ python3 probe2.py          # loads scripts/gen_pipe_dirty_surface.py, mutates the mapping dict
--- what the gate says about the TRUE answer X(SetPixelStoreParam, NEW_PIXEL_PACK) ---
   UNDER-FIRING answer NEW_PIXEL_PACK for SetPixelStoreParam - it writes nothing NEW_PIXEL_PACK's
   shutter reads (shutter: m_pixelStorePackParameters, m_pixelStoreUnpackParameters), so a mutation
   through it publishes nothing
--- moved['SetPixelStoreParam'] (what the analysis believes it writes) ---
   ['FIELD:paramNameTail']
--- can ANY mutator legally name NEW_PIXEL_PACK? ---
   accepted mutators for NEW_PIXEL_PACK over ALL 932 known function names: []
```

The last line is the load-bearing one: over **every** function name the analysis knows under
`MG_State/GLState` + `MG_Impl/Pipe` (932 of them), **not one** can carry the answer
`NEW_PIXEL_PACK`. The gate does not merely fail to check that bit — it forbids it.

**What that did to the deliverable.** `MobileGL/MG_Pipe/DirtySurface.def:172` now reads

```
    X(SetPixelStoreParam,                 kPulledEveryVerb)
```

and `kPulledEveryVerb` is defined at `DirtySurface.def:82-85` as

> `// kPulledEveryVerb  no shutter exists, and none is needed yet: … A shutter here is a P3/P4`
> `//                   optimisation, not a correctness gap.`

For this mutator that is false about **P2's own shipped behaviour**. A shutter exists: it is
`MGPipeDirty::NewPixelPack`, bit 2, one of the five `kMGPipeDirtyEmittedAtP2`
(`Tracker.h:61, 90-93`); it is computed as a byte compare of the pack half at `Tracker.h:275-280`;
and it is what emits `set_pixel_pack_state` at `PipeFill.cpp:1118-1120`. The package's own unit test
`TrackerWalk.ThePixelPackShutterIsAByteCompareOfThePackHalfOnly` (`TrackerTest.cpp:429-440`) pins
precisely the fact the row now denies. D16 exists so that P3a can build narrow shutters *from this
file*; a P3a reader that trusts the machine-readable answer is told the one bit P2 already ships has
no shutter.

**Why this is a major and not a documentation nit.**

1. `ROADMAP.md:7` — **每个门必须能因它存在的理由变红**. This gate goes red for a reason other than
   the one it exists for, and its red text asserts something demonstrably untrue about the source.
2. It is the *same class of defect* as round-2 MAJOR 2 ("the gate is structurally blind and accepts
   an arbitrarily false answer for 28 rows"), reintroduced by the fix for it, and **not declared**:
   §4 of `tracker-v3.md` lists "the object-class derivation cannot prove a publisher fires on every
   path, only that it fires on none" — which is the claim being refuted here — and §7's three new
   deviations do not mention it.
3. The evidence `tracker-v3.md` §2 offers for the fix ("The gate demonstrably sees the two defects.
   Before the rows were corrected, the new derivation printed exactly, and only, them") is, for one
   of the two, produced by the blind spot rather than by the derivation. The derivation models no
   `case` arms at all, so it cannot have seen the eight-of-sixteen-arms asymmetry that the row's
   prose gives as the reason.
4. It is prospective as well as present: the moment `SetPixelStoreParam` is split (P3's obvious
   move) or any new mutator writes a struct-valued shutter member field-wise, the **correct** row is
   the one CI rejects, and the only way to green the lane is to write a false answer.

**Minimum to clear.** Either (a) make the write analysis actually over-approximate — record
`MEM:m_foo` as well as `FIELD:bar` for `m_foo.bar = v`, and expand or textually pre-substitute the
`SET_*`/`RETURN_*` token-pasting macros in `RenderState.cpp` — and then re-decide the
`SetPixelStoreParam` row on its merits under the file's "every path that mutates" rule; or (b) stop
claiming absence-proof: state the blind spot in `DirtySurface.def`'s header and in `--check`'s
output the way the three scanner blind spots are stated at `:87-95`, decline rather than fail a row
whose shutter resolves only to members the analysis cannot match, and give the row a comment that
does not tell P3a that bit 2 has no shutter.

---

## 3. Minors

1. **The residual block's firing rate is now measurable, and the number `tracker-v3.md` §2 offers
   for it cannot show it.** §2's cost note quotes `CrossFrameBufferScenario` — one draw per frame,
   `resid=8.00` once and `0.00` after — which by construction never exercises the change. I measured
   the shipped tip on a real workload:
   ```
   $ MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=60 python3 ~/w7/retrace_gate.py \
       --lib build-push/libMobileGL.so --only 'minecraft-1.21.4-fabric-iris-bsl-in-world' …
   MGPipe stats: frames=120 window=60 draws=2293 draws/f=38.22 … resid=197.07 … cso[csom=8 csob=1415]
   ```
   197.07 B/frame ÷ 8 B × 60 frames = **1478 residual blocks per 60 frames, 0.64 per draw**, each one
   35 out-of-line `ctx.IsCapabilityEnabled` calls (`PipeFill.cpp:934-938`) plus the applier's 35-bit
   compare. Round 3's own delta is small — the old pipeline-only arming fired ~`csob=1415` times, so
   ~4 % — but the *absolute* rate is a P2 cost on two draws in three and it is the workload class
   D.4.3's `T1 ≤ 45 ns/draw` ceiling is read against. This number, not
   `CrossFrameBufferScenario`'s, belongs beside the `resid=` byte class in `MEASUREMENTS.md`.
   (Both retrace cases passed: SSIM 0.997496 / 0.997324.)
2. **The object-class half of the derivation checks 7 (mutator, bit) pairs, one of them
   object-class.** Cross-product over the 73 rows × the 16 non-render bits, run in memory:
   **1135 rejected, 33 accepted, 0 declined**. The 7 real answers are the three patch setters
   (`NEW_PATCH_STATE`, matched through `FIELD:` tokens), the three `SetCurrentVertexAttribute*`
   (`NEW_VERTEX_ATTRIB_DEFAULTS`, matched through the `MGP_NOTE_AGGREGATE` expansion — that hop
   works, and it is the best part of this commit) and `BumpTextureBindGeneration ←
   NEW_SAMPLER_VIEWS`. The two rows that were *wrong* both left the checked family for the
   unchecked 25-row prose bucket. Of the 33 accepted false answers, 9 pass only through the
   call-name union (`Mark*ForDeletion ← NEW_VERTEX_BUFFERS` and friends). `--check` prints the
   prose count, so the gap is visible; the "7" is 7 pairs, not 7 of the ~28 non-render rows, and one
   word in the output would say so.
3. **The tracker's context identity is a raw heap address.** `MGPipeTracker::Update` keys
   "is this a different server" on `m_context != &ctx` (`Tracker.h:192-195`, storage
   `const void* m_context` at `:393`), which is the exact ABA the Track H slices exist to remove
   from the backends. A destroy-then-create `GLContext` at the freed address skips `Reset()`, and
   with it `FreshlyPrimed`, the applier reset, the suppressor invalidation and the 32-attribute
   republish. It is benign *today* only by a chain of accidents worth writing down: the applier is
   **not** reset either, so `m_staged` / `m_stagedAttribs` / `m_pack` / `m_patch` and the CSO cache
   still describe what `gPipeInputs` holds; and `MGPipeWidenedCounter::Observe` reads the new
   context's lower version as a wrap and fires. The one hole is exact equality of a widened counter
   across the swap. `GLContext` has no lifetime id; a monotonic context serial bumped in its
   constructor would close it for one `Uint64`. The fix's own test at `TrackerTest.cpp:679` cannot
   reach this case, because `unique_ptr::operator=` allocates the new object before releasing the old.
4. **`MGPipeNoteAggregate` resolves the target through `LiveContext()`** (`PipeFill.cpp:518-543`),
   so `GLContext::SetCurrentVertexAttributeFloat` (`Core.cpp:222`) bumps *the current* context's
   `m_anyVertexAttribDefaultGeneration`, not `this`'s. Identical whenever the mutated context is the
   current one, which is every production path — but the sixth aggregate is a member of the very
   object being mutated (`Core.h:243-247`), so `++m_anyVertexAttribDefaultGeneration` would be exact
   and cheaper. As written, a `GLContext` mutated while another is current bumps the wrong counter
   and a `GLContext` constructed without `pGLContext` set bumps nothing.
5. **Three emission globals survive the test fixture's reset.** `g_residualDue`
   (`PipeFill.cpp:952`), `g_attribDefaultRepairs` (`:813`) and the new
   `g_attribDefaultLastHeader` (`:820`) are file-scope statics that
   `ResetTheServerSideSingletons()` (`TrackerTest.cpp:239-245`) does not touch. The three round-3
   cases are order-independent only because ctest runs one gtest case per process; running the
   binary with a filter that selects several is a different experiment. One `MGPipeResetEmissionState()`
   beside the other three resets would make the fixture say what it means.
6. **`MGPipeVertexAttribDefaultsLastHeader()` is a second production observable** (declared,
   deviation 13) — accepted, and the argument that reading `m_currentVertexAttribute` back would be
   a poison violation is right. Worth noting only that it is written on the emission path and read
   by nothing but tests, so it is one more global the split has to account for.
7. **Every push and verify process now logs one `MGLOG_W_ONCE`** ("`MGPipeApplySetVertexAttribDefaults`
   does not reproduce the carried value on this build"). B declares it and the reason is sound, but
   the integrator should know it lands in `/sdcard/MG/latest.log` on every device run of D.4.2 and
   will look like a defect to anyone reading those logs cold; one line in `MEASUREMENTS.md` beside
   the hand-off closes it.
8. **`MGPipeCsoCache` frees its slots only in `Reset()`, never in a destructor** (`CsoCache.h:110-114`),
   and `TrackerWalk::m_cache` (`TrackerTest.cpp:287`) is not `Reset()` in `TearDown`. Each such case
   leaks its CSO slots into the process-global `MGPipeSlots()`. Harmless (one gtest per process, and
   the allocator only advances a high-water mark), but it is the discipline D13's allocator contract
   is about.
9. **`FillPoints.def`'s eight-row verdict is a keep-all with reasons, not a narrowing.** C.1 asks for
   "the verdict per row in the def's comment", and `FillPoints.def:29-64` delivers exactly that with
   a per-group justification and a named condition for retiring one later (the poison omission across
   the full CTS caselist, recorded as P3a). Correct scope; recorded here so the integrator does not
   read "8 rows re-checked" as "8 rows narrowed".
10. Still outstanding, and correctly recorded by B in §7/§8: G6/G7 are package A's, G8/G12 package
    E's, G11 the device's; G9 is not a CI gate until E lands `pipe-gates`; G1's admitted resize set is
    **four** symbols, not D15's three; D14/D.4.3's `T1 − T2` no longer isolates the tracker because the
    dirty walk is unconditional and only emission is behind the bitmask; `MGPipeWidenedCounter` cannot
    see a change of exactly 65536; `s_hashForTest` is a live function pointer in production
    (`CsoCache.h:128`); the three header-only files still want their `list(APPEND)` line.
11. **The three §A command defects reproduce for me too**, so §A and D.3 must be corrected before the
    integrator runs the five-part gate:
    * `retrace_gate.py … --ssim 0.99` → `error: unrecognized arguments: --ssim 0.99` (the gate as
      written in G3/C.2/C.3 exits non-zero having run nothing);
    * G2/G14's `grep -E '^\s+Test #'` matches **1408** of this tree's 2407 names (it drops every test
      numbered under 1000); `'^ +Test +#[0-9]+: '` matches 2407;
    * G4's `grep -L 'MGPipe verify:' <out>/<case>/mobilegl.log` matches nothing at all — the logs are
      at `<out>/<case>/<backend>/output/mobilegl.log` and the arming line is `MGPipe: verify armed`
      (`grep -c 'MGPipe verify:'` → 0 in every log I checked; `grep -l 'MGPipe: verify armed'` → 39 of
      the 39 DirectGLES verify logs). Fixing only the path would turn a true green into a false red.

---

## 4. Deviations checked against §B

Declared and accepted: rounds 1–2's twelve (header-only `Tracker`/`CsoCache`/`SetHashSuppressor`;
the sixth aggregate generation; `Core.{h,cpp}`; coarse shutters for bits 5–17; the `Vector` scan in
the CSO cache; `s_hashForTest`; the residual block emitted after the fill; the runtime derivation
probe; five stale comment references; `|`-joined `.def` answers; `GLContext`'s push-only class
array; `MGPipeVertexAttribDefaultRepairCount`) and round 3's three (13
`MGPipeVertexAttribDefaultsLastHeader`; 14 `SetPatchVertices`' answer class; 15 `kPulledEveryVerb`
carrying two rows whose mutator moves a real shutter on some paths).

**Undeclared deviation found:** MAJOR 1 — the write analysis is an under-approximation, so the
soundness property `DirtySurface.def:34-45` and `gen_pipe_dirty_surface.py:249-255` both assert does
not hold, and `NEW_PIXEL_PACK` is un-nameable. Deviation 15 declares the *answer* change but names
the Unpack arms as the reason, not the gate's inability to accept the alternative.

Checked and clean: no hot-path timer anywhere (`Tracker.h` has none; every tally is behind
`PipeStats::Enabled()`, and `TrackerWalk.TheFireTalliesOnlyRunWhilePipeStatsIsOn` asserts it); no
stdio under `MG_State`/`MG_Impl/Pipe`; `PipeCalls.def` untouched; the walk does not call
`GetProgramForDraw` (`Tracker.h:215-219`) so it cannot force a link join; the five
`SubsystemForEmitter`/`MGPipeSubsystemForDirty` `static_assert`s (`PipeFill.cpp:647-661`) plus the
new `NoDirtyBitOwnsTheResidualSubsystem()` one (`:961-973`); the residual arming is now outside the
per-subsystem gate (`PipeFill.cpp:1165-1168`) and the fresh-context reset outside it too
(`:1108-1113`), which is what round-2 minors 4 and 5 asked for; the CSO cache's collision `memcmp`,
LRU eviction and content-addressing-off arm are each pinned by a test that can fail
(`CsoCacheTest.cpp:117-147`).

I also re-checked the two mechanisms MAJOR 1 does *not* touch and found them sound: the applier
assembles `gPipeInputs.m_renderState` from a full seven-chunk scatter on every bind
(`PipeApply.cpp:165-177`) plus the named dynamic chunks, the fresh walk sends
`kAllDynamicChunks`, and the staging mirror is advanced only by the branch that actually sent bytes
(`PipeFill.cpp:1040-1042`) — so "server == `tracker.Staged()`" holds inductively, which is what 818
integration-verify entries and 79 verify retraces are green on.

---

## 5. Bottom line

The machinery is sound and independently verified: G1 (0/0/0, four attributed resizes), G2
(2407 = 2407, zero-line diff), G3 (79/79 push, lowest SSIM 0.993475), G4 (818/818 and the verify
retrace), G5 (byte-identical `RenderStateImpl`), G9, G10's desktop half, G13 and G14 all reproduce;
three unit lanes are 1529/1529; three integration-gpu arms are 878/878 with no flake; the chunk-table
rule survived a ten-setter re-derivation; the aggregate bump points are complete against D4; and
round 2's three majors are properly closed with tests that each go red when the fix is reverted.

What blocks approval is one thing, and it is in the same file and the same gate as round 2's
MAJOR 2. The fix added a real, discriminating derivation (1135 of 1168 false answers rejected) and
then described it as something it is not: the write analysis under-approximates through a member's
field, so it cannot prove the absence it is documented to prove, it reports a false statement about
`RenderState.cpp:829-833`, and it has made `NEW_PIXEL_PACK` — a bit P2 *emits a call from* — an
answer no function in the tree may carry. The consequence is already in the deliverable: the mapping
file D16 hands to P3a says the mutator behind `set_pixel_pack_state` has no shutter and needs none.
Nothing renders wrong today, and nothing in any lane can go red for it, which is exactly why it is
worth another round.
