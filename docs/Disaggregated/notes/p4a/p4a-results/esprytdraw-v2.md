# P4a package E — `esprytdraw`. Rework result, v2

Branch `refs/heads/p4a/esprytdraw`, **HEAD `9b8441a6`**, rebased onto `refs/heads/feat/disaggregated`
(`17db7598` = c0 + c0b + c0c; **no c0d existed when this round started**, so `Tracker.h`'s two
under-fires are not reconciled here — see §6). One file, `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`,
**+1214 / −38** against `17db7598`. Nothing was pushed. `~/w7/p4a-esprytdraw-backup` (`0b00f7f0`,
DEV-13's net) was not touched.

All line numbers below are **HEAD (`9b8441a6`) line numbers in `DirectGLES.cpp`**.

---

## 1. The rebase, and the commit series

`git rebase refs/heads/feat/disaggregated` replayed all five original commits with **no conflicts**
(c0b/c0c touch `MG_Impl/Pipe`, `MG_Pipe` and `scripts/` only, and this package owns one backend
file). The five rebased commits are `ff07f5fa 08945879 7b79a1f2 e588cd55 d6127608`, and
`git diff 0b00f7f0 d6127608 -- DirectGLES.cpp` is **empty** — the rebase moved the base and nothing
else.

The rework is five further commits on top:

| # | hash | message |
|---|---|---|
| r1 | **`ba93dfda`** | `[Refactor] (Espryt): read the applier's framebuffer records through one accessor, carry D-K2's fourth row into the temporary latches and tick the declining epoch arm as a miss` |
| r2 | **`011c8d50`** | `[Fix] (Espryt): sweep every image unit the driver holds rather than only the pushed window, and say the seam line when a record names another texture at the validate point` |
| r3 | **`1aee17ca`** | `[Fix] (Espryt): unbind the sampler on every touched unit the pushed window does not describe instead of leaving an earlier draw's object on it` |
| r4 | **`f8002a5e`** | `[Fix] (Espryt): make a half-described framebuffer applier and a mis-keyed program record say the seam line, and refuse a short global-constant block instead of falling back to the frontend block` |
| r5 | **`9b8441a6`** | `[Refactor] (Espryt): index every decline site with the flip it takes at the verification round, and invalidate rather than stamp the forced framebuffer bind` |

Working tree clean at HEAD; `git status --porcelain` empty.

---

## 2. Every review item, and what happened to it

### MAJOR-1 — the image sweep's membership — **FIXED** (`2618-2673`, commit `011c8d50`)

`SyncImageTextureBindings`' handle arm no longer walks `[Start, Start + Count)`. It walks

```
[0, min(max(ShaderImageStart + ShaderImageCount, g_imageUnitHighWaterMark), unitCount))
```

— the record's window **unioned** with the backend's own high-water mark, starting at 0 because a
unit *below* `Start` is as undescribed as one above `Start + Count`. The reasoning is written at the
site (`2618-2656`): D-J2 and `PipeApply.h:461-464` are a rule about **record retention**, not about
which **driver** units a re-issue sweep must visit; the membership this loop needs is "every unit the
driver has an image on", which is `g_imageUnitHighWaterMark` — server-owned and monotonic, because
`SyncImageTextureBinding` is the only path to `glBindImageTexture` and raises the mark on every unit
it hands a texture. DEV-3's own argument, applied here rather than contradicted.

What the record still buys is real and is what `e3` was actually for: the four field **values**
(`Layered`/`Layer`/`Level`/`InternalFormat`) still come off it inside `SyncImageTextureBinding`, and
the walk now stops at the high-water mark instead of at the device's `MaxImageUnits`. The units in
`[end, unitCount)` the pre-handle arm additionally visits have never been given a texture through the
only funnel that can give one, so re-binding 0 on them is a provable no-op — that is the one
remaining behavioural difference from the pre-handle sweep and it is stated in the source.

**A8 added** (§4) and named in the source at `2637-2650`.

### MAJOR-2 — the sampler walk's stale binding — **FIXED** (`4656-4683`, commit `1aee17ca`)

`BindCurrentUnitSamplers`' record arm: a unit in `[0, maxTouchedUnit]` outside
`[SamplerStateStart, SamplerStateStart + SamplerStateCount)` now runs
`SamplerImpl::UnbindSampler(unit)` (`4682`) instead of `continue`. That is the pre-handle arm's
`else { UnbindSampler(unit); }` restored on the new arm, with its reason carried to the new site:
for a **driver** binding, "no record" means "no sampler object", and a sampler left on a unit by an
earlier draw keeps being applied — on a multisample texture the draw is rejected outright. The
alternative the review offered (decline the whole arm when the window does not cover
`[0, maxTouchedUnit]`) was **not** taken: unbinding is the cheaper of the two and is exactly what the
frontend walk does for the same units, so the two arms stay at parity instead of diverging on a
window shape nothing yet pins.

### MAJOR-3 — the two silent seams — **FIXED** (image `2454-2462`, program `3922-3936`; the log lines at `2458` and `3929`, commits `011c8d50` / `f8002a5e`)

All three seams now log the same stem, **`"does not describe the binding it names"`**, so the one
grep §8.2 mandates covers all three:

* framebuffer, `SyncCurrentFBOByRecord` `2895-2896` (unchanged wording — it is the stem);
* **image**, `ResolveShaderImageRecord` `2454-2462`;
* **program**, `ResolveGlobalConstantsRecord` `3922-3936`.

**One thing the review's fix as written would have broken, and how it is handled.** The image seam
cannot log unconditionally. `SyncImageTextureBinding` is *also* the eager funnel `glBindImageTexture`
runs, and at that moment the newest record legitimately describes the **previous** draw — this
package's own §4 says so. An unconditional `MGLOG_E_ONCE` there would fire on the first ordinary
frame of every application and the mandated grep would be worthless. So the seam is loud **only at
the validate point**: `g_imageRecordSeamIsAuthoritative` (`2433`, with its reason at `2423-2432`) is set around the
draw/dispatch sweep's loop and only there (`2665` / `2669`), which are the only two call sites where the set for
*this* draw has been applied. The program seam has no such funnel — its only caller is the global-UBO
upload at the draw validate point — so it is unconditional. Both facts are written at their sites.

### MAJOR-4 — `InvalidateFramebufferHandleArmMemos` — **HANDED TO D, recorded here as A9** (`2776-2790`)

Not fixable in this package (`Managers.cpp` is D's for the phase). Recorded in full at the site E
*can* reach — the function's own definition — naming the three `MG_Test/SanityTest.cpp` callers
(`2530`, `2778`, `2799`) that clear the pre-handle trio and leave `g_fboSyncedSerials` /
`g_fboRecordsTrusted` stale across a GLES function-table swap, and naming the fix: call it from
**inside** `InvalidateFramebufferBindingCache`. On D's rework list.

### The two SILENT decline sites that become loud NOW (ID-19)

**F2 — `SyncCurrentFBOByRecord`, the half-described applier** (`2863-2890`, commit `f8002a5e`).
The site is split into the two cases it was conflating:

* **neither** target recorded — the transitional state of a tree whose client half has not landed,
  which is *every* draw on this tree — stays silent and declines; the round flips it;
* **exactly one** recorded — loud, with the stem, naming which target has no record and the
  `{slot, gen}` of the one that does (`2882-2888`, the log at `2882`). No correct emitter can produce it: an emitter
  walks `{Draw, Read}` together and a `Target = Both` record writes both, so it is exactly the shape
  a partially landed emitter produces, and it is invisible in a pixel test because the arm just
  declines.

That split is why **the default arm is still 491/491 on this tree** (§5): with no client, both
records are null, which is the silent case. No test names to list as expected-failing.

**P7 — `ResolveGlobalConstantsRecord`, the short block image** (`3943-3977`, commit `f8002a5e`).
This is protocol corruption, not a missing record: the upload copies `GetUBOSize()` bytes out of
`GlobalConstants`, so a shorter block image is a read past the end of the applier's own buffer.

**A judgement the integrator should confirm.** ID-19 says it "must not fall back to `MapUBO()`". The
verdict implemented is the one the contract already spells for every trip wire it owns
(`PipeApply.cpp:22-40`, *"the verdict of every trip wire in this file, in one place"*): under
`MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY` it is `MGLOG_F("MGPipe: Fatal{ProtocolCorruption} …")`
(`3959`) + `std::abort()` — so the **gate lanes never reach `MapUBO()` at all**, and G4's `Fatal{` grep finds
it — while any other push build logs the same line at error level, once, and declines. I did not put
an unconditional `abort()` on a shipped game's draw path over a client-side bug, and inventing a
second trip-wire verdict in a backend file would contradict the contract's own one-place rule. If the
integrator wants the stop on every arm it is one `#if` away and the comment at `3943-3956` says so.
The `#if` arm was compile-checked (§5).

### MINOR-1 — the `ReadSurface` claim — **FIXED (comment corrected)** (`2972-2984`)

The comment no longer says the read buffer is applied "from the record's own `ReadSurface`" (it is
not; this package reads `Fbo`, `IsDefault`, `Target`, `DrawBuffers[]` and `ContentHash` and nothing
else). It now says what the change actually buys: the `lastUpdatedFBO` **pointer compare** became a
`record.Target == Both` **field test**, so the read-buffer-shared-FBO defect class can no longer be
produced by an accident of loop order — the defect **expressed as a field**, not made
unrepresentable. Reading `ReadSurface` is the D-C1 endpoint and belongs with A1's rewrite.

### MINOR-2 — ID-12's DV-5 reader assignment — **DECLARED, for the integrator**

Confirmed against this tree: E's diff contains **no `MGPSurface` read at all**, and
`SyncAttachmentObject` is `Managers.cpp` (D's). Nothing to do here; the c0c `TextureTarget` field
lands unread unless the integrator reassigns the reader to D (esprytobj's own source at
`Managers.cpp:8603` already believes it is E's, so the confusion is two-sided).

### MINOR-3 — `ForceBindCurrentFBO` stamping a record-keyed memo — **FIXED** (`4306-4326`)

`StampSyncedFramebufferSerial(target, …)` → `InvalidateSyncedFramebufferSerial(target)` (`4325`), a
new one-line helper beside the stamp (`2797-2801`). The stamp claimed "this target reflects the applier's
record as of this serial" from a path that syncs the **frontend** object out of the binding slot and
never consults the record. Like the review, I could not turn it into a wrong-pixel scenario; it is an
invariant break with no demonstrable failure, and the honest form costs at most one extra sync.

### MINOR-4 — the `PipeStats` gate cannot separate the arms — **FIXED** (`1938-1948`)

`CurrentUnitBindingsEpoch` now ticks `Gate::EsprytUnitBindingsEpoch` with `hit=false` on the
record-arm decline before falling through. Because the walk arm below ticks the *same* gate, every
declining call now contributes at least one miss, so §8.1's *"hit rate should go to 100%"* means
exactly one thing: the record arm answered every time. A distinct gate would have been cleaner but
`PipeStats::Gate` is not this package's file.

### MINOR-5 — the refuted D-K2 mirror sentence — **FIXED, and the fourth row added** (`222-250`)

The comment no longer claims the directions are the brief's three; it states ID-15's correction out
loud (*"D-K2 HAS FOUR ROWS, NOT THREE"*) and `EsprytDrawTextureResourceHandlesEnabled()` now also
requires bit 11 (`242`). The row is spelled as a **raw bit test**, not as a call to the sampler
latch, because the two `static const` latches would otherwise initialise each other. Net effect: the
texture-resource and sampler latches become the same predicate (bits 7 ∧ 10 ∧ 11), which is what the
mutual dependency means. No arm this round tests is affected (`0x1fff`, `0x1ff`, `0` all unchanged).
The four latches are still deleted at the rebase (A4); the comment now names D's **actual wrapper
names** so that deletion is mechanical.

### Also fixed, found this round and not in the review

**G13's `pGLContext` grep had a second hit, in this file** (`315-319`, commit `ba93dfda`). The v1
report's §7.2 recorded `grep -rc 'pGLContext' MobileGL/MG_Backend` as `PipeInputs.h:1`, but the D-F3
paragraph v1 added at `302-316` **spelled the token in a comment**, and G13's grep is a bare-token
grep — so the sentence claiming the purity gates were unaffected was itself the second hit. The
sentence now says the same thing without spelling it. G13 is back to **1** (§5).

---

## 3. The decline-site index

Every row of the review's §2 table has a `// P4a decline-site <id>: <M|S|K> - …` comment at its site,
each naming the flip it takes at the verification round. `grep -n "P4a decline-site"
MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` returns **30 lines** (29 table rows; F8 has two sites
and carries the marker at both).

| id | line | class | state now | flip at the verification round |
|---|---|---|---|---|
| F1 | 3008 | M | silent | none; becomes D's `FramebufferSubsystemEnabled()` |
| F2 | 2867 | S | **half-described = LOUD now**; both-null silent | flip the both-null case |
| F3 | 2891 | S | loud | keep verbatim — it is the stem |
| F4 | 2947 | S | loud, `continue`s | `g_fboRecordsTrusted = false; return false;` |
| F5 | 4185 | — | silent | unreachable; `MOBILEGL_ASSERT` or delete |
| F6 | 4191 | S | loud | keep (parity with the pre-handle arm: both bind nothing) |
| F7 | 4010 | — | silent | unreachable; same treatment as F5 |
| F8 | 2121, 2270 | K | silent | none — a memo-key selection, not a decline |
| F9 | 4314 | — | **fixed** (MINOR-3) | none |
| T1 | 1905 | M | silent | none; becomes D's `SamplerSubsystemEnabled()` |
| T2 | 1888 | S | silent (+ gate miss ticked) | `MGLOG_E_ONCE` then decline |
| I1 | 2437 | M | silent | none |
| I2 | 2441 | S | silent | loud-once **when the program declares images** |
| I3 | 2445 | M | silent | none |
| I4 | 2450 | — | silent | unreachable by `PipeApply.h:461-464`; keep as defence |
| I5 | 2454 | S | **LOUD now** at the validate point | none (see MAJOR-3 on the eager funnel) |
| I6 | 2653 | M | silent | none — the safe (wider) direction |
| I7 | 2656 | — | **fixed** (MAJOR-1) | none; only A8's measurement |
| S1 | 4646 | M | silent | none |
| S2 | 4650 | S | silent | loud-once, then the frontend walk |
| S3 | 4656 | — | **fixed** (MAJOR-2) | none; only A8's measurement |
| S4 | 4685 | M | silent | none — deliberately not memoised as a miss |
| P1 | 3913 | M | silent | none; becomes D's `ProgramSubsystemEnabled()` |
| P2 | 3882 | S | silent | loud-once, **distinguishing the composite band** |
| P3 | 3888 | S | silent | loud-once — ID-8's stale-generation refusal |
| P4 | 3919 | S | silent | folded into P2/P3 |
| P5 | 3922 | S | **LOUD now** | none |
| P6 | 3937 | M→S | silent | loud-once once bit 12 is on **and** `GetUBOSize() > 0 && HasGlobalUboBlock()` |
| P7 | 3943 | S | **LOUD now** (Fatal-shaped on poison/verify) | confirm the verdict (§2) |

---

## 4. The assumptions, reconciled against D's actual API

Read from `~/w7/p4a-esprytobj/MobileGL/MG_Backend/DirectGLES/Managers.{h,cpp}` (package D v1,
`9bb95335`), read-only. What follows is what the **source now says**, so the rebase is mechanical.

| # | reconciliation, as now written in the file |
|---|---|
| **A1** | `2928-2949`. D adds `BackendPtr* GetOrCreateByHandle(MGPipeHandle)` (`Managers.h:453`) returning a **pointer**, so the fallback is `GetOrCreateByHandle(record.Fbo)` **plus a null check**, not the one-liner v1 predicted; the `slot.GetBoundObject()` resolve above it and `FramebufferRecordMatchesBinding` go with it. The source also carries the **pointer-invalidation** warning (`Managers.h:381-385`): the current `twinSlot ? *twinSlot : GetOrCreate(currentFBO)` short-circuits safely, but `GetOrCreateByHandle` **can grow the table**, so the two must not share one expression after the rewrite. |
| **A2** | Nothing to do; confirmed unchanged in D (`Managers.h:1754`/`:1759` are textually the tag's). The `#else` half of A2 does not fire. |
| **A3** | Signatures hold, so the new read-FBO list compiles unchanged. **The open question is D's new record-keyed cheap gates** (`Managers.h:1709`, `:1715`): once `d2` keys `IsDrawSyncClean` on a record `ParamsSerial`, a **read-only** attachment's serial is one nothing on the read path ever advances — at which point `g_readFboTextureSyncList` may do no work at all. **I expect it still does work**, because `IsDrawSyncClean` requires *both* `m_syncedTextureParamsVersion == GetTextureParamsVersion()` and `m_syncedSamplerVersion == samplerObject->GetVersion()`, and neither is advanced by the read path either — so the first sync for a read-only attachment is still a miss and still pushes. **What the verification round must check** is stated in §6.5: instrument the new list's per-frame push count on the integrated tree at `d2` or later; a count of **zero** means D's re-keying turned D-E3's closure into a no-op and the closure has to move into D's gate instead. |
| **A4** | `202-250`. D's four are **wrappers**, and the names differ from v1's guess: `FramebufferSubsystemEnabled()`, `TextureResourceSubsystemEnabled()`, `SamplerSubsystemEnabled()`, `ProgramSubsystemEnabled()` (`Managers.h:649-663`), which hold the `static const` latch — the `Resolve<Family>SubsystemArm()` resolvers beside them are never the thing to call. The source now names all four and says the local latches are **deleted**, not "kept and updated". ID-15's fourth row is carried (MINOR-5). |
| **A5** | Confirmed against `Managers.h:2158-2169`; nothing to do. DEV-5 stands. |
| **A6** | Unchanged by c0b/c0c — `MGPImageView::Access` still has no documented encoding, so `e3` still does not read it (DEV-6). Stays a C item. |
| **A7** | Confirmed by reading `PipeApply.cpp`; §0's `MGPipeApplierReset` finding stands and is D's input. |
| **A8** *(new, the review's)* | Written into the source at **both** sites it binds: `2637-2650` (images) and `4671-4676` (samplers). Nothing in the contract pins the emitted window's membership to "every unit the driver holds"; until package C documents it in `ImageEmit.h` / `SamplerEmit.h`, MAJOR-1's union and MAJOR-2's unbind are load-bearing. §6.4 is the measurement that would let them be relaxed. |
| **A9** *(new, the review's MAJOR-4)* | Written at `2776-2790`. D's fix, recorded at the site E can reach. |

### The ID-19 framebuffer-record redesign — what I assumed

ID-19 makes the applier's framebuffer state a **per-object table keyed by the framebuffer handle**,
with `BoundFramebuffer[Draw|Read]` holding the bound handles, and says wire v3 keeps
`DrawFramebuffer` / `ReadFramebuffer` as **accessors of those names** resolving through the bound
handle "so E's `SyncCurrentFBOByRecord` keeps its shape".

To make that literally true, **every one of this file's reads of those two names now goes through
one function**, `BoundFramebufferRecord(FramebufferTarget)` at **`284-287`** (declared and argued at
`271-287`, commit `ba93dfda`). The seven call sites are the two F8 memo keys (`2127`, `2273`), the
two identity reads and the per-target loop in `SyncCurrentFBOByRecord` (`2863`, `2864`, `2905`), the
fragColor broadcast derivation (`4009`) and `BindCurrentFBO` (`4179`).

**What I assumed, explicitly:**

1. The names `DrawFramebuffer` and `ReadFramebuffer` survive wire v3 and still answer "the record
   describing the framebuffer bound to this target". If they land as **member functions** rather than
   members, this file's rebase is *one pair of parentheses inside `BoundFramebufferRecord`* and
   nothing else moves.
2. If the accessor can answer "there is no record at the bound handle" (a slot whose generation has
   moved, or a binding whose framebuffer never got a record), it answers with a **null `Fbo`** — which
   is already exactly what every caller here treats as a decline (F2/F5/F7/F8). If wire v3 chooses a
   different "absent" encoding (a null pointer return, say), `BoundFramebufferRecord`'s signature
   changes from `const MGPFramebufferState&` to a pointer and the four call sites take a null check;
   still one function plus four one-line edits.
3. A `Named` record (`MGPipeFramebufferTarget::Named = 3`) never reaches these six reads, because
   they always ask *by target* and a `Named` write does not touch the bound handles. Nothing in this
   file interprets `record.Target` except the `Both` skip at `2982-2983`, which compares against
   `Both` explicitly and so is unaffected by the new enumerator.
4. D's side of ID-19 (`PushedFramebufferRecord(target, fbo)` → `FramebufferRecordFor(handle)`) is in
   `Managers.cpp` and does not appear here; the local accessor is deliberately named
   `BoundFramebufferRecord` so it cannot be confused with D's.

---

## 5. Gates — every number

All in `~/w7/p4a-esprytdraw`, `CCACHE_BASEDIR=/home/swung/w7`, `-j 8`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported, at **HEAD `9b8441a6`** unless said otherwise.

```
build-linux (pull, PUSH=OFF)      rc 0
build-push  (PUSH=ON)             rc 0        0 warnings in DirectGLES.cpp on either arm

G1  symbol_report.py --threshold 0 --before ~/w7/p4a-before-libMobileGL.so
                                  --after build-linux/libMobileGL.so
    file bytes 19114360 -> 19114360 ; .text 10806323 -> 10806323 (+0, +0.000%)
    27811 -> 27811 defined symbols:  0 added, 0 removed, 0 resized, 0 renamed, 27072 unchanged
    ### Removed (0)  ### Added (0)  ### Resized (0)  ### Renamed only (0)          <- 0/0/0/0

scripts/p3a_untouched_regions.sh 37da3c3a HEAD                             rc 0
  (the 11 pool / deferred-release / ring / flush-drain functions byte-identical)

RenderStateImpl namespace sha256
  d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27
  == ~/w7/p4a-before-syncrenderstate.sha  and == the sha at 37da3c3a           UNCHANGED

D-N, the two in this package's files:
  DepthStencilSamplingReadImpl (a NAMESPACE, not a function)
  dfefc9964c82f23cb6b89292b2f155ebd2ac22ae442c18a959d38d5bc9063f53  at 37da3c3a AND at HEAD
  ShouldUseCaveatTextureFormat: git diff --stat 37da3c3a HEAD -- Utils.h Utils.cpp MultiDraw.cpp
                               is EMPTY (the whole files are untouched)

ctest --test-dir build-linux -L unit -j 8     100% passed, 0 failed out of 1638
ctest --test-dir build-push  -L unit -j 8     100% passed, 0 failed out of 1638
  (1638, not v1's 1635: c0c added three PipeCatalogueTest cases)

ctest --test-dir build-push -L integration-gpu -R DirectGLES -j 8
  default (0x1fff)      100% passed, 0 failed out of 491
  MOBILEGL_PIPE_PUSH=0x1ff  100% passed, 0 failed out of 491
  MOBILEGL_PIPE_PUSH=0      100% passed, 0 failed out of 491

C.4's 41-name family regex, -L integration-gpu --no-tests=error -j 4
  default 100% 497/497   |   0x1ff 100% 497/497   |   0 100% 497/497

G13   grep -rc 'pGLContext' MobileGL/MG_Backend           -> PipeInputs.h:1  (ONE file)
G13b  grep -rc 'MGPipeUnmigratedEmulation' .../DirectGLES -> DirectGLES.cpp:4  (+ D's 1 = 5)

MOBILEGL_PIPE_PUSH=ON, MOBILEGL_PIPE_LEGACY_MEMOS=OFF
  cmake configure rc 0 ; ninja -C build-nolegacy MobileGL rc 0     (dir removed afterwards)

MOBILEGL_PIPE_VERIFY=1 (the P7 trip wire's Fatal arm)
  clang++ -fsyntax-only on DirectGLES.cpp with the build-push flags + -DMOBILEGL_PIPE_VERIFY=1
  rc 0, no diagnostics                                     (the abort arm compiles)

Per commit (ba93dfda 011c8d50 1aee17ca f8002a5e 9b8441a6), all five:
  -fsyntax-only on both arms                  rc 0, no diagnostics in DirectGLES.cpp
  RenderStateImpl sha                         d8fd1c48716056c5…  (unchanged)
  DepthStencilSamplingReadImpl sha            dfefc9964c82f23c…  (unchanged)
  Utils.{h,cpp} / MultiDraw.cpp diff          0 lines
  p3a_untouched_regions.sh 37da3c3a <c>       rc 0

Hygiene: no TODO/FIXME/printf/std::cout in the diff; git status --porcelain empty;
         tools/trace_replay/fixtures/*.tgz are real files, 0 LFS pointers (ID-13) — no retrace
         was run, so `git lfs checkout` was not needed, but the tree is clean for one.
```

**The default arm shows no new failures.** The prompt anticipated that the two newly-loud sites might
red the default arm where a record is half-described; they do not, and the reason is structural
rather than luck: F2's loud branch fires only when **exactly one** of the two framebuffer records
exists, and on this tree neither does (the both-null case is still the silent decline, per §2), while
P5/P7 sit behind `FindShaderCsoRecord`, which returns null on a tree with no `ShaderCsos` at all. So
there is **no expected-failing set to name** for E v2. The first tree on which either can fire is the
integrated one.

### Not run here, and why

* **G3 / G3b / G4** (`retrace_gate.py`, the verify lane) — this worktree has no `build-verify`
  (DEV-13: it was created without the `verify` argument), and the round's instructions did not add
  one. They belong to the integrated tree, together with the two named traces (§6.7).
* **G5's `p4a_untouched_regions.sh`, G6, G7, G8/G8b/G9/G12** — packages B/C/F; none exists on this
  tree. D-E3's closure is still **implemented but not demonstrated**.
* **`ctest -L unit` ×3** — the third arm in C.3's list is `build-verify`; two arms were run.

---

## 6. The verification-round checklist (E, on the integrated tree)

0. **Do not run `wsl_tree.sh p4a esprytdraw p4a/contract`** (DEV-13) — it would reset the branch and
   drop all ten commits. `refs/heads/p4a/esprytdraw-backup` (`0b00f7f0`) is v1's net; this rework is
   only on `refs/heads/p4a/esprytdraw`. `git lfs checkout` in the worktree before any retrace.
1. **Take §4's reconciliations first**, in this order: A4 (delete `202-250`, call D's four
   **wrappers**), A1 (`GetOrCreateByHandle` + null check at `2946`, then drop the resolve above it
   and `FramebufferRecordMatchesBinding` with it), the ID-19 accessor question (§4 — expected to be
   zero or one edit inside `BoundFramebufferRecord` at `284`), then A3.
   **c0d is not in this branch**: it did not exist when this round started, so `Tracker.h`'s two
   under-fires (ID-17) are unreconciled here and the rebase must pick them up.
2. **Apply the §3 flips**, then run the default mask and assert **every** arm engages:
   `SyncCurrentFBOByRecord` returns true, `UnitBindingsEpochFromRecords` answers,
   `ResolveShaderImageRecord` returns non-null, `BindCurrentUnitSamplers` sets `walkedFromRecords`,
   `ResolveGlobalConstantsRecord` answers. `Gate::EsprytUnitBindingsEpoch`'s hit rate going to
   **100%** now means exactly that (MINOR-4).
3. **Zero seam hits.** `grep 'does not describe the binding it names'` across the itest and retrace
   logs must be empty for **all three** seams. A hit is a finding against B/C, never a reason to
   relax a check. Also `grep 'Fatal{ProtocolCorruption}'` for P7 on the verify lane.
4. **A8's measurement, which is what makes MAJOR-1/MAJOR-2 relaxable or permanent.** With the client
   emitting, log `(ShaderImageStart, ShaderImageCount)` against `g_imageUnitHighWaterMark` and
   `(SamplerStateStart, SamplerStateCount)` against `keys.maxTouchedUnit` for one Minecraft frame.
   If they agree everywhere, C can document the membership in `ImageEmit.h` / `SamplerEmit.h`, the
   two fixes become assertions, and DEV-3's `keys.maxTouchedUnit` can also move to the record. If
   they do not, both fixes are load-bearing and must stay.
5. **A3 / D-E3: does the new read-FBO list still do work after `d2`?** Count
   `g_readFboTextureSyncList`'s pushes per frame. **Zero means D's record-keyed cheap gates turned
   the closure into a no-op** and the closure must move into D's gate. This is the one item where my
   expectation (§4, A3: it still works, because neither of `IsDrawSyncClean`'s two version compares
   is advanced by the read path) has to be **measured, not argued**.
6. **The unit-list equivalence** (the review's §1 "Refuted", last item): compare
   `g_unitTextureSyncList` rebuild counts between `MOBILEGL_PIPE_PUSH=0x1ff` (walk arm) and `0x1fff`
   (record arm) over one frame. A large drop means the epoch substitution is missing rebuilds.
7. **Named traces**: `photon-v1.3b` on llvmpipe desktop retrace is the canary for `e3`'s image
   semantics and is the trace that would have caught MAJOR-1; `improved-transparency-minecraft-26.3`
   is the draw-buffer-`None` net for `e1`.
8. **The full C.4 verification**: G1, `p3a_untouched_regions.sh`, `p4a_untouched_regions.sh` (F's —
   it needs the **namespace** shape for the `DepthStencilSamplingReadImpl` row), `ctest -L unit` ×3,
   `-L integration-gpu` on all three arms, the C.4 family regex, G13's two greps, and
   `retrace_gate.py` on `build-push` **and** `build-verify`, plus the `LEGACY_MEMOS=OFF` compile and
   the `tcache_count=0` exit-order lanes on both backends — the latter now has a named item to
   confirm, `g_rawDepthFetchSamplerState` (the file-static `SharedPtr<SamplerObject>` at `61` (formally named by the D-F3 paragraph at `302-320`),
   pre-existing at `37da3c3a`, owned by P3b/P4b).
9. **Carried to other packages, unchanged from the review**: MAJOR-4/A9 → **D**; ID-12's DV-5 reader
   reassignment → **integrator**; G9-as-white-box → **F / ROADMAP** (it cannot be written against
   public GL; the readback emulation sets `GL_DEPTH_STENCIL_TEXTURE_MODE` itself and
   `IsDrawSyncClean` pushes the parameters on the first sample).
10. **Track H census** (unchanged and re-verified): 7 memos re-keyed (3 direct, 4 transitive),
    0 retired, 1 bypassed (`UnitSamplerLookupMemo`), 1 new (`g_readFboTextureSyncList`).

---

## 7. Deviations of this round

* **DEV-14 — the image seam is loud only at the validate point.** MAJOR-3 as written asks for an
  unconditional `MGLOG_E_ONCE`; that would fire on the eager `glBindImageTexture` funnel, where a
  mismatch is *expected*, and would make the mandated grep useless. Implemented as a latch set around
  the sweep. Reason and mechanism at `2423-2433`.
* **DEV-15 — P7's verdict is the contract's trip-wire verdict, not an unconditional abort.** §2.
  Flagged for the integrator; one `#if` to change.
* **DEV-16 — MAJOR-2 takes the unbind, not the arm-refusal.** §2. Unbinding keeps the two arms at
  parity; refusing the arm would diverge on a window shape nothing yet pins.
* **DEV-17 — the four local latches gained a bit rather than losing the comment.** MINOR-5 offered
  either; adding ID-15's fourth row makes the temporary latches agree with what D's rework will do,
  which is the whole reason the directions were copied here. No arm under test changes.
* **DEV-18 — the six applier reads were funnelled through one accessor.** Not asked for; it is what
  makes ID-19's table redesign a zero-to-one-line rebase for this file (§4).
* **DEV-13 still stands** and is repeated in §6.0 because it is the one item that can silently
  destroy this branch.

---

## 8. Logs

Deleted per ID-5: `/tmp/p4ae/` (this round's edit scripts, build, ctest and configure logs) and the
`build-nolegacy` directory. `~/w7/p4a-esprytdraw` holds only `build-linux` and `build-push`.
Nothing was written under `~/w7/` except this file. The shared scratchpad `wsl` directory was not
touched (ID-7).
