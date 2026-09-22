# espryt D1 — the Espryt (DirectGLES) stream, part 1

P3b/P4b items landing under the P7 program (`notes/p7/PLAN-PH-P34B-P7.md` §1.2 rows XFB /
回读 / `g_fboTextureSyncList`, §3 wave 2-D). Base `feat/disaggregated@665cfefc`, branch
`p7/espryt-d1`.

Per-slice red-once notes (R-16). A slice is not done until the case it adds has been seen RED
against the tree without that slice's fix and GREEN with it, on the arm the fix is for.

---

## slice 1 — the orphaned XFB capture target

**The hole.** `ScatterCapturedRecords` (`MG_Backend/DirectGLES/DirectGLES.cpp`) is a
read-modify-write of the capture destination's PRE-CAPTURE bytes: a `gl_SkipComponents` hole
has to come back holding whatever the application left there, so the scatter stages the old
range, patches the captured varyings into it, and uploads the whole range once. Under a
transport the old bytes are the SERVER's staged shadow (`GLESBufferResource::hostBytes`)
rather than a frontend `MappedData()`, and the split arm read a null shadow as *"this target
has no pre-capture bytes, discard it"*:

```
if (splitResource == nullptr || hostBytes == nullptr) { MGLOG_E_ONCE(...); continue; }
```

A null shadow is not an error. `MGPResourceDesc.HasDefinedContent == 0` — the store the
application ORPHANED with `glBufferData(size, NULL)` — declares every byte it has not staged
since to be UNDEFINED, and `Managers.cpp`'s own M-3 comment says so in as many words. So the
`continue` threw away a whole capture for the ordinary streaming idiom (orphan, upload part of
it, capture into it), leaving the application's buffer holding its pre-draw bytes under
`GL_NO_ERROR` — the exact silent-loss class the preamble above `ReadbackCapturedRanges` exists
to close.

The partly-uploaded shape did not even reach the `continue`: it has a shadow, so it went on to
`RequireStagedCoverage` over the WHOLE bound range and died on
`Fatal{StageSnapshotTooNarrow, "xfb_scatter_pre_capture"}`.

**The fix.** Monolith has always been right here, and it is right by construction:
`Vector<Uint8> staged(rangeBytes)` is value-initialised, so scattering over an unreadable
source means scattering over zeroes and uploading them — byte for byte what a fresh
`MappedData()` hands back. So the split arm skips only the `Memcpy`, never the target:

* `splitResource == nullptr` keeps discard-with-log. That one really is impossible (the target
  was recorded against a handle the applier had already minted a twin for) and there is no
  `backendId` to upload into either.
* a null `hostBytes` falls through with `staged` left zeroed.
* `RequireStagedCoverage` is guarded by `BufferImpl::ResourceContentIsDeclared(res)`, exactly
  as `Managers.cpp`'s pool-reuse ladder guards its own whole-store `MGL_SERVER_STAGED_REQUIRE`.
  A declared store still Fatals on a coverage gap (a missing record is still a missing record);
  an orphaned one does not.

`ResourceContentIsDeclared` moved out of `Managers.cpp`'s anonymous namespace to namespace
scope beside `RequireStagedCoverage` and is declared in `Managers.h`: the two callers must not
be able to answer "did the application declare these bytes" differently. Both are inside
`#if MOBILEGL_BUILD_DISAGGREGATED`, so the pull build compiles neither (G1).

**Red-once (R-16).** `XfbAfterClipDistanceScenario.OrphanedCaptureTarget*`, two cases, on the
split arms:

| case | store | without the fix |
|---|---|---|
| `…WithAPartialUploadKeepsItsVaryings` | `glBufferData(size, NULL)` + `glBufferSubData` over the first half | `Fatal{StageSnapshotTooNarrow, "xfb_scatter_pre_capture"}` |
| `…WithNoUploadAtAllKeepsItsVaryings` | `glBufferData(size, NULL)`, nothing staged | silent discard: every captured component is still the pre-fill |

Both assert the captured varyings everywhere and the preserved `gl_SkipComponents` holes only
inside the uploaded region — past it the application declared the bytes undefined and nobody
may check them. The ambient monolith registrations of the same two cases are the control that
says the expectation is right rather than the arm's.

**RESULT — red once, then green.** With the `continue` and the unguarded `RequireStagedCoverage`
restored (fix reverted, everything else identical), `ctest -R 'DirectGLES.*OrphanedCaptureTarget'`
gave 4/10 passed, 6 failed — and the six are exactly the six split-arm entries:

```
3736 DirectGLES.Split.Xfb …WithAPartialUploadKeepsItsVaryings  (Subprocess aborted)
3737 DirectGLES.Split.Xfb …WithNoUploadAtAllKeepsItsVaryings   (Failed)
3738 DirectGLES.Spawn.Xfb …WithAPartialUploadKeepsItsVaryings  (Subprocess aborted)
3739 DirectGLES.Spawn.Xfb …WithNoUploadAtAllKeepsItsVaryings   (Failed)
3740 DirectGLES.Tcp.Xfb   …WithAPartialUploadKeepsItsVaryings  (Subprocess aborted)
3741 DirectGLES.Tcp.Xfb   …WithNoUploadAtAllKeepsItsVaryings   (Failed)
```

The abort is the one the row predicted, verbatim from the lane log:

```
MGPipe: Fatal{StageSnapshotTooNarrow, "xfb_scatter_pre_capture"} - the server's ladder wants
[0, 696) of a buffer whose staged coverage does not include it.
```

and the failure is the silent one:
`captured varying at index 1 (past the uploaded region): got 0, expected 1`. The two ambient
monolith entries (2471 / 2472) PASSED in the same red run, which is what makes the six a
transport fact rather than a wrong expectation.

With the fix: 10/10.

**Gates.** `integration-split` 188/188 (base 186 + these 2), `integration-spawn` 104/104
(base 102), `integration-tcp` 105 labelled + 2 fixtures = 107/107 (base 103 + 2),
`ctest -L unit` 2408/2408, the monolith `DirectGLES.` XFB + readback families 66/66,
`spawn_lane_parity.py build-split` RC 0 (split 105 / 102 comparable, spawn 102),
`fatal_census.py` RC 0 (79 abort sites, 0 unmarked), `link_ratchet.py --assert-monotone`
unchanged at 186 symbols. G1: `.text` `0xa52203`, `nm --defined-only` and `nm --defined-only -D`
both byte-identical to `~/w7/p7-before/`.

---

## slice 2 — the unbounded writeback event

`RESULT: (pending)`

## slice 3 — the dead `OnXfbScatterReady` declaration

`RESULT: (pending)`

## slice 4 — readback close-out

`RESULT: (pending)`

## slice 5 — four transport-aware resolvers

`RESULT: (pending)`
