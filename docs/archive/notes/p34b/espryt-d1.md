# espryt D1 — the Espryt (DirectGLES) stream, part 1

P3b/P4b items landing under the P7 program (`notes/p7/PLAN-PH-P34B-P7.md` §1.2 rows XFB /
回读 / `g_fboTextureSyncList`, §3 wave 2-D). Base `feat/disaggregated@ec3ba00a`, branch
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

**The hole.** Both XFB writeback producers posted ONE unbounded `OnBufferWriteback` covering the
whole bound range: `ScatterCapturedRecords` from its staged vector, `ReadbackCapturedRanges` from
a live `glMapBufferRange` mapping. `ServerOnBufferWriteback` turns each into one SEG_EVENT record
of `sizeof(EventBufferWritebackHead) + bytes`, and `RingProducer::Reserve` refuses any record
above `Capacity()/2` **outright** — not "when full", but always, because above half a capacity a
record is placeable at some head offsets and not at others and waiting for room would be a hang.
`EventRingBytes` defaults to 256 KiB. So any capture target wider than ~128 KiB was
`Fatal{EventRingOverflow}` and the **server process died** — on a buffer size, with nothing else
wrong. The producers' own Fatal text already said what to do: *"Raise MOBILEGL_IPC_EVENT_KB or
slice the event at its producer, as the writeback path already does"* — which was true of the
client's request path and of nothing on this side.

