# P2 package B — `p2/tracker` — result

Tree `~/w7/p2-tracker`, branch `p2/tracker`, branched from the contract commit
`9c6a8a25` (the `p2/contract` tag). Not pushed.

---

## 1. Commits

| sha | subject |
|---|---|
| `f52dd262` | `[Feat] (State): give the frontend six aggregate generations so a tracker can answer "did any bound texture, buffer, attachment or attribute move" with one Uint64 compare` |
| `9c9df515` | `[Feat] (Pipe): compute the per-verb dirty mask at the validate point and count how often each bit fires` |
| `8d000f7e` | `[Feat] (Pipe): mint render-state CSOs on the pipeline subset and send only the dynamic chunks that moved - the steady-state cost of the whole render-state family is now two Uint16 compares` |
| `a0c44c4f` | `[Feat] (Pipe): push pixel-pack, patch and vertex-attribute-default state as their own calls, the last one behind the set-hash suppressor` |
| `e54e7399` | `[Feat] (Pipe): carry what has no call of its own in the residual value block and abort when it disagrees with the assembled state` |
| `e49f0ea7` | `[Feat] (Pipe): map every frontend mutator onto the aggregate generation that publishes it, and make the dirty-surface scanner a gate` |
| `b3daa404` | `[Test] (Pipe): pin the tracker's shutters and the CSO cache's content addressing, including the collision the memcmp exists to stop` |
| `7993d711` | `[Test] (Pipe): give the pull build the same ctest names as the push build so a push-only case skips instead of vanishing` |

26 files, +2259 / -78. **No file owned by another package was touched** (see §5).

---

## 2. Verification — every command run and what it printed

### 2.1 The package's own list (brief §C.1)

| command | result |
|---|---|
| `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — `dirty-surface: 73 mutators, all mapped, no stale rows` |
| `python3 scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — `3 negative controls, all tripped` |
| `python3 scripts/gen_pipe.py --check` | rc 0 — `generated files are up to date`; `63 PipeInputs fields (7 sticky, 34 emitted by a P2 call)` |
| `python3 scripts/gen_pipe.py --self-test` | rc 0 — `7 negative-control trip(s), positive control OK` |
| `python3 scripts/check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| `cmake --build build-linux` + `symbol_report.py --threshold 0` | **0 added / 0 removed / 0 renamed**, 4 resized, `.text +160 (+0.001%)`. The four resized are the CONTRACT commit's own (`RenderState::{RenderState, SetCapability, IsCapabilityEnabled}` and `_GLOBAL__sub_I_DirectGLES.cpp`); this package adds **zero** pull-build delta on top of them. |
| `ctest --test-dir build-push -L unit` | **1518 / 1518 passed** |
| `ctest --test-dir build-push -L integration-gpu -j 4` | **878 / 878 passed** |
| `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | **878 / 878 passed** (the all-pull control) |
| `ctest --test-dir build-verify -L integration-verify -j 4` | **818 / 818 passed** |
| `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py --lib build-verify/libMobileGL.so -j 4` | rc 0 — **79 / 79 passed**, lowest SSIM 0.997009 |
| `grep -l 'Fatal{' .../p2-tracker/*/mobilegl.log` | **empty** |
| `grep -L 'MGPipe verify:' .../p2-tracker/*/mobilegl.log` | **empty** — every case armed |

### 2.2 Section-A gates this tree can reach on its own

- **G1** — green, as above.
- **G2** — `build-linux` and `build-push` ctest name sets: **2396 each, zero-line diff**.
- **G3 (SSIM half)** — the 79-case desktop corpus, both backends, lowest SSIM 0.997009
  under verify (which is push plus the comparator). The push-only arm was run separately;
  see §7 if its line is absent below.
- **G4** — verify build: 818 `integration-verify` green, 79 retraces green, no `Fatal{`,
  every case armed.
- **G5** — `SyncRenderState` byte identity:
  `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` on both sides,
  equal to `~/w7/p2-before-syncrenderstate.sha`. **Not one line changed.**
- **G9** — the dirty-surface gate, above.
- **G10 (the non-device half)** — `MGL_RESIDUAL_BLOCK_SIZE == 8` is the contract's
  `static_assert`; the **`resid=` byte class is now non-zero**, measured on the desktop:
  `MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1` over
  `CrossFrameBufferScenario` prints one window with `resid=8.00` and 63 windows with
  `resid=0.00`, which is the block going out once and then being suppressed. The same run
  shows one window with `cso[csom=1 csob=1]` and 64 with `cso[csom=0 csob=0]` — the mint,
  the bind, and then the steady state.
