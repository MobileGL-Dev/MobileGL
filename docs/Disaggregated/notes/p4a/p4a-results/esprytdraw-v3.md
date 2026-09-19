# P4a package E — `esprytdraw`. Verification round, v3

**Branch** `refs/heads/p4a/esprytdraw`, worktree `~/w7/p4a-esprytdraw`, **HEAD `0cd0d1ec`**,
rebased onto `refs/heads/feat/disaggregated` = **`ee31944b`**. One file,
`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`. Nothing was pushed.

This is the round that runs on the first tree where every record this package reads is REAL
(contract c0..c0e, wire v3's applier, clientsp v3, clientfb v2, esprytobj v2). It started on
`17396216`; the integrator moved the pipe to `ee31944b` mid-round (`29d51ab9`, which closes
SD-2 below), so **every number in this report was re-taken on `ee31944b`** and every red one has
a control run of `~/w7/pipe` at the same commit, on the same box, in the same session.

**The verdict, in three lines.**

1. **The arms engage and the flips are taken.** Every decline site that can now speak, does; the
   eight-family refusal census is **0 on seven of the ten shapes** (§6.1), and the three
   residuals are seam defects against B/C/D with named evidence and patches.
2. **On both itest lanes, E changes nothing.** Default arm 483/491 with E and 483/491 without,
   **failing sets byte-identical**; `0x7ff` 438/491 both; DirectVulkan **475/475** both;
   `integration-verify` 842/844 both, **zero `Fatal{`**.
3. **On the retrace, E found one real regression of its own - in `e3`, from v2, not from this
   round's flips - and this round root-caused it, patched it and verified the patch.**
   `create-indirect.DirectGLES` was `FAIL ssim=0.887403` against the control's `PASS 0.999961`;
   the cause is a **client-side dirty shutter that cannot see an image re-bind**
   (`Tracker.h:518`), the four-line fix is in §SD-0, and with it applied E's retrace is
   **62/79 with a failing set identical to the control's**. The patch is in another package's
   file and is therefore reported, not committed.

Two findings are larger than this package and are handed up: the tree's own **62/79** at the
default retrace mask, seventeen shader-pack traces wrong with **no refusal behind them** (§7),
and the sampler-record residual that costs two scenarios a correct picture (§SD-1).

---

## 1. The rebase, and the commit series

`git rebase refs/heads/feat/disaggregated` replayed all ten v2 commits **clean** - no conflict,
one file - both onto `17396216` and again onto `ee31944b`. D's replay touched none of the lines
this package touches. Four new commits on top:

| commit | what |
|---|---|
| `ea3d664c` | **A4 + the ID-19 accessor.** The four temporary family latches (`EsprytDraw*HandlesEnabled`) deleted; all 12 consults now call D's public wrappers `FramebufferSubsystemEnabled()` / `TextureResourceSubsystemEnabled()` / `SamplerSubsystemEnabled()` / `ProgramSubsystemEnabled()`. `BoundFramebufferRecord` takes wire v3's landed shape. |
| `90f0c35a` | **A1 + ID-27 + F2/F4/F5/F7.** The framebuffer twin resolved by handle; the `Both` test replaced by the applier's bound handles; the undescribed-binding declines made loud. |
| `3d6fcc4c` | **The remaining flips** - T2, I2, S2, P2, P3, P4, P6 - and the M/K sites' comments updated to record that the round left them silent deliberately. |
| `0cd0d1ec` | **The one flip the first real-path run corrected**: T2 and S2 scoped to a draw that actually touches a texture unit. |

The final diff against pipe is `1371 +/38 -`, one file.

### The `git lfs checkout` trap, recorded because it cost two runs

The task said "`git lfs checkout` first". On this box that is **wrong and destructive**: LFS
content is not local (`Skipped checkout … content not local`), so the checkout **replaced every
real trace fixture with its 130-byte pointer** and the first retrace came back 1/79 with
`0s ssim=None` on 78 of them. The working recipe is `wsl_tree.sh`'s: copy from
`~/w7/pipe/tools/trace_replay/fixtures/` and re-apply `git update-index --assume-unchanged`.
**And `git reset --hard` undoes it again** - `--assume-unchanged` does not survive one - which
is how it happened a second time while the commit series was being rebuilt. Anyone re-running a
retrace on a package tree must re-copy the fixtures immediately before it.

---

## 2. ID-27 — the `Both` question is now asked of the applier's own fact

`DirectGLES.cpp:2983` (v2) tested `record.Target == MGPipeFramebufferTarget::Both` to decide
whether one framebuffer is bound to both bindings and the DRAW pass's attachment work need not
be repeated. Since ID-19(c) the stored `Target` is the **last emission's** target, and B v2
emits `Named` records at sixteen DSA sites, so:

* a framebuffer genuinely bound to both bindings can carry `Target == Named`, and
* a framebuffer bound to neither can carry a stale `Target == Both`.

Replaced by `BoundFramebuffer[Draw] == BoundFramebuffer[Read]` - the applier's own fact, a
`{slot, gen}` comparison, with both handles already proved non-null by F2 three lines up. The
old test degraded to a *redundant* read-buffer sync rather than to a wrong picture (the
`!= Both` path takes the full `SyncToBackend`, which is correct and merely repeats work), which
is exactly why no lane caught it and the wire review had to read the emitter. `grep 'record.Target'
DirectGLES.cpp` is now **empty**, and so is `grep '.Target =='`.

---

## 3. The seven assumptions, reconciled against D's and wire's ACTUAL landed code

| # | what the integrated tree actually says | what E did |
|---|---|---|
| **A1** | `BackendPtr* GetOrCreateByHandle(MGPipeHandle)` at `Managers.h:453` returns a **pointer** and declines three ways (legacy arm, slot past the sanity bound, generation behind the live entry's), silently, "for its release-build voice at the per-kind resolver". | The twin is now resolved **by handle only** - the `FindByHandle`-else-`GetOrCreate(currentFBO)` pair is gone, so no twin on this arm can be minted against the bound frontend *address*. The null return is a **loud whole-arm decline** naming the handle and `LiveGenAt(slot)`. |
| **A1, two corrections to the review's sketch** | `slot.GetBoundObject()` **stays**: A2's `#else` never fired, so `SyncToBackend(SharedPtr<FramebufferObject>, target)` still takes the frontend object as its argument. `FramebufferRecordMatchesBinding` (F3) **stays**: §2's "keep verbatim; it is the wording §8.2 greps" governs, and with the twin now resolved from the record and configured from the bound object, that identity check is the only thing between a mis-keyed record and one framebuffer's attachments written into another's twin. | recorded at the site |
| **A2** | Confirmed unchanged: `Managers.h:1769`/`:1774` still take `const SharedPtr<FramebufferObject>&`. | nothing |
| **A3** | **The feared no-op does not exist, and this is settled by code rather than by a counter.** D added `m_syncedResourceSerial` / `m_syncedParamsSerial` **beside** the legacy members and re-keyed the *sync functions*; `IsDrawSyncClean` (`Managers.h:1565-1583`) still compares the FRONTEND's `GetTextureParamsVersion()` twice, `GetContentVersion()`, and `samplerObject->GetVersion()`, and returns **false outright when the texture has no sampler object at all**. Nothing on the read path advances any of those, so a read-only attachment's first sync is still a miss and `g_readFboTextureSyncList` still pushes. | nothing; D-E3's closure stays where it is |
| **A4** | D's four wrappers exist with the predicted names at `Managers.h:650-665`, holding the `static const` latch; the resolvers carry the refusals, **including D-K2's fourth row** (`ResolveTextureResourceSubsystemArm`, bit 10 requires bit 11, with both mirror pairs said out loud). | E's four latches **deleted**; no dependency direction is restated in this file any more. |
| **A5** | Confirmed. | nothing |
| **A6** | `MGPImageView::Access` now HAS an encoding - `MGPipeEncodeImageAccess` in C's `ImageEmit.h` - but nothing in the contract documents it as read-back-able, and `e3` still does not read it. | unchanged; still a C documentation item |
| **A7** | Confirmed. | nothing |
| **A8** *(the one that had to be measured)* | **Image half: MAJOR-1's union is PERMANENT, and C's own source says so.** `ImageEmit.h:63-80` derives the window from "the highest image unit the CURRENT PROGRAM names … unioned with a sticky mark of every unit this emitter has already described" - explicitly *not* every unit the driver holds. The five I2 hits in §6 are that gap, observed. **Sampler half: C's window is `[0, GetMaxTouchedTextureUnit()]`** (`SamplerEmit.h:822-825`), the same bound E's walk uses, so MAJOR-2's unbind is *expected* unreachable - but it is capped at `kMGPipeMaxTextureUnits` and gated by a content-hash suppressor, so it may only become an assertion once **C documents the membership in `SamplerEmit.h`**. Until then both stay load-bearing. |
| **A9** | D landed `InvalidateFramebufferHandleArmMemos` inside `InvalidateFramebufferBindingCache`. | closed |

**The ID-19 accessor question, answered.** Wire v3 landed `DrawFramebuffer()` / `ReadFramebuffer()`
as member **functions** returning a **nullable pointer** - i.e. *both* halves of v2's DEV-18
prediction. Because every read in this file goes through `BoundFramebufferRecord`, the rebase
was that one function plus a null check at each of its four call sites, and nothing else moved.
---

## 4. The flip table, as executed

`grep -c 'P4a decline-site' DirectGLES.cpp` = **30** (29 rows, F8 twice), unchanged.

| id | class | v2 state | **flip taken this round** |
|---|---|---|---|
| F1 | M | silent | none; **is** `FramebufferSubsystemEnabled()` now |
| F2 | S | half-described loud, both-null silent | **both-null now LOUD** - names both bound handles and `StaleFramebufferRecordLookups`, so "no record was written" and "the slot's generation moved" are told apart without a third message |
| F3 | S | loud | **kept verbatim** (the stem §8.2 greps); only the dereference changed. Deliberately kept against the review's A1 sketch - see §3 |
| F4 | S | loud, `continue` | **`g_fboRecordsTrusted = false; return false`** + the record's handle in the message |
| F5 | — | silent | **`MOBILEGL_ASSERT` + a kept null check** (not deleted: a release build must not deref null if the ordering ever changes) |
| F6 | S | loud | kept - parity with the pre-handle arm |
| F7 | — | silent | same treatment as F5 |
| F8 ×2 | K | silent | none; gained wire v3's null check, which is not a decline |
| F9 | — | fixed in v2 | none |
| **NEW** | S | — | **A1's third decline**: `GetOrCreateByHandle` returning null is a loud whole-arm decline naming the handle and `LiveGenAt(slot)` |
| T1 | M | silent | none; **is** `SamplerSubsystemEnabled()` now |
| T2 | S | silent | **loud-once, then decline — scoped to `maxTouchedUnit >= 0`** (see §6.1a) |
| I1 | M | silent | none |
| I2 | S | silent | **loud-once at the validate point when the unit holds an image texture** - a strictly narrower and more informative condition than the review's "when the program declares images", and the flip that found SD-4 |
| I3 | M | silent | none, and **deliberately not folded into I2**: A8 says nothing pins the window's membership, so a unit *outside* it is not evidence the way an *empty* set is |
| I4 | — | silent | kept as defence; **not** an assertion (that bound is the applier's, not this file's latch) |
| I5 | S | loud | none |
| I6 | M | silent | none - I2 speaks once from inside the sweep instead, where the unit can be named |
| I7 | — | fixed in v2 | none |
| S1 | M | silent | none |
| S2 | S | silent | **loud-once, then the frontend walk — scoped to `maxTouchedUnit >= 0`** |
| S3 | — | fixed in v2 | none |
| S4 | M | silent | none |
| P1 | M | silent | none; **is** `ProgramSubsystemEnabled()` now |
| P2 | S | silent | **loud-once, telling the two bands apart** - an out-of-range ordinary slot says "no `create_shader_state` arrived" (client seam), an out-of-range composite index says the band base or the decode is wrong (contract bug) |
| P3 | S | silent | **loud-once**, naming *dead* vs *live-but-differently-generationed* - ID-8's stale-generation refusal |
| P4 | S | silent | **folded into P2/P3**: stays silent because both causes have just spoken with the band and the generation named |
| P5 | S | loud | none |
| P6 | M→S | silent | **loud-once**, and the review's condition is not retested here because the **only caller already guarantees it** (`GetUBOSize() > 0 && backendProgram.HasGlobalUboBlock()`, the global-UBO upload) |
| P7 | S | loud (Fatal-shaped) | verdict confirmed - **0 `Fatal{ProtocolCorruption}` on the verify lane** |

### 4.1 Every arm engages

The default-arm run is not merely green, it is green *through the new arms*: with F2, F4, T2,
S2, I2, P2, P3, P6 all wired to speak on a decline, the census in §6 shows **zero** hits for
every framebuffer, program and sampler-window site over 491 scenarios. A declining arm cannot
be silent any more, so "no message" now means "the arm ran".

---

## 5. The lanes — every number

All on `~/w7/p4a-esprytdraw` at **`0cd0d1ec`** on **`ee31944b`**, `CCACHE_BASEDIR=/home/swung/w7`,
`-j 8`, `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. The box was shared (load ~45 on 28 cores)
with the gate run and package D's round throughout. Every red number below has a **control run
on `~/w7/pipe` at the same commit, on the same box, in the same session**, and every one of
them matches.

```
build-linux (pull)      rc 0      build-push  rc 0      build-verify rc 0
build-nolegacy (MOBILEGL_PIPE_LEGACY_MEMOS=OFF, PUSH=ON)   configure rc 0   build rc 0
   0 warnings in DirectGLES.cpp on any of the four

G1  symbol_report.py --threshold 0, before ~/w7/p4a-before-libMobileGL.so
    .text 10806323 -> 10806323 (+0, +0.000%)
    27811 -> 27811 defined symbols: 0 added, 0 removed, 0 RESIZED, 0 renamed
G2  pull-vs-push ctest name diff                      0 lines
G14 vs the P4a before-set                             0 removed, +141 added, 2729 total
G5  scripts/p3a_untouched_regions.sh 37da3c3a HEAD    rc 0 (P3a's eleven byte-identical)
G5  ~/w7/p4a-gates/scripts/p4a_untouched_regions.sh   rc 0 (the seventeen)
      DepthStencilSamplingReadImpl  822ca3b0ece4e662f9b462945f2697f6b14547d841d926ae94c2b35139f16e53
      ShouldUseCaveatTextureFormat  266710e0fdc19756f31e27dcee536149c99fc9fe583752042135b3a9cde26366
      == the two shas D's report prints; these are the two D-N rows that live in E's file
    RenderStateImpl sha  d8fd1c48716056c53675 == ~/w7/p4a-before-syncrenderstate.sha
    pGLContext in MG_Backend  MG_Backend/MGPipe/PipeInputs.h:1  == pipe's, unchanged
    MGPipeUnmigratedEmulation in DirectGLES.cpp  4
    record.Target / .Target == tests in DirectGLES.cpp   0   (ID-27)

ctest -L unit          build-linux 1763/1763   build-push 1763/1763   build-verify 1763/1763
emitter/CSO suites (build-push)                168/168
```

### 5.1 `-R DirectGLES -L integration-gpu`, six arms, with the control beside each

| mask | E (`0cd0d1ec`) | pipe without E (`ee31944b`) | verdict |
|---|---|---|---|
| **default `0x1fff`** | **483/491**, 8 failed | **483/491**, 8 failed | **failing sets byte-identical** |
| `0x1ff` | 485/491, 6 failed | — | the six armless `.Handles` aborts |
| `0` | 485/491, 6 failed | — | same six |
| `0x3ff` | 485/491, 6 failed | — | same six |
| `0x7ff` | 438/491, **53 failed** | **438/491, 53 failed** | **identical** — SD-5, a D-side half-run |
| `0xfff` | 483/491, 8 failed | — | same eight as the default |
| C.4 family regex (default) | **495/497**, 2 failed | — | the 2 are SD-1's pair |
| **`-R DirectVulkan`** | **475/475** | **475/475** | **unchanged, and clean** |

The eight default-arm failures are **six + two**:

* six `DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.*` **(Subprocess aborted)** —
  category (a), F's, expected until F lands. **Positively identified this round rather than
  assumed**: their logs carry exactly one line, `Fatal{PipeLegacyMemosDisabled, "MOBILEGL_PIPE_PUSH
  leaves kMGPipeSubsystemSamplers (bit 11) clear (or refuses it) and MOBILEGL_PIPE_LEGACY_MEMOS
  …"}` — the itest's 0x1ff pin meeting D's armless stop.
* `IntegerBorderColorScenario.SamplerParameterIivBorderColourSurvivesToAnIntegerSampler` and
  `SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenASamplerObjectCompletesTheTexture`
  — category (b): **a NAMED REFUSAL**, seam defect **SD-1**, §6.2.

### 5.2 `-L integration-verify` (build-verify, `tcache_count=0`)

```
842/844 passed, 2 failed, 0 Timeout, 0 Subprocess aborted, 0 Not Run
Fatal{  lines: 0
the 2 = DirectGLES.Verify.{IntegerBorderColor…, SampledSetStaleness…}  = SD-1, again
```

Zero `Fatal{` is P7's verdict confirmed and the exit-order lane clean on both backends.
---

## 6. The census, and the seam defects

### 6.0 The census could not be taken the obvious way, and the first attempt was a false ZERO

`MOBILEGL_LOG_ENABLE_CONSOLE` is **0** (`MobileGL/Defines.h:71`), so the library's log has
exactly one sink: the file named by `MOBILEGL_LOG_FILE_PATH`. Grepping `ctest -V` output -
which *does* capture every test's stdout - therefore returns **zero of everything, always**, and
looks exactly like a clean census. Setting one `MOBILEGL_LOG_FILE_PATH` for the whole run does
not work either: `Log.cpp:71` opens it with `"w"`, so 491 processes truncate each other and only
the last survives.

**The recipe that works, and that the integrator and package D should both use.** Move
`build-push/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest` aside to `….real` and put a
four-line shell shim in its place that gives each process its own log file, **named after its
`--gtest_filter`** so a residual can be traced back to its scenario, and that leaves an
already-set `MOBILEGL_LOG_FILE_PATH` alone so the four scenarios that manage their own
(`PoisonOmission`, `PointSizeDemotion`, `StorageBufferRegrow`, `PrimitivesGeneratedNoXfb`) are
unaffected.

**One trap inside the trap:** any `cmake --build` after installing the shim RE-LINKS over it,
after which a naive re-install sees `….real` already present and shims the *stale* binary. Delete
both and rebuild. One census run was thrown away to this.

**Positive control, so the zeros below mean something**: the same harness at
`MOBILEGL_PIPE_PUSH=0x9ff` on one scenario produces
`ERROR]: MGPipe: kMGPipeSubsystemSamplers (bit 11) is set but kMGPipeSubsystemTextureResources
(bit 10) is clear …` - D's dependency refusal, captured.

### 6.1 The census — all TEN shapes, not eight

ID-34's N-2 corrected the census from six shapes to eight. **It is still short by two, and by
its own method**: `grep 'applier record' Managers.cpp` cannot see `:7584` and `:7895`, because
there the phrase straddles a string-literal line break (`"… has no applier "` / `"record …"`).
Both are the **built-in sampler CSO** family, and one of them fires on this tree.

`-R DirectGLES -L integration-gpu`, 491 scenarios, 477 processes that logged:

| # | shape (`Managers.cpp`) | **default `0x1fff`** | `0x1ff` |
|---|---|---|---|
| 1 | `:6310` texture … storage and uploads cannot be driven | **0** | 0 |
| 2 | `:7564` texture … built-in sampler cannot be pushed | **0** | 0 |
| 3 | `:7778` texture … parameters cannot be pushed | **0** | 0 |
| 4 | `:8866` framebuffer … its read buffer cannot be pushed | **0** | 0 |
| 5 | `:9083` framebuffer … cannot be synced from the pushed record | **0** | 0 |
| 6 | `:11925` program … no shader-CSO applier record | **0** | 0 |
| 7 | `:12174` sampler … its parameters cannot be pushed | **2** → **SD-1** | 0 |
| 8 | `:12418` renderbuffer … storage cannot be allocated | **0** | 0 |
| **9** | `:7584` built-in sampler CSO … declining the built-in sampler push | **3** → **SD-3** | 0 |
| **10** | `:7895` built-in sampler CSO … border colour cannot be pushed | **0** | 0 |
| — | E's three seam lines (`does not describe the binding it names`) | **5** → **SD-4** | 0 |
| — | `Fatal{ProtocolCorruption}` | **0** | 0 |
| — | `Fatal{PipeLegacyMemosDisabled}` | 6 (= the six armless aborts) | 6 |

`0x1ff` is zero for every row by construction (all four E families take their M sites), and is
recorded because it is the arm the six aborts are pinned to.

### 6.1a The flip the run corrected — T2 and S2, 333 false positives

Before `0cd0d1ec`, T2 fired in **172** of 477 processes and S2 in **161**, and every single S2
line said `units 0..-1`. `maxTouchedUnit < 0` is "this draw touches no texture unit at all", for
which an empty sampler-view / sampler-state window is not a seam defect but **the only correct
emission**. Both are now scoped to `maxTouchedUnit >= 0` and both are **0**. This is the one
place where the review's table, written before C landed, asked for a rule the real path
disproved; the site comments say so and say how it was measured. The MINOR-4 gate tick still
counts *every* decline, loud or not, so nothing became uncountable.

---

## SD-0 — **E's own regression, found and root-caused: `create-indirect` on DirectGLES**

This is the one defect the round found *in this package's own consequences*, and the only
difference between E's retrace and the control's.

```
retrace, default mask, 79 cases
  ~/w7/pipe   at ee31944b, WITHOUT E    62 / 79
  ~/w7/p4a-esprytdraw at 0cd0d1ec       61 / 79
  the single difference:
    MobileGLTraceReplay.minecraft-1.21.1-neoforge-create-indirect-in-world.DirectGLES
      E    FAIL ssim=0.887403      pipe  PASS ssim=0.999961     (deterministic, 4/4 runs each)
```

**Bisected to the commit**, by rebuilding at six points across E's fourteen commits (the pre-wire-v3 ones
carry a mechanical accessor fix so they compile; it is behaviour-preserving - a null answer
becomes a zeroed record, which is what a null `Fbo` already was):

```
ee31944b  base                                                   PASS 0.999961
78967961  e1  framebuffer records                                PASS 0.999961
6c63e017  e2  texture sync lists                                 PASS 0.999961
1e1a042f  e3  SHADER IMAGES FROM THE PUSHED SET                  FAIL 0.887403   <-- first bad
9aca1457  e4                                                     FAIL 0.887403
15ab31dc  … through 0cd0d1ec                                     FAIL 0.887403
```

**Bisected to the field**, with each of `e3`'s four record-sourced fields put behind its own
switch:

```
drop record->Layered  FAIL      drop record->Level   PASS  <-- this one
drop record->Layer    FAIL      drop record->InternalFormat  FAIL
```

**Root cause, and it is NOT in `DirectGLES.cpp`.** `MGPipeDirty::NewShaderImages`'s shutter is
`Mix(Mix(textureContent, textureParams), programImages)` (`Tracker.h:518-519`), and
`ImageEmit.h:20-25` states that as an invariant: *"all three FRONTEND counters"*. **None of the
three moves when `glBindImageTexture` re-binds a texture that is already on that unit and
changes only `level`** (or `layer`, `layered`, `format`, `access`). So `wants(NewShaderImages)`
is false, `EmitShaderImages` never runs, the applier keeps the PREVIOUS bind's `Level`, and
`e3` - the first code anywhere to read that field off the record - paints the wrong mip.
The set-hash suppressor cannot save it: the emitter is never called, so no hash is ever taken.
Flywheel/Create does exactly this, which is why one trace in 79 finds it.

**Ruled out along the way** (each rebuilt and re-run): trusting the record at the eager
`glBindImageTexture` funnel rather than only at the validate point (`FAIL` either way, so
DEV-14's latch is not implicated); ID-27's bound-handle comparison (`FAIL` with the old
`record.Target == Both` restored); A1's `GetOrCreateByHandle` (`FAIL` with `FindByHandle` +
`GetOrCreate(currentFBO)` restored).

**The patch, written and VERIFIED, for the owner of `MG_Impl/Pipe/Tracker.h`** (C / the
contract - not E's file under C.7, so it is *not* committed on this branch):

```c++
// Tracker.h, in Update(), replacing lines 518-519
now[Index(MGPipeDirty::NewShaderImages)] = MGPipeMixShutter(
    MGPipeMixShutter(MGPipeMixShutter(textureContent, textureParams), programImages),
    ctx.GetTextureBindGeneration());
```

`glBindImageTexture` already calls `NoteTextureUnitTouched` (`GL_Texture.cpp:6741`), so that
generation moves on every image bind and nothing new has to be counted - which matters, because
`ImageEmit.h:69` records that adding a counter to `TextureState` would resize the pull build's
object and G1 forbids it. It is also the shape `NewSamplers` already uses two lines above, with
Tracker.h's own argument for it: *"MIXING THE GENERATION IN IS THE FIX RATHER THAN A SECOND GATE
ON THE EMITTER"*. Measured with the patch applied to this tree:

```
create-indirect.DirectGLES     PASS ssim=0.999961   (was FAIL 0.887403)
DirectGLES integration-gpu     unchanged
DirectVulkan integration-gpu   unchanged
full retrace                   see §7
```

`ImageEmit.h`'s invariant-2 paragraph must be reworded in the same commit: it currently says the
shutter is three frontend counters *and gives that as the reason the sweep's gate is sound*. It
becomes four, and the added one is still a frontend counter, so the property it protects
survives - but the sentence as written is now the documentation of a bug.

---

## SD-1 (category (b)) — the sampler family refuses, and it is the tree's only wrong picture on the DirectGLES itest lanes

**Refusal**, twice, one per scenario:

```
ERROR]: MGPipe: sampler 1 has no applier record on the handle arm, so its parameters
                cannot be pushed (handle {8, 0})   IntegerBorderColorScenario…IntegerSampler
                                (handle {6, 0})   SampledSetStalenessScenario…CompletesTheTexture
```

**Reading side**: `Managers.cpp:12174`, `BackendSamplerObject::SyncToBackend` - D's file. It
resolves the sampler CSO record for the handle and declines when there is none; it does **not**
fall back to the frontend, by design.

**Emitting side**: package **C** (`clientsp`, `SamplerEmit.h`). No `create_sampler_state` has
been applied for the sampler object the draw binds. The generation in both handles is **0**
while the slots differ (6 and 8): the handle was *acquired* (the slot exists) but never
*described*.

**The failure is visible, not silent**, which is what makes it the most important residual:
`IntegerBorderColorScenario.SamplerParameterIivBorderColourSurvivesToAnIntegerSampler` reads
back `component 0 = 0 instead of 255`, `1 = 0 instead of -1`, `2 = 0 instead of 7` and
`3 = 1065353216 instead of 3` on **64 of 64 texels**. `1065353216` is `0x3F800000` - **`1.0f`
reinterpreted as an integer** - so the integer border colour was not merely lost, the FLOAT form
was applied in its place. That is `SamplerParameters::borderColorForm` (the field F's G7
negative control targets) not reaching the driver because the record it rides on was never
created.

**Not E's**: identical on `~/w7/pipe` without this package, on both the gpu and the verify lane.

**Patch, for package C.** The refusal is at the *first* `SyncToBackend` of a sampler object
whose CSO record was never emitted, so the fix is birth-side, not per-draw: every path that
acquires a `SamplerCso` slot for a `SamplerObject` must emit `create_sampler_state` before any
handle naming it can reach the applier - the "mint-and-create in one step" shape
`TextureEmit.h:943-949` already has. Two things to check first, in this order:
1. `glSamplerParameterIiv` / `glSamplerParameterIuiv` are the entry points both failing
   scenarios use. If `SamplerEmit.h`'s parameter hooks cover the `f`/`i` forms but not the
   integer `Iiv`/`Iuiv` forms, the object is never marked dirty and never emitted - which fits
   `Gen == 0` exactly.
2. `SampledSetStalenessScenario` binds a sampler object to *complete* an incomplete texture,
   i.e. the sampler is created and bound with **no parameter call at all**. If the emission
   hangs off a parameter hook rather than off the object's birth, that case can never emit.

Both point at one rule: **emit on acquire, not on first parameter.**

---

## SD-2 (category (c)) — bit 10 cost the DirectVulkan lane 66 tests, and it was fixed under this round

Recorded because this round found it independently and its bisect is worth keeping.

On `17396216` (the tree this round started on), on `~/w7/pipe`'s own `build-push`, **without E**:

```
MOBILEGL_PIPE_PUSH=0     475/475   =0x3ff  475/475   =0x1000 475/475
                 =0x1ff  475/475   =0x7ff  409/475   =0x11ff 475/475
                 =0x1fff 409/475   =0xfff  409/475
```

Exactly **bit 10** (`kMGPipeSubsystemTextureResources`), deterministic
(`DirectVulkan.EmptyScissorScenario.AnExplicitlyEmptyScissorBoxClipsEveryFragment` reproduces
serially and single-process), and identical with and without E. The integrator's `29d51ab9`
("gate the four P4a families on a backend having registered `MGPipeResourceOps` - Magma emitted,
the applier accepted, and the acceptance-cleared dirty flags left its legacy path nothing to
upload") landed while this round was running and **closes it**: on `ee31944b` the lane is
**475/475 with and without E**.

Two things from this round's own investigation are worth keeping in the record, because they
would have sent the next person down the same two dead ends:
* a **negative control that refuted the obvious single-line culprit**: putting only
  `TextureEmit.h`'s `mipmap->MarkStorageDirty(uploadTarget, level, false)` (the sub-data
  acceptance clear) behind a switch left the lane at **66 failed either way**. The clear that
  mattered was on a different path, which is why `29d51ab9` had to gate the whole family
  rather than one call;
* the reading side, for the record: `VkTextureManager::UploadDirtyMipLevels`
  (`VkTextureManager.cpp:2873`, and `:1828`) skips any level whose `IsStorageDirty` is false,
  and no `MG_Backend/DirectVulkan` file reads `MGPipeApplier().TextureResources` or
  `PendingUploads` - so Magma could only ever lose by the emission, never gain by it.

---

## SD-3 (category (b)) — the built-in sampler CSO family refuses, in the two shapes the census cannot see

```
ERROR]: MGPipe: texture N's built-in sampler CSO {slot, gen} has no applier record
                - declining the built-in sampler push                            x3
  DepthStencilReadbackMatrixScenario.ReadbackLeavesNoGLStateBehind
  UnboundImageDescriptorScenario.ABufferTextureWithNoAttachedBufferDoesNotLoseTheDraw
```

Both scenarios **pass**, so this is a silent fallback rather than a wrong picture - which is
exactly why it has to be reported: the record for a texture's `MGPTextureParams::BuiltinSampler`
handle was never applied, and D's twin declines the push instead of using it. Same owner and
almost certainly the same root cause as SD-1 (C never emitted `create_sampler_state` for a
sampler CSO whose handle it had already acquired); the difference is only which side holds the
handle - here the texture record's built-in sampler rather than a bound `SamplerObject`.

**Owed by D's report (documentation) and C (code).** D: add `:7584` and `:7895` to the census in
`esprytobj-v3.md` §4/§5 and say that the stem `no applier record on the handle arm` matches only
**seven of ten**. The integrator's residual check must grep **four** patterns, not two:
`no applier record on the handle arm`, `no shader-CSO applier record`,
`declining the built-in sampler push`, `border colour cannot be pushed`.

---

## SD-4 (category (b), found by E's own I2 flip) — `set_shader_images` is never emitted for a buffer-texture image

```
ERROR]: An image record does not describe the binding it names: image unit 0 holds a
        texture at a draw and no set_shader_images has ever been applied; running the
        pre-handle image bind.                                                        x5
  BufferTextureScenario.AnImageStoreIntoABufferTextureIsVisibleToTheCpu
  ImageTargetKindScenario.LoadsTextureBuffer
  ImageTargetKindScenario.StoresTextureBuffer
  NonCoreImageFormatScenario.ASplitBufferImageStillSamplesWholeTexels
  NonCoreImageFormatScenario.BufferImageAddressesTheApplicationsOwnTexels
```

All five pass, because E declines to the pre-handle bind - the fallback MAJOR-1's union exists
to protect. **The discriminator is the buffer flavour and nothing else**:
`ImageTargetKindScenario` runs one shader template over `uimage1D`, `uimage1DArray`,
`uimage2D`, … and `uimageBuffer`, and **only the `Buffer` kind appears here**.

**Emitting side**: `ImageEmit.h:81-90`. The window is
`resolution.MaxImageUnit < 0 ? 0 : MaxImageUnit + 1`, and `count == 0` takes the zero early-out
before any hash - so **no `set_shader_images` is ever applied for the life of the process**.
`MaxImageUnit` comes from `SamplerEmit.h:635-642`, which counts a uniform location only when
`GetUniformSamplerOrImageUnitIndex(location) >= 0 && GetUniformTypeFacts(location).isImage`.
For an `imageBuffer` uniform one of those two answers wrongly; the unit index is the likelier -
ES forbids `glUniform1i` on an image uniform, so it can only come from `layout(binding=)` at
link, and `ProgramLinkTask.cpp:57` sets `isImage` straight from glslang's `type->isImage()`.

**Reading side**: `DirectGLES.cpp`'s I2, the flip this round installed, which is the only reason
this is visible at all.

**Patch, for package C**, in preference order:
1. Fix the reflection so an `imageBuffer` / `uimageBuffer` / `iimageBuffer` uniform contributes
   its unit to `MaxImageUnit`. If the gap is in `uniformSamplerOrImageUnitIndex` rather than in
   `isImage`, that is `MG_State`'s file and outside P4a's set - the integrator has to grant it,
   and it is then a **pre-existing frontend reflection gap that P4a's client emitter is the
   first code to depend on**, worth saying in the phase's docs.
2. Failing that, union the emitted window with the units the CONTEXT actually holds an image
   binding on - the same union E's sweep already takes server-side (MAJOR-1), which would make
   the two sides agree by construction.

Until one of them lands, **A8's image half is settled: MAJOR-1's union in
`SyncImageTextureBindings` is permanent and may not become an assertion.**

---

## SD-5 (category (c), pre-existing) — `0x7ff` half-runs the framebuffer family against a refused texture family

53 DirectGLES scenarios fail at `MOBILEGL_PIPE_PUSH=0x7ff`, **identically with and without E**,
where every other arm this round ran is 6 or 8. The shape is D's:
`ResolveTextureResourceSubsystemArm` refuses bit 10 because bit 11 is clear (D-K2's fourth row,
correctly), so the texture family runs the LEGACY arm - but
`ResolveFramebufferSubsystemArm` tests **the raw mask bit 10**, not the texture family's
resolved verdict, so the framebuffer family ARMS. That is precisely the half-run D-K2 exists to
forbid, one level up: the dependency is expressed against the BIT and the bit lies whenever the
depended-on family refused itself.

**Patch, for package D** (`Managers.cpp`, `ResolveFramebufferSubsystemArm`): make the row read
the arm rather than the bit -

```c++
// bit 9 requires the TEXTURE-RESOURCE ARM, not merely bit 10: bit 10 can be set and still
// refused (bit 10 requires bit 11), and a refused family runs its legacy arm, which is
// exactly the state MGPSurface::Res cannot be resolved against.
refused = !TextureResourceSubsystemEnabled();   // logs its own reason
```
with the existing `PipeSubsystemDependencyMissing` line kept for the plain bit-10-clear case so
the message still names both bits. `0x7ff` is not a shipped mask, so this is not urgent - but it
is a lane that reports 53 failures to anyone who sweeps the masks, and F's dependency-refusal
scenario is the natural place to pin it.
---

## 7. The retrace, and what it says about the tree rather than about E

`python3 ~/w7/retrace_gate.py --tree ~/w7/p4a-esprytdraw --lib build-push/libMobileGL.so
--out ~/w7/retrace-out/p4a-esprytdraw-v3 -j 4`, default mask, 79 cases.

```
~/w7/pipe   ee31944b, WITHOUT E                     62 / 79   17 failed, all DirectGLES
~/w7/p4a-esprytdraw 0cd0d1ec                        61 / 79   18 failed, all DirectGLES
~/w7/p4a-esprytdraw 0cd0d1ec + SD-0's Tracker patch 62 / 79   17 failed - THE SAME SEVENTEEN,
                                                              set-identical to the control
```

So the whole of E's retrace delta is **one case**, `create-indirect.DirectGLES`, and SD-0's
four-line patch closes it exactly.

**79/79 was not obtainable, and is not obtainable without this package.** The integrated tree is
at 62/79 at the default mask, and the seventeen are identical in name and in SSIM to six decimal
places between the two trees:

```
improved-transparency-minecraft-26.3.DirectGLES              0.994435
minecraft-1.21.11-main-menu.DirectGLES                       0.936674
minecraft-1.21.4-fabric-iris-bliss-in-world.DirectGLES       0.856432
minecraft-1.21.4-fabric-iris-bsl-esc-menu-854.DirectGLES     0.922890
minecraft-1.21.4-fabric-iris-bsl-in-world.DirectGLES         0.851414
minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world       0.855946
minecraft-1.21.4-fabric-iris-complementary-reimagined        0.815194
minecraft-1.21.4-fabric-iris-complementary-unbound           0.806136
minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14        0.761700
minecraft-1.21.4-fabric-iris-iterationt-in-world             0.985983
minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world       0.985049
minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world       0.981368
minecraft-1.21.4-fabric-iris-nostalgia-in-world              0.845284
minecraft-1.21.4-fabric-iris-photon-v1.1-in-world            0.824615
minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world           0.970720
minecraft-1.21.4-fabric-iris-sundial-lite-in-world           0.812143
minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world    0.610444
```

**Fifteen of the seventeen are Iris shader packs**, plus `improved-transparency-26.3` and one
main menu; every DirectVulkan half passes, and every non-Iris in-world and inventory trace
passes. Nobody had run a retrace at the DEFAULT mask on an integrated P4a tree before this
round - B's 79/79 was at `0x1ff`, and D's arms were `189/6/6` on the itest lane only - so **this
is the first measurement of it, and it is an integrator-level finding independent of E**:
seventeen shader-pack traces are wrong on the DirectGLES handle arm, at SSIM 0.61 to 0.99, with
**zero named refusals and zero seam lines** anywhere in the census. That is category (c): a
silent difference in the client emission or the applier, not a declined arm. E's decline sites
cannot speak about it because none of them is declining, and the fact that the failures cluster
so exactly on shader packs (many programs, many framebuffer switches, many image and sampler
units per frame) is the strongest lead the round can hand over.

The SD-0 patch is **reverted** on the branch (`Tracker.h` is not E's file); the working tree is
clean.

---

## 8. The verification-round checklist (v2 §6), item by item

| item | result |
|---|---|
| 6.0 do not run `wsl_tree.sh`; `git lfs checkout` first | **the `git lfs checkout` instruction is WRONG on this box** - see §1. The branch was never reset; `refs/heads/p4a/esprytdraw-backup` untouched |
| 6.1 take §4's reconciliations first | done, in the order given; §3 |
| 6.2 apply the flips, assert every arm engages | done; §4, §4.1 - and one flip was corrected by the run itself (§6.1a) |
| 6.3 zero seam hits, and `Fatal{ProtocolCorruption}` on verify | **5 image-seam hits (SD-4, a finding against C, not a reason to relax the check)**, 0 framebuffer, 0 program, **0 `Fatal{ProtocolCorruption}`** |
| 6.4 A8's measurement | **answered, and it is the image half that matters**: `ImageEmit.h`'s own text says the window is the program's named units unioned with a sticky mark, not the driver's set, and SD-4 is that gap observed. MAJOR-1's union is **permanent**. The sampler half's window is `[0, GetMaxTouchedTextureUnit()]`, the same bound E walks, so MAJOR-2 is *expected* unreachable but stays load-bearing until C documents the membership in `SamplerEmit.h` (it is capped at `kMGPipeMaxTextureUnits` and gated by a suppressor) |
| 6.5 A3 / D-E3: does the read-FBO list still do work? | **yes, and settled by code rather than by a counter**: `IsDrawSyncClean` was NOT re-keyed onto the record's `ParamsSerial` - it still compares the frontend's params version twice, the content version, and the sampler object's version, and **returns false outright when the texture has no sampler object**. Nothing on the read path advances any of them. D-E3's closure stays in E |
| 6.6 unit-list equivalence between `0x1ff` and `0x1fff` | the two arms' *failing sets* differ only by the two SD-1 scenarios, and every family census row is 0 on both, so no rebuild class is missing; the per-frame rebuild COUNT is not instrumented on either arm and stays an optimisation-list item |
| 6.7 named traces | `photon-v1.3b` and `improved-transparency-26.3` both run, both fail **identically with and without E** (§7); the canary that actually fired was `create-indirect`, and it caught a real one (SD-0) |
| 6.8 the full C.4 verification | §5. `p4a_untouched_regions.sh` run from F's tree, rc 0, and the two D-N rows that live in E's file carry D's own shas. `g_rawDepthFetchSamplerState` confirmed still the only file-static `SharedPtr<SamplerObject>` in this file, pre-existing at `37da3c3a`, untouched (P3b/P4b's) |
| 6.9 carried to other packages | A9 closed by D; ID-12's DV-5 still the integrator's; G9-as-white-box landed in D |
| 6.10 Track H census | unchanged: 7 memos re-keyed, 0 retired, 1 bypassed, 1 new |

---

## 9. What the integrator must re-run, and what is owed by whom

**Before the push**

1. **SD-0 → the owner of `MG_Impl/Pipe/Tracker.h` (C / contract).** The four-line shutter patch
   in §SD-0, plus the `ImageEmit.h` invariant-2 rewording. **Verified on this tree**: it turns
   `create-indirect.DirectGLES` from `FAIL 0.887403` into `PASS 0.999961` and leaves both itest
   lanes unchanged. Until it lands, E's tree is one retrace case behind pipe's, and **that case
   is the only difference between them**.
2. **SD-1 → package C.** Two scenarios red on the gpu AND verify lanes, with a named refusal and
   a demonstrable wrong picture (`0x3F800000` where an integer border colour belongs). This is
   the residual ID-12's rule is about.
3. **SD-4 → package C** (possibly reaching into `MG_State`'s reflection under an integrator
   grant). Five scenarios silently on the pre-handle arm.
4. **SD-3 → package D's report and package C's code.** The census is short by two shapes; the
   residual check needs four grep patterns.
5. **SD-5 → package D**, when convenient. `0x7ff` is not a shipped mask.

**Re-runs the integrator owes after any of the above lands**

* the DirectGLES `-L integration-gpu` default arm and the census (the recipe is §6.0 - **the
  obvious `ctest -V` census is a false zero and must not be used**);
* the **full retrace at the default mask**, which nobody had run on an integrated P4a tree
  before this round and which is at **62/79 without E** (§7). Those seventeen are a
  category-(c) finding with no refusal behind them and they are **not this package's**; they
  need an owner before the phase closes.

**Not owed by anyone, recorded as settled**

* the DirectVulkan lane: **475/475, with and without E**, at every mask this round swept, once
  `29d51ab9` landed;
* the six armless `.Handles` aborts: F's, and now positively identified by their
  `Fatal{PipeLegacyMemosDisabled}` line rather than assumed;
* ID-27: `record.Target` has no reader left in this file.

---

## 10. Deviations of this round

* **DEV-19 — F3 was KEPT against the review's A1 sketch.** The A1 cell says the seam check "can
  go" once the twin resolve takes the handle; §2's table says "keep verbatim; it is the wording
  §8.2 greps". The table wins, and the reason is stronger than precedence: with the twin now
  resolved from the record and configured **from the bound frontend object**,
  `FramebufferRecordMatchesBinding` is the only thing between a mis-keyed record and one
  framebuffer's attachments being written into another's twin. Removing it would delete the
  check that makes A1 safe.
* **DEV-20 — F5 and F7 became assertions PLUS a kept null check**, not assertions alone and not
  deletions: `MOBILEGL_ASSERT` is inert in a release build, and a dereference of a nullable
  accessor is not something to leave to an ordering argument.
* **DEV-21 — I2's condition is narrower than the review's.** "When the program declares images"
  is not testable at that site; "the validate-point latch is set AND this unit holds an image
  texture" is, is strictly stronger, and names the unit. The latch was extended to cover the
  pre-handle sweep as well, because I2 fires precisely when the record arm is NOT taken and a
  latch covering only the record arm would make the flip unreachable.
* **DEV-22 — T2 and S2 are scoped to `maxTouchedUnit >= 0`**, against the review's table, on the
  evidence of the first real-path run (§6.1a). 333 of 333 hits of the unnarrowed rule were draws
  that touch no texture unit.
* **DEV-23 — P6 does not re-test the review's condition** because its only caller already
  guarantees it; re-testing would have been a second reader of one rule.
* **DEV-18 (v2) paid for itself**: wire v3 landed the accessors as member functions returning a
  nullable pointer - both halves of what v2 predicted - and the rebase was one function plus
  four null checks.

---

## 11. Logs

Everything this round wrote lives in `~/w7/p4ae3/` and is **deleted** at the end of the round per
ID-5: the edit scripts, the four `build-*.log`s, the arm logs, the per-process `mglogs/`, the
census extracts, the bisect logs and the retrace logs. `~/w7/p4a-esprytdraw` keeps only
`build-linux`, `build-push` and `build-verify` (`build-nolegacy` was deleted with the logs; it
is a compile gate, rebuilt in one command). The shared scratchpad `wsl`
directory was not touched (ID-7). Nothing was pushed; `~/w7/pipe` and the other `p4a-*` trees
were read-only except for the control ctest runs inside `~/w7/pipe/build-push` and
`~/w7/pipe/build-verify`, which write no tracked file.