**The fix.** `MG_Pipe` grows the one number, published the way `gMGPipeSegmentResolver` is
published: `gMGPipeEventRingCapacityBytes`, installed by `ServerSession::Accept` beside the four
reverse-channel producers and released at `Close` only if still ours, and
`MGPipeBufferWritebackSliceBytes()` which turns it into a per-record width (a quarter of the ring,
4 KiB floor — the same number and the same reasoning as the client's `BufferWritebackSliceBytes`).
The backend cannot reach `MG_Remote::Client` (wrong linkage) or `ServerSession` (wrong layer), so
the hook is the seam. 0 means "do not slice": monolith has no ring under the callback.

Both producers loop against one helper, `XfbWritebackSliceStep`, so a second spelling of the width
cannot leave one of them still killing the server. `:1782` slices INSIDE its mapping and still
unmaps once; `:1969` slices the staged vector and keeps the single `glBufferSubData` whole,
because the ES store has no ring and cutting it would change how many backend calls one
application call makes. In-order delivery makes the last slice's landing imply every earlier one,
so the client needs nothing new to reassemble them.

**Red-once (R-16).** `XfbAfterClipDistanceScenario.{AScatteredCapture,APlainCapture}IntoAMegabyteRangeReachesTheApplication`
— a 1 MiB bound range (≈8× what one record can ever be) with the same six capture records; it is
`glBindBufferBase` over the whole store that makes the mirrored range a megabyte. Two cases
because there are two producers: the `gl_SkipComponents` layout is not expressible on ES and goes
through the scatter, the plain single-varying layout is and goes through the readback.

With `XfbWritebackSliceStep` returning the whole range (fix reverted), all six split entries went
red and both monolith controls stayed green:

```
3744 DirectGLES.Split.Xfb …AScatteredCapture…  (Subprocess aborted)
3745 DirectGLES.Split.Xfb …APlainCapture…      (Subprocess aborted)
3753/3754 DirectGLES.Spawn.Xfb …              (Failed — the server process died)
3762/3763 DirectGLES.Tcp.Xfb   …              (Failed — the server process died)
```

```
MGPipe: Fatal{EventRingOverflow} - kEventBufferWriteback needs 1048600 bytes and SEG_EVENT caps
ONE record at 131072 (half its capacity).
```

inproc aborts the test process (the server is in it); spawn and tcp lose the peer instead, which
is the same defect seen from the other side of a socket.

**Also registered here**: `XfbAfterClipDistanceScenario.*` and `XfbRepeatedCaptureScenario.*` in
full on the three split arms (slice 1 had only `OrphanedCaptureTarget*`, because the rest could
not survive the unsliced event). `XfbRepeatedCaptureScenario`'s `gl_NextBuffer` compacted-target
shape is the first thing outside the census to exercise the archive-served stride and varyings
(`ProgramArchive->Link`'s xfb fields) on the server — **it passed on all three arms with no
further change**, so there was nothing of D1's to fix behind it.

**Gates.** `integration-split` 202/202, `integration-spawn` 118/118, `integration-tcp` 121/121,
`ctest -L unit` 2408/2408, the monolith + split XFB families 120/120,
`spawn_lane_parity.py build-split` RC 0, `fatal_census.py` RC 0 (79 sites, 0 unmarked),
`link_ratchet.py --assert-monotone` unchanged at 186. G1: `.text` `0xa52203`, both nm lists
byte-identical.

## slice 3 — the dead `OnXfbScatterReady` declaration

Plan B's section 7.1 named ten reverse-channel callbacks. The tenth, `OnXfbScatterReady`,
belonged to a design the tree does not have: plan B put the XFB scatter on the CLIENT — the
server would hand back the packed scratch and the client would run the patch loop over its own
shadow — so something had to tell the client the layout. P5c/P5f went the other way. The
declaration was left behind with **zero producers, zero consumers and no `EventKind`**, and the
`static_assert` beside `kMGPipeCallbackCount` made it look load-bearing.

Deleted; `kMGPipeCallbackCount` 10 → 9. Three pieces of prose that still described the old design
go with it:

* `MGPipeCallbacks.h`'s own header — "ten callbacks" → nine, with the reason the tenth went.
* `MGPipeTypes.h`'s `MGPXfbAccounting` comment, which said the scatter "is a read-modify-write of
  the client's shadow and lives there". It lives on the server's staged shadow.
* `ARCHITECTURE.md` §8.5, retitled from 「XFB scatter 搬到 client」 and rewritten to what the tree
  does: server staged shadow, one `OnBufferWriteback` carrying the reconciled range (sliced —
  slice 2), orphaned targets legal (slice 1). Its `OnXfbScatterReady` row in §8.1's table goes too,
  since leaving it would be a new staleness rather than an inherited one.

`PipeCatalogue.ReverseChannelHasTenCallbacks` now asserts 9. **The case name is deliberately not
renamed**: G2/G14 say the ctest name set only grows, so renaming would delete a name the gate
watches. The count it asserts is what has to be right; the comment carries the number.

**RESULT — red once, then green.** With the member and the count restored (everything else as
committed), `PipeCatalogue.ReverseChannelHasTenCallbacks` fails:

```
PipeCatalogueTest.cpp:1111: Failure
Expected equality of these values:
  kMGPipeCallbackCount   Which is: 10
  9u                     Which is: 9
```

which is what makes the struct's shrink a measured fact rather than a claim — the `static_assert`
beside the constant only proves the two agree with each other.

**Gates.** `integration-split` 202/202, `integration-spawn` 118/118, `integration-tcp` 121/121,
`ctest -L unit` 2408/2408, parity / census / ratchet (186) green. **G1 run for this slice in
particular**, because `MGPipeCallbacks.h` is included from MG_Backend and the struct is *not*
behind `MOBILEGL_BUILD_DISAGGREGATED`, so `sizeof(gMGPipeCallbacks)` really does shrink by 8 bytes
in the pull build: `.text` still `0xa52203` and both nm lists still byte-identical. The shrink is
in `.bss` and nothing indexes the table by count.

*(One unrelated flake seen and cleared: `DirectGLES.Tcp.PrimitiveRestartScenario.TheFixedIndex
ValueTakesTheForwardingPath` failed once in the tcp lane and passed on re-run, and the full lane
was re-run clean at 121/121.)*

## slice 4 — readback close-out

The plan's 回读 row had four parts. The headline is part (a): **the census came back clean**, which
is not what the row expected ("Expect reds — that is the point").

### (a) the readback lane census

Registered on the three split arms (`integration-split` / `-spawn` / `-tcp`), 22 cases per arm:

| family | cases/arm | result |
|---|---|---|
| `F1WireScenario.TextureAndFramebufferReadsPreservePackBufferPadding` | 1 | green |
| `F1WireScenario.TextureReadbackRejectsPackedDestinationOverflowBeforeWriting` | 1 | green |
| `PixelStoreSweepScenario.*` | 2 (+1 new, below) | green |
| `FramebufferChurnScenario.*` | 1 | green |
| `PackedWordReadbackScenario.*` | 3 | green |
| `LayeredTextureReadbackScenario.*` | 2 | green |
| `DepthStencilReadbackMatrixScenario.*` | 12 | green |

**No case needed a fix and no case needed a named deferral.** The first run was 65/65 (63 cases +
the two TCP fixture entries), before any of this slice's code changes. That is a real finding and
it is worth stating plainly: the 回读 half was assumed open because nothing had ever run it under a
transport, and the assumption, not the code, was what was wrong. `ScatterTightReadbackIntoPackState`
and the server's neutral-pack read were already carrying it.

Two honest caveats, because a skip is not coverage:

* `DepthStencilReadbackMatrixScenario.SeparateDepthAndStencilAttachmentsAreBothReadable` **SKIPS on
  all three arms** — and on monolith too. It is a driver fact, named by the case itself: *"this
  driver cannot host separate DEPTH_COMPONENT24 and STENCIL_INDEX8 attachments"*. Not a transport
  fact, nothing deferred.
* `DepthStencilReadbackMatrixScenario` needs `MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1`
  mirrored into the split arms, which the registration macro cannot carry, so it gets its own
  per-arm block (`DirectGLES.{Split,Spawn,Tcp}.ForcedDs.`). Without the pin the scenario is
  unfalsifiable here: ES has no core depth/stencil readback, Mesa accepts the reads anyway, and on
  llvmpipe every case goes green through a path the Adreno device does not have — deleting the
  whole emulation left all of them passing. A split arm inheriting the ambient default would have
  measured the same non-path three more times.

### (b) the two `std::abort` emulations

The row said both *"still `std::abort` under any non-monolith transport"*. Checked rather than
assumed, and the tree disagrees on one of them:

* **`copy-image-shadow-mirror` already cannot abort.** `MirrorCopyImageIntoDestinationShadow`
  returns under `if (MG_Config::Transport != Monolith)` **before** reaching
  `MGPipeUnmigratedEmulation`, and has since P5b package i1 (its own comment names the ruling and
  the two backstop Fatals that bound what the skip loses). Nothing to do; the row is stale.
* **`get-tex-image-shadow` is retired here**, and the interesting part is *why* it was there. P4a's
  comment reads "a split server holds no shadow to convert" — but the level storage this function
  converts **is the server's own**, fed by the staged texture store. When `MapMipmapData` answers,
  the conversion reads bytes that exist on this side and the answer is right; the emulation was
  never reaching across the split, it only looked as though it must. So: convert when the shadow
  is there, and decline by name (rule I: `MGLOG_E_ONCE` + `GL_INVALID_OPERATION`, destination
  untouched) when it is not. `std::abort` inside a server answering a client's readback takes the
  whole session down for a texture level it happens not to hold, and it bypasses `Session::Fail`,
  which is the funnel the census gate watches.
  **The funnel keeps its teeth for the other four sites** — `generate-mipmap-storage`,
  `generate-mipmap-cpu-fallback`, `generate-mipmap-cpu-filter`, `texture-remint-pull` — which
  really do reach into a client address space. Only this call site is retired, and only because its
  own premise did not hold. `fatal_census.py` is unchanged at 79 abort sites, because the abort
  lives in `MGPipeUnmigratedEmulation` and that function stays.

  **Reachability, said out loud**: no case in the armed census reaches this site under a transport,
  and the analysis says why — the client emits `MGPWireOp::GetTextureImage` and the server answers
  it with `ReadTextureImageWire` (a scratch-FBO read), never through the backend's `GetTexImage`
  slot. So this change is defence in depth and **has no integration-level red-once of its own**;
  saying otherwise would be inventing one.

### (c) the case nothing covered

`PixelStoreSweepScenario.ReadPixelsIntoAPackBufferHonoursPackStateAndLeavesTheGapsAlone`:
`glReadPixels` into a bound `GL_PIXEL_PACK_BUFFER` at a non-zero buffer offset, with
`PACK_ALIGNMENT`/`ROW_LENGTH`/`SKIP_ROWS`/`SKIP_PIXELS` all non-default, read back with
`glMapBufferRange`. This is the only thing that drives `EmitTables.cpp`'s `if (pbo)` branch — the
client servicing the pack PBO itself after the reply lands, uploading tight rows one at a time at
the stride the pack state implies. The existing sweep case reads into a client POINTER, which is
`ScatterTightReadbackIntoPackState`'s half and has unit cover; the PBO half had none, and it is
pure arithmetic (stride, alignment round-up, two skips, a buffer offset).

Every number is chosen to make a term matter: `ROW_LENGTH` 5 × 4 bpp = 20, which `ALIGNMENT` 8
rounds up to a **24**-byte stride; `SKIP_ROWS` 1 + `SKIP_PIXELS` 2 put the first pixel at image
byte 32; the read is issued at buffer offset 16, so it lands at **48**. The whole buffer starts
`0xCD` and the case asserts both halves: the rows land where the pack state says, and **every other
byte is still poison**.

**RESULT — red once, then green.** Dropping just the buffer-offset term from
`EmitTables.cpp:1001` (`start = pixels + …` → `start = …`) and rebuilding:

```
3794 DirectGLES.Split.PixelStoreSweepScenario.ReadPixelsIntoAPackBuffer…  (Failed)
3797 DirectGLES.Spawn.…                                                   (Failed)
3800 DirectGLES.Tcp.…                                                     (Failed)
```
`PixelStoreSweepScenario.cpp:353: readback[at] Which is: '\xCD' (205)` — the destination is still
poison because the rows landed 16 bytes early. The ambient monolith entries passed in the same run
(the driver services its own PBO there), so the three reds are a transport fact. `EmitTables.cpp`
was restored immediately and is **not** part of this commit.

### (d) the stale comment

`SplitReadbackPackBuffer`'s comment claimed *"The client refuses PACK_BUFFER before emission, so no
frontend pack binding can affect this destination."* The client does no such thing — it **keeps**
the PBO and services it after the reply. The conclusion (the server must see no pack buffer) was
right; the reason was backwards, and a refusal would have been observable as a GL error. Rewritten
to the actual reason, with ID-49's split of the work named.

**Gates.** `integration-split` 225/225 (base 186), `integration-spawn` 141/141 (base 102),
`integration-tcp` 144/144 (142 labelled + 2 fixtures; base 103 + 2), `ctest -L unit` 2408/2408,
`integration-magma-split` 73/73, parity / census (79 sites, 0 unmarked) / ratchet (186) green.
G1 `.text` `0xa52203`, both nm lists byte-identical.

## slice 5 — four transport-aware resolvers

**The hole.** `ResolveFramebufferSubsystemArm` and its three siblings
(`ResolveTextureResourceSubsystemArm`, `ResolveSampler…`, `ResolveProgram…`) classify a family's
arm as `Handles`, `Legacy` or `NoArm`, and stop only on `NoArm`. Under a transport `Legacy` is not
a slower answer, it is a **postponed crash**: all four pre-handle arms reach frontend state through
accessors that are BARRIER_PULLED rows (`FieldOwnership.def`), and under an active transport that
state belongs to a client that is no longer parked behind this apply — so the accessor's honest
answer is `Fatal{UnmigratedPipeInput, "<field>@<verb>"}` on the first verb that reaches it.

This is the audit's own claim about `g_fboTextureSyncList` — "separation-safe, because the record
arm returns first" — which is true **only while the record arm is armed**, and nothing checked
that. The row is in the plan (§1.2, `g_fboTextureSyncList`) precisely because the argument had
never been run.

**The fix.** Each of the four, under `#if MOBILEGL_BUILD_DISAGGREGATED`, now stops when
`MG_Config::Transport != Monolith && verdict != Handles`, through one shared
`StopOnTransportWithoutRecordArm(bit, detail)` that formats both causes and hands them to
`BufferImpl::StopOnArmlessPipeSubsystem` — this file's existing single voice for "the
configuration left no arm to run". A transport with the record arm off *is* that condition with a
different cause: the legacy arm is present but unreachable. Reusing the voice keeps the census's
family word (`PipeLegacyMemosDisabled`) and the cause travels in the message rather than in a new
word the census would have to learn.

**Red-once (R-16).** `MOBILEGL_PIPE_PUSH=0x1dff` (the Handles lane's `0x1fff` minus `0x200`,
`kMGPipeSubsystemFramebuffer`) with `MOBILEGL_TRANSPORT=inproc`, running
`ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels`. Both runs
abort (exit 134) — what changes is *what the abort says*:

*without the stop (fix reverted):*
```
Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@Clear"}
```
a mid-frame Fatal naming an accessor and a verb, from which nothing points at the mask.

*with it:*
```
Fatal{PipeLegacyMemosDisabled, "MOBILEGL_TRANSPORT is not monolith and
kMGPipeSubsystemFramebuffer (bit 9) is clear (or refused), so the record arm is off and the
legacy arm is unreachable: the pre-handle g_fboSynced* / g_fboTextureSyncList arm reads the
framebuffer binding slots, and under a transport GetFramebufferBindingSlot is a BARRIER_PULLED
row belonging to a client that is not parked behind this apply - so the first verb to reach it
would raise Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@<verb>"} mid-frame instead"}
```

**"Resolve time", not "startup"** — said precisely, because the difference matters and the plan's
wording ("启动期具名 stop") is looser than the tree. The four resolvers are function-local statics
resolved lazily at first use, deliberately (`Managers.cpp`'s own comment: a stop inside
`eglMakeCurrent` reads to the harness as "no usable GPU" and SKIPS). So the stop still lands in a
scenario body where ctest reports it. What it no longer does is wait for a verb to dereference the
client's state and then blame the accessor.

**Gates.** `integration-split` 202/202, `integration-spawn` 118/118, `integration-tcp` 121/121,
`ctest -L unit` 2408/2408, `integration-magma-split` 73/73, parity / census (79 sites, 0 unmarked)
/ ratchet (186) green. G1 `.text` `0xa52203`, both nm lists byte-identical — the four blocks are
inside `MOBILEGL_BUILD_DISAGGREGATED` and the pull build compiles none of them.