- **G13** — `grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'` empty; include
  closure clean; generators regenerate clean. No stdio added anywhere.
- **G14** — 2363 baseline names, **0 removed**; the tree now lists 2396.

Not reachable from this tree and left to their owners: **G6/G7** (`RenderStateSpans.` tests
and `scripts/g7_negative_control.sh` — package A and E), **G8/G12**
(`HandleRecycleScenario`, `CsoContentAddressingScenario` — package E), **G11** (the
two-device A/B — the integrator).

---

## 3. Deviations from the brief, with reasons

1. **`Tracker`, `CsoCache` and `SetHashSuppressor` are header-only, not `{h,cpp}`.**
   The brief's D15/C.1 asks for `.cpp` files, but registering a new source means editing the
   root `CMakeLists.txt`, which the C.5 ownership table gives to **package A** and which is
   frozen behind the `p2/contract` tag. The contract's `if (MOBILEGL_PIPE_PUSH)` block names
   only `MGPipeRenderStateSpans.cpp`, `PipeApply.cpp` and `SlotAllocator.cpp` — not
   `Tracker.cpp` or `CsoCache.cpp`. Rather than edit another package's file I made all three
   header-only; every one of them is included by exactly one translation unit in the library
   (`MG_Impl/Pipe/PipeFill.cpp`) plus the tests, so inline costs nothing. **Splitting them
   back out is one `list(APPEND)` line** whenever the integrator wants it.
   *This is the one thing in this package that would have been a blocking deviation, and it
   was routed around rather than reported as blocked.*

2. **A SIXTH aggregate generation, `VertexAttribDefault`, which D4 does not list.**
   D4 shutters `NEW_VERTEX_ATTRIB_DEFAULTS` on a `ContentHash` over all 32
   `CurrentVertexAttributeValue`s. That is ~768 bytes hashed on **every draw**, which does
   not fit inside the T1 ≤ 45 ns/draw ceiling the same brief pins in D.4.3. The hash still
   decides whether to **emit** (D11's set-hash suppressor, wired); the new generation decides
   whether to hash at all. It lives on `GLContext` rather than a state container because the
   values it guards do too.

3. **`MobileGL/MG_State/GLState/Core.{h,cpp}` were edited.** They are in no package's row of
   the C.5 table ("everything else | nobody"), so no other package's ownership was crossed,
   but B's file list does not name them either. They are needed because D4's bump points sit
   on OBJECTS, which have no back-pointer to the state container that owns them, so
   `MGP_NOTE_AGGREGATE` goes through the free function D4 itself allows and that function
   needs a `GLContext` facade. All of it is `#if MOBILEGL_PIPE_PUSH`; G1 is unaffected.

4. **The shutters for bits 5–17 are coarser than D4's table, and three of them are composed
   with a mixing hash.** D4 describes what each bit will eventually watch (a 32-attribute
   prefix, a recomputed framebuffer `ContentHash`, a `GetMaxTouchedUnit()` prefix). Those are
   per-draw scans and hashes, and P2 emits nothing from these bits, so the tracker uses the
   cheapest shutter that **cannot under-fire**: the aggregate generations, the bound object's
   identity, and the current program's version counters. Five bits share one buffer
   aggregate. Every one over-fires rather than under-fires, which is the safe direction, and
   the file says so where it is coarse. A composed shutter is a hash and can in principle
   collide; that is stated in the code and is acceptable only because nothing consumes those
   bits in P2.

5. **`CsoCache` uses a `Vector` linear scan, not `ska::flat_hash_map`.** 64 entries of
   `Uint64` is a handful of cache lines, the probe runs only when the pipeline version moved,
   and keeping the entries in one array is what lets LRU eviction answer "which is oldest"
   without a second structure. Behaviour (capacity 64, LRU, hash → probe → `memcmp` → handle,
   `delete_render_state` on evict) is exactly D7.

6. **`MGPipeCsoCache::s_hashForTest`, a test seam in production code.** A 64-bit collision
   between two different render states is silent wrong pixels and is precisely what the
   `memcmp` confirm exists to stop, so `CsoCacheTest.HashCollisionDoesNotAliasTwoStates` has
   to be able to make one happen. Null in every real build: one never-taken, perfectly
   predicted branch on a path that runs only when the pipeline version moved.

7. **The residual value block goes out AFTER the residual fill, and is held when the verb's
   class does not read the capability mirror.** D9 says "once per context and again whenever
   the capability set changes". Emitting it with the other calls compares the carried bits
   against the PREVIOUS verb's assembled mirror; and `IsCapabilityEnabled` is in seven of the
   nine class masks, so at a `kQuery` or `kXfbSpan` verb the mirror is stale by construction.
   The change is therefore **held** rather than dropped — dropping it would silently disarm
   the wire for a capability that moved between two queries. (This is not theoretical: the
   first version of the commit fired
   `Fatal{PipeResidualDiverged, "Dither"} carried=1 assembled=0` on 112 of the 818
   `integration-verify` entries. GL_DITHER defaults to enabled, so the very first mismatch
   the block could find is the one it found.)

8. **`MGPipeValidateForVerb`'s residual-fill skip is guarded by a one-shot runtime probe of
   the applier's derivation.** See §4.1 — this exists because of a real state of the contract
   tree, not because of a design preference.

9. **Five comment references to the old `MGPipeFillForVerb` name were left in package A's
   files** (`MG_Pipe/MGPipe.h:113`, `generated/PipeFilled.inc:404`,
   `generated/PipeFillPoints.inc:17`, `scripts/gen_pipe.py:914` and `:1012`). Fixing them
   means editing A's files. `MG_Test/ScopedPipeVerb.h` and `MG_Test/Pipe/PipeInputsTest.cpp`
   (owned by nobody) were updated.

---

## 4. Where the tree contradicted the brief

### 4.1 `MGPipeDeriveRenderStateFields` is a declared STUB on the `p2/contract` tag

The brief's D5 and C.1 assume the derivation exists (it is package A's commit `c1`, on
`p2/spans`, which this branch does not contain). Package B's headline deliverable — the
residual fill skipping the 29 derived fields — is therefore **not verifiable on this branch
alone**, because skipping a field the derivation would have written leaves its mirror
unwritten.

**Resolution, and it is self-healing rather than a hard-coded branch check:** the filler asks
once. It puts a sentinel in a scratch block's working `RenderStateParameters`, clears the
mirror the derivation is supposed to recompute (`m_clearStencil`), runs
`MGPipeDeriveRenderStateFields`, and looks. The answer is latched for the process and costs
one compare, once. On this tree the probe answers **false** and logs
`MGPipe: MGPipeDeriveRenderStateFields does not derive on this build - the render-state
mirrors stay on the pull path`; the moment A's `c1` is in the tree it answers true and the
full skip engages with no edit. It stays useful afterwards: if the derivation is ever deleted
or gated off, the filler degrades to **pulling** those fields rather than rendering a
default, which is the safe direction.

**What this means for the integrator:** after A's `p2/spans` lands, re-run
`ctest -L integration-verify` and the verify retrace on the merged tree — that run, and not
this one, is what proves the 26 derived mirrors. On this tree the eight fields the applier
writes DIRECTLY (`GetRenderStateParameters`, both versions, the patch trio,
`GetPixelStoreParameters`, `GetCurrentVertexAttribute`) are already skipped and already
proven by 818 verify entries and 79 retraces.

### 4.2 Two rows of `Coverage.def`'s `MGP_COVERAGE_EMITTED_LIST` cannot retire their pull

Both are contract-side defects in files this package does not own. Neither is worked around
silently; each is named in the code with its reason and the emission still happens, so the
wire shape, the payload bytes and the suppressor are real.

- **`GetPixelStoreParameters → SetPixelPackState`.** The `PipeInputs` field is BOTH halves of
  the pixel store (`m_pixelStore[0]` pack and `[1]` unpack) and `set_pixel_pack_state`
  deliberately carries only PACK (`ARCHITECTURE.md` 4.6 D5, `MGPipeTypes.h`). The unpack half
  has **no carrier at all**, so the field keeps being pulled. *Fix: either split the field, or
  record in `Coverage.def` that this row is partial.*
- **`GetCurrentVertexAttribute → SetVertexAttribDefaults`.** The three views of a
  `CurrentVertexAttributeValue` are **not bit-identical** — `GLContext` CONVERTS between them
  (`SetCurrentVertexAttributeFloat` writes `(Int32)value` into `intValue`) — while
  `MGPipeApplySetVertexAttribDefaults` `memcpy`s one `Data[4]` into all three and ignores
  `MGPAttribValue::ValueClass`, which the wire type carries precisely so it does not have to.
  Until that applier reads `ValueClass` the carrier cannot reproduce the frontend value.
  *Fix: `MGPipeApplySetVertexAttribDefaults` should switch on `ValueClass` and reproduce
  `GLContext`'s conversions, and the client should set `ValueClass` from
  `ClassifyVertexAttribType`.*

### 4.3 The brief's own G2/G14 grep is wrong

`grep -E '^\s+Test #'` matches only a **four-digit** test number: `ctest -N` right-aligns the
number, so tests 1..999 print as `Test    #7:` with more than one space. On this tree that
silently dropped **999 of 2368** names, i.e. the gate passes while looking at 58 % of the
list. The pattern that works is `grep -E '^ +Test +#[0-9]+: '` with
`sed -E 's/^ *Test +#[0-9]+: //'`. Both gates are green under the corrected command. The
integrator should fix the two commands in §A of the brief and in any CI step that copies
them.

### 4.4 Minor

- `MGPipeApplyDeleteRenderState` takes `MGPHandleOnly` (the tree), not the bare handle the
  brief's D2 signature list implies. Followed the tree.
- Three `integration-gpu` entries (`*.TheEmulationIsActuallyArmed*`,
  `*.TheRerouteIsActuallyArmed*`, `*.TheDemotionIsActuallyArmed*`) failed once under `-j 4`
  and passed standalone and on a re-run of the whole lane. Treated as a parallel flake of the
  lane, not a regression; recorded here because it will be seen again.

---

## 5. File ownership

Every file this package touched is one B owns in C.5, plus three the table gives to nobody:

- `MG_State/GLState/Core.{h,cpp}` — nobody (deviation 3 above).
- `MG_Test/ScopedPipeVerb.h`, `MG_Test/Pipe/PipeInputsTest.cpp` — nobody; both only follow the
  `MGPipeFillForVerb` → `MGPipeValidateForVerb` rename D1 mandates.

`CMakeLists.txt` (package A) was **not** edited — see deviation 1. No file under
`MG_Backend/` was touched at all.

---

## 6. Unfinished

1. **The 26 derived render-state mirrors are still pulled on this branch** — §4.1. Nothing to
   do: it engages by itself once package A's `c1` is in the tree. The integrator should re-run
   the verify lane and the verify retrace on the merged tree, because that is the run that
   proves the derivation.
2. **`FillPoints.def` tightening: the verdict is recorded, no row was removed.** All eight
   statically over-approximated rows are KEPT, per group, with the concrete backend path each
   one names written into the def's own comment. The reason none was removed is stated there:
   the only evidence that could retire one is DYNAMIC, and a corpus that never reaches a path
   proves nothing about it — a row dropped on that basis turns a rare path into
   `Fatal{UnmigratedPipeInput}` in a shipped build. What *would* retire a row is the
   `MOBILEGL_PIPE_POISON_OMIT` knob run across the full `gl44to46` caselist on both devices,
   which is recorded as P3a work. The contract's new `FramebufferSrgb` storage in fact makes
   one of the eight MORE load-bearing than it was: it used to read a compile-time constant.
3. **Per-bit fire rates for `MEASUREMENTS.md` were not collected.** The tallies are
   implemented (`MGPipeTracker::FireCount/WalkCount`, behind `PipeStats::Enabled()`) but
   nothing prints them on the `MGPipe stats:` line — that line's format lives in
   `PipeStats.cpp`, which is package A's. *Suggested one-line follow-up for A or the
   integrator: add the 18 fire counts to `FormatWindowLine`, or have the integrator read them
   through the accessor from a scenario.*
4. **No device work.** G11 (two devices, paired A/B), the DriverBench T1/T2 numbers, the
   blend-toggle table and the CSO negative-control device arm are all D.4 items and belong to
   the integrator. `TrackerTest.BlendToggleReusesTwoCsos` and
   `CsoCacheTest.ContentAddressingOffMintsEveryTime` are the correctness half of the last two.
5. **The `MGPipeSetHashSuppressor`'s other six slots are declared and unit-tested but not
   wired**, which is D11's own scope (P3b/P4b).

---

## 7. Push-arm retrace

`python3 ~/w7/retrace_gate.py --lib build-push/libMobileGL.so -j 4` was started after the
verify run and its result is in `~/w7/tk-retrace-push.log`. The verify run above is the
stronger evidence — it is the same push semantics plus the compare-at-read oracle — and it
reported 79/79 with a lowest SSIM of 0.997009.
