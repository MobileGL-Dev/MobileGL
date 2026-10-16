# c0b — the P5b contract package, v1

Branch `p5b/c0b`, forked from `feat/disaggregated @ c9878d69`, merged `feat/disaggregated @
f5614995` (v1 round 3) before finishing. Worktree `~/w7/p5b-c0b`. Nothing pushed; nothing merged
into the integration branch.

```
4dd4027e [Feat] (MG_Pipe, P5b): append the five P5b verb rows at opcodes 72..76, arm draw_vbo
         with the kDrawIsIndirect second tail, give resource_copy_region its two GL names and
         the clear discriminants one spelling, and stamp every P5b verb boundary
a1641130 [Feat] (MG_Remote, P5b): decode every P5b row to a WireVerbSink method, stub each
         ServerVerbSink body with a named UnmigratedVerb Fatal, and partition the emit table
         class-C list per migration package
02972f70 [Test] (MG_Test, P5b): pin the 76-row catalogue, the 18 stamp rows and the 77 verify
         payloads, and round-trip each P5b row once through the codec
538c3c19 [Docs] (MG_Remote): CONTRACT-P5B.md - the wire records, sinks, refusals and rulings
         for the 25 measured class-C slots
f5676103 [Merge] (P5b, c0b): merge feat/disaggregated at f5614995 (v1 round 3, ID-62/ID-67)
         into p5b/c0b
```

Range **`4dd4027e..f5676103`** (four single-line commits plus the merge). The working tree is
clean; the worktree's submodule symlinks are left OFF (git-safe).

Notes written beside it: `~/w7/notes/p5b/PACKAGE-PREAMBLE.md`, `~/w7/notes/p5b/BRIEF-P5B.md`.
The contract: `MobileGL/MG_Remote/CONTRACT-P5B.md` (beside `CONTRACT-P5.md`, which stays
authoritative for everything it covers).

---

## 1. What landed

| # | item | state |
|---|---|---|
| 1 | `CONTRACT-P5B.md`: every one of the 25 slots - wire record, class, sink signature, reply slot (none), thread rule, refusal shape, monolith (untouched) - grouped d1 / i1 / t2 / f1; table 0 additions; table 2 and 3 under P5b; knobs (none); 12 rulings with overturn conditions; the 71-slot partition; ownership with two pre-granted edits | **done** |
| 2 | `PipeCalls.def`: **five rows appended at opcodes 72..76** (`BindShaderImage`, `PatchParameter`, `BindStreamOutput`, `SetStorageBlockBinding` `kHasBlob`, `CopyFramebufferToTexture`), documented count 71 → 76; no opcode moved | **done** |
| 3 | `MGPipeTypes.h`: the five PODs (40 / 8 / 16 / 40 / 48 B); `kDrawIsIndirect`; `MGPCopyRegion` 64 → 72 (two GL names); `MGPMipPlan::Target` ruled the GL enum; the clear discriminants `kMGPipeClearKind*` / `kMGPipeClearValueClass*` (one spelling; the two hand-minted copies in `EmitTables.h` and `PipeApplier.h` now alias them) | **done** |
| 4 | `PipeFields.def`: five field lists + `MGPCopyRegion`'s two names; verify payloads 72 → 77. `FieldOwnership.def`: six stamp rows (`ResourceCopyRegion → CopyImageSubData` and the five new ops), 12 → 18. Generated tables regenerated; `gen_pipe.py --check` / `--self-test` (9/9), `gen_pipe_field_ownership.py --check` / `--self-test` (15/15), `gen_pipe_dirty_surface.py --check` all green | **done** |
| 5 | Codec: `MGPW_FOR_EACH_CALL` + name/size tables for the five rows; `BlobSlotsFor` for the name blob; `draw_vbo`'s layout takes `kDrawIsIndirect` (exclusive with the span, `NumDraws == 0`, both `Fatal{ProtocolCorruption}` on both sides); `WireRecordLayout::SecondTailIsHostSpans` so the encoder's span honesty pass cannot read an indirect block as spans; arms for the six pre-existing rows that returned false (`LaunchGrid`, `MemoryBarrier`, the four stream-output rows), for `ResourceCopyRegion` and `GenerateMipmap`, and for the five new rows (the name blob is bounded at 4096 bytes, copied and re-terminated, rule C) | **done** |
| 6 | `WireVerbSink`: `OnDrawVbo` gains `const MGPDrawIndirect*`; 13 new methods. `ServerVerbSink`: 13 overrides, every body `Fatal{UnmigratedVerb, "<GL slot>"}` by the slot's own name ("(server sink)" in the message text); `OnDrawVbo` declines the indirect block, the span, `NumDraws > 1` and the instanced arms by name instead of by log line | **done** |
| 7 | `EmitTables.cpp`: class C partitioned into `MGR_UNMIGRATED_{D1,I1,T2,F1,TAIL}_SLOTS` with per-package `kEmittedSlots*` and ownership `static_assert`s 19 / 7 / 7 / 11 / 20 = 64; `EmitTables.h` includes `MGPipeTypes.h` for the aliases. **No slot flipped**: `UnmigratedSlotCount() == 64`, `ImplementedVerbCount() == 5` as `RemoteClientTest` pins | **done** |
| 8 | Tests: `PipeCatalogueTest` (kCtxVerb 18, opcodes 72..76 by value, `kOpCount` 77, the P5b flags/sizes, the clear constants, verify payloads 77); `FieldOwnershipTest` (18 stamp rows, six by name); `PipeWireCodecTest` (13 new cases: 11 round trips - one per row, the four stream-output rows in one - and two forged-record Fatals for the draw flag exclusion and the indirect-with-ranges refusal) | **done** |
| 9 | Notes: `PACKAGE-PREAMBLE.md` (numbers on this head, the ID-66 process rules, the `setsid` trap), `BRIEF-P5B.md` (goal, four packages with slots / rows / models, per-package gate, file ownership, the loop) | **done** |
| 10 | Tools: `~/w7/p5b-c0b-{setup,submodlinks,gate,accept}.sh`, `~/w7/p5b-c0b-census.{sh,py}` (the P6 lane runner over any tree: `p5b-c0b-census.sh <tree> <outdir>`) | **done** |

---

## 2. Acceptance numbers

All on `~/w7/p5b-c0b @ f5676103` (the merged head), `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`.
Log: `~/w7/p5b-c0b-gate-run3.log`; the census: `~/w7/p5b-c0b-census-logs/results.json` (per entry:
status, rc, first `Fatal{…}`, command, working directory, its own `.log`/`.out`).

### 2.1 Builds — four for four

`build-linux` (pull), `build-push`, `build-verify`, `build-split`: **configure rc=0, build rc=0,
all four**, `CTestTestfile.cmake` confirmed in each.

### 2.2 G1 — did not move

```
nm --defined-only build-linux/libMobileGL.so | grep -ic MG_Remote  ->  0
nm --defined-only build-split/libMobileGL.so | grep -ic MG_Remote  ->  627
symbol-report vs ~/w7/p5-before-libMobileGL.so (.READY 4595163c), --threshold 0:
  .text 10806611 -> 10806611 (+0, +0.000%)
  27814 -> 27814 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
```

Held because everything c0b added is either a header-only type, a `.def` row whose generated
tables the pull build never odr-uses (the baseline `.so` defines no `gMGPipe*` symbol), or an
`MG_Remote` file compiled only under `MOBILEGL_BUILD_DISAGGREGATED`.

### 2.3 Generators and purity

```
gen_pipe.py --check                       : generated files are up to date
gen_pipe.py --self-test                   : 9 negative-control trips, positive control OK
gen_pipe_field_ownership.py --check       : generated file is up to date
gen_pipe_field_ownership.py --self-test   : 15 negative-control trips, positive control OK
gen_pipe_dirty_surface.py --check         : green
check_include_closure.py                  : 4 probes, 0 skipped, 0 problem(s)
check_doc_citations.py CONTRACT-P5B.md    : 36 citations, 0 problem(s)
```

### 2.4 Test lanes

| lane | result |
|---|---|
| `ctest -L unit` build-linux / push / verify | **1817 / 1817** each (the head's baseline) |
| `ctest -L unit` build-split | **2084 / 2084** = the head's 2071 + c0b's 13 codec cases; no known red (c1g landed before c0b forked) |
| `ctest -L integration-split` build-split, `MOBILEGL_TRANSPORT=inproc` | **22 / 22** (2 skipped by design, as in ID-66's quick gate) |
| `ctest -L integration-gpu` build-push, `MOBILEGL_TRANSPORT=monolith` | **1128 / 1128** |
| G2 pull-vs-push `ctest -N` names, every label | **0 diff lines** |
| G14 build-linux names vs `~/w7/p5-before-ctest-names.txt` | **0 removed**, 43 added (c0b's 13 + v1 round 3's 30 `ServerLoopTest` cases) |
| G5 `p3a_untouched_regions.sh ff2994d9 HEAD` | rc=0, byte-identical |
| G5 `p4a_untouched_regions.sh ff2994d9 HEAD` | rc=0, byte-identical |

### 2.5 The inproc census on this head

`~/w7/p5b-c0b-census.sh ~/w7/p5b-c0b ~/w7/p5b-c0b-census-logs` (the P6 runner over this tree;
1,149 entries, eight workers, private logs):

| status | P6 census (`e61d0012`) | c0b head (`f5676103`) |
|---|---:|---:|
| passed | 426 | **432** |
| skipped | 185 | **185** |
| subprocess-aborted | 511 | **505** |
| ordinary failed | 27 | **27** |

**The verb census is unchanged: 505 `UnmigratedVerb` aborts, the same 505, the same per-slot
table** (`BindImageTexture` 138, `BeginTransformFeedback` 95, `DrawElements` 56, `DispatchCompute`
49, `PatchParameteri` 43, `CopyImageSubData` 30, `MemoryBarrier` 18, `MultiDrawElementsBaseVertex`
18, `ClearBufferiv` 12, `ClearBufferuiv` 8, `ClearBufferfv` 6, `DrawArraysInstancedBaseInstance` 4,
…). The six entries that moved are exactly the six the P6 census listed as
`Fatal{StageSnapshotTooNarrow, "respecify_whole_store"}` (two `LargeArenaAdoptionScenario`
pairs, `MapPersistentRoundtrips.LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMap`,
`ResourceSubsystemControlScenario.ClearingTheP3aBits`), now PASSED - v1 round 3's M-3 fix
(`1646049a`, "aborted six LargeArenaAdoption / ResourceSubsystemControl joint entries by name"),
which landed on `feat/disaggregated` while c0b ran and is in the merge. Entry names identical
between the two runs; no other entry changed status. **432 / 185 / 505 / 27 is therefore the
baseline the four packages diff against**, and every abort in it is a verb abort.

---

## 3. The 25 rows and their carriers, in one table

`R` = an existing row; `N` = appended by c0b. Every payload is an inline POD in `SEG_CMD`; the
two variable parts are named. No row owns a reply slot.

| # | GL slot (census) | pkg | row (op) | R/N | fixed payload | var part / carrier | sink | stamp |
|---|---|---|---|---|---|---|---|---|
| 1 | `DrawElements` (56 + every MC trace) | d1 | `draw_vbo` (59) | R | `MGPDrawInfo` 56 B | `MGPDrawRange[NumDraws]` in `SEG_CMD`; client-side indices: `MGHostSpan` tail naming a `SEG_STAGE` run | `OnDrawVbo` | `DrawArrays` |
| 2 | `DrawElementsInstancedBaseVertex` (trace) | d1 | 59 | R | `InstanceCount`, `IndexBias` | as 1 | `OnDrawVbo` | `DrawArrays` |
| 3 | `MultiDrawElementsBaseVertex` (18) | d1 | 59 | R | `NumDraws = drawcount` | `MGPDrawRange[drawcount]` | `OnDrawVbo` | `DrawArrays` |
| 4 | `DrawArraysInstancedBaseInstance` (4) | d1 | 59 | R | `IndexSize 0`, `InstanceCount`, `StartInstance` | one range | `OnDrawVbo` | `DrawArrays` |
| 5 | `DrawElementsBaseVertex` (4) | d1 | 59 | R | `IndexBias` | one range | `OnDrawVbo` | `DrawArrays` |
| 6 | `MultiDrawArrays` (2) | d1 | 59 | R | `IndexSize 0`, `NumDraws` | ranges | `OnDrawVbo` | `DrawArrays` |
| 7 | `MultiDrawElements` (2) | d1 | 59 | R | `NumDraws` | ranges | `OnDrawVbo` | `DrawArrays` |
| 8 | `DrawArraysInstanced` (2) | d1 | 59 | R | `InstanceCount` | one range | `OnDrawVbo` | `DrawArrays` |
| 9 | `MultiDrawElementsIndirect` (2) | d1 | 59 | R (tail N) | `Flags = kDrawIsIndirect`, `NumDraws 0` | `MGPDrawIndirect` (40 B) as the second tail, in `SEG_CMD` | `OnDrawVbo(…, indirect)` | `DrawArrays` |
| 10 | `MultiDrawArraysIndirectCount` (2) | d1 | 59 | R (tail N) | as 9 + `ParameterBuffer`, `ParameterOffset`, `DrawCount = maxdrawcount` | `MGPDrawIndirect` | `OnDrawVbo` | `DrawArrays` |
| 11 | `BindImageTexture` (138) | i1 | `bind_shader_image` (72) | N | `MGPImageBind` 40 B (GL args verbatim + handle) | — | `OnBindShaderImage` | `BindImageTexture` |
| 12 | `DispatchCompute` (49) | i1 | `launch_grid` (60) | R | `MGPGridInfo` 48 B | — | `OnLaunchGrid` | `DispatchCompute` |
| 13 | `CopyImageSubData` (30) | i1 | `resource_copy_region` (53) | R (72 B) | `MGPCopyRegion` + `SrcGlName/DstGlName` | — | `OnResourceCopyRegion` | `CopyImageSubData` (new stamp row) |
| 14 | `MemoryBarrier` (18) | i1 | `memory_barrier` (61) | R | `MGPMemoryBarrier` 8 B | — | `OnMemoryBarrier` | `MemoryBarrier` |
| 15 | `ShaderStorageBlockBinding` (4) | i1 | `set_storage_block_binding` (75) | N, `kHasBlob` | `MGPStorageBlockBinding` 40 B | the block name, `SEG_STAGE`, `Size = strlen + 1` | `OnSetStorageBlockBinding(…, name)` | `ShaderStorageBlockBinding` |
| 16 | `BeginTransformFeedback` (95) | t2 | `begin_stream_output` (62) | R | `MGPStreamOutputBegin` 8 B | — | `OnBeginStreamOutput` | `BeginTransformFeedback` |
| 17 | `PatchParameteri` (43) | t2 | `patch_parameter` (73) | N | `MGPPatchParameter` 8 B | — | `OnPatchParameter` | `PatchParameteri` |
| 18 | `BindTransformFeedback` (2) | t2 | `bind_stream_output` (74) | N | `MGPStreamOutputBind` 16 B | — | `OnBindStreamOutput` | `BindTransformFeedback` |
| 19 | `ClearBufferiv` (12) | f1 | `clear` (57) | R | `MGPClear` 48 B, `Kind/ValueClass` per the one spelling | — | `OnClear` (live) | `Clear` |
| 20 | `ClearBufferuiv` (8) | f1 | 57 | R | as 19 | — | `OnClear` (live) | `Clear` |
| 21 | `ClearBufferfv` (6) | f1 | 57 | R | as 19 | — | `OnClear` (live) | `Clear` |
| 22 | `ClearBufferfi` (1) | f1 | 57 | R | as 19 | — | `OnClear` (live) | `Clear` |
| 23 | `ClearNamedFramebufferfv` (2) | f1 | 57 | R | `Fbo` = the named handle (Named record precedes) | — | `OnClear`; unbound → refused by name | `Clear` |
| 24 | `CopyTexImage2D` (3) | f1 | `copy_framebuffer_to_texture` (76) | N | `MGPCopyFromFramebuffer` 48 B | — | `OnCopyFramebufferToTexture` | `CopyTexImage2D` |
| 25 | `GenerateMipmap` (2) | f1 | `generate_mipmap` (54) | R | `MGPMipPlan` 16 B, `Target` = GL enum | — | `OnGenerateMipmap` | `GenerateMipmap` |

Companions that share a row and whose slots the same package owns (unmeasured, no gate):
`DrawElementsInstanced*` ×3, `DrawRangeElements*` ×2, `*Indirect` ×4 (d1);
`DispatchComputeIndirect`, `MemoryBarrierByRegion` (i1); `End/Pause/ResumeTransformFeedback`
(t2; `DeleteTransformFeedback` has no row); `ClearNamedFramebufferfi/iv/uiv`,
`CopyTexSubImage2D` (f1).

---

## 4. Rulings I made, each with what would overturn it

The full list with evidence is CONTRACT-P5B.md §6; the ones a package will trip over:

1. **No P5b slot gains an `MGPipeApply*`; all 25 reach `WireVerbSink`.** The applier's 37
   entry points are the object/state families; a verb is a backend CALL, and the census's
   correction (the P5 five already go to the sink) is the pattern. Consequence: R-17's generated
   routing is untouched, `PipeCatalogueTest` still reads 33 installed + 4 escapes, and a
   migration is two files per slot. *Overturned by* a verb that has to be replayable from the
   monolith side through a table (P13's recorder).

2. **Rule D: a verb crosses AS THE CALL.** GL enums and GL names verbatim beside the handle;
   the sink reproduces the backend call; the backend keeps reading its verb class's
   BARRIER-PULLED fields. This is why nothing on the monolith path changes and why `rsp` grows -
   the phase's measurement, not a regression. *Overturned by* nothing in P5b; retiring the pulls
   is P3b/P4b/P7/P8's per CONTRACT-P5 table 2.

3. **Four of the five appended rows exist because the backend does work AT THE CALL** (Espryt:
   `SyncImageTextureBinding` at `DirectGLES.cpp:9154`, `glPatchParameteri` at `:8760`, the XFB
   object rebind at `:1390`, the driver-program rebinding at `:9200`) that no validate-time set
   record reproduces; the fifth (`copy_framebuffer_to_texture`) has a source no resource row can
   name. *Overturned by* the backend moving that work into its draw prep - G5-pinned regions,
   so not P5b's.

4. **`resource_copy_region` = `glCopyImageSubData` only**, and the framebuffer-sourced copies
   get row 76 - which also settles the stamp row `FieldOwnership.def` had left to "the phase
   that emits it".

5. **`kCapNeedsHostIndexBytes` stays 0 while the user-index span is ARMED.** The cap is the
   server asking for VBO-backed index bytes (P8's mirror); the span is the client handing over
   bytes only it has (the P8 resolve-on-client rule applied to the one case that needs it).
   CONTRACT-P5 table 0 conflated them because P5 produced neither. *Overturned by* a client
   index array larger than a `SEG_STAGE` run at 32 MiB (R-10's `maxrec=` would say).

6. **Renderbuffer copy endpoints, unbound DSA clears, and multi-draws with client-side indices
   are refused BY NAME** (ID-57's shape) - no barrier-pulled forward can serve them and none
   was measured. *Overturned by* a measured workload hitting one.

7. **The `copy-image-shadow-mirror` site is skipped under split** (behind
   `#if MOBILEGL_BUILD_DISAGGREGATED`, i1's edit); the two mipmap emulation sites stay Fatal
   (one may be re-derived from the descriptor by f1; the CPU fallback needs P9's
   `OnTextureWriteback`). *Overturned by* a measured `GetTexImage` of a copy destination before
   P9 - which is the wave-3 `GetTexImage` Fatal anyway.

8. **`GetProgramForDispatch` FATAL → BARRIER_PULLED "P7 (Magma), P8 (Espryt)"** (i1's one-row
   edit; granted). **`kCapBackendOwnsXfbCapture` (1<<9)** for the `EndTransformFeedback` probe
   at `GL_Drawing.cpp:1290` (t2's edit; granted). The only two c0b-file edits a package makes.

9. **The emit-table class-C list is partitioned per package** with five ownership
   `static_assert`s, so four parallel landings edit four disjoint regions and a package that
   touches another's list breaks the other's line. Arithmetic; nothing overturns it.

10. **`MGPCopyRegion` 64 → 72.** No producer, no consumer, no wire history; the names are the
    pull keys the P5b sink needs beside the handles P7 will use. *Overturned by* P7's
    handle-keyed endpoints, when the two fields become padding (and stay in the layout).

11. **Client-side vertex arrays are not a P5b question**: served on the server through the
    barrier-pulled `GetBoundVertexArray` (CONTRACT-P5 table 2 "P8"), correct under inproc by
    the barrier and the shared address space, retired by P8's `HostResolve.cpp`. *Overturned
    by* P6's spawn - which ID-68 puts after P5b.

12. **Companions ride for free but are not gated.** Nine draw variants, two compute/barrier
    variants, three XFB span controls and four clear/copy variants share a measured row; each
    package owns their slots (the ownership counts include them) and may flip them in the same
    change, but the census gate names only the 25.

---

## 5. What I believe the brief or the census got wrong or incomplete

1. **The brief's `IVerbSink` is `MG_Remote::Wire::WireVerbSink`** (declared in the codec header
   so the codec does not depend on the session), and its server implementation is
   `Server::ServerVerbSink`. Nothing else in the tree is named `IVerbSink`.
2. **The census's "reuse `MGPipeApplySetShaderImages` / `MGPipeApplySetPatchState`" for
   `BindImageTexture` / `PatchParameteri` does not work**: both appliers write the working block
   at VALIDATE time and the backend slots do their work AT THE CALL (§4.3). Those two slots
   needed rows of their own, which is where two of the five appended rows come from. The
   census's "add/finish `MGPipeApplyLaunchGrid` / `MGPipeApplyDrawVbo` / `MGPipeApplyClear`"
   is likewise not the shape: the census's own correction (verbs reach the sink) is.
3. **`ShaderStorageBlockBinding` was documented as "folded into the reflection archive"**
   (`PipeCalls.def` footer, `BackendObject.h`). The archive carries the bindings as LINKED;
   `glShaderStorageBlockBinding` moves one AFTER link and both backends apply it to an
   already-built program. It needed a row (75); the footer is corrected in place.
4. **The 25 are 25 only if `DrawElementsInstancedBaseVertex` counts** (trace-only, 0 lane
   entries). The brief's d1 list has 10 with it; the census's integration table has 24 non-zero
   rows. Both are right; the contract lists all 25 by name.
5. **`FieldOwnership.def`'s stamp map covered the pre-existing rows but not
   `ResourceCopyRegion`**, by design ("the phase that emits it adds its row"). Left to a
   package, an omitted stamp row is the ONE silent failure the table has (the record applies
   under the previous verb's serial). All six rows are landed here, not left to the packages.
6. **`feat/disaggregated` moved twice while c0b ran** (c1g at `c9878d69` before I forked; v1
   round 3 at `f5614995` after). Merged; one four-line overlap in `PipeApplier.h`, no conflict.

---

## 6. Debts, for the packages and the phases after

- **`rsp` per Minecraft frame is unmeasured** until d1 renders one; each package reports it
  for its scenarios (PACKAGE-PREAMBLE §"五件事" 4).
- **Renderbuffer `glCopyImageSubData` endpoints** need a renderbuffer forward or P7's handle
  keying (§4.6). **Unbound DSA clears** need Espryt's handle-keyed
  `SyncAndBindFramebufferObject` sibling (P7 / P3b-P4b, or f1 behind the option). **Multi-draw
  with client-side indices** needs P8's `HostResolve.cpp`.
- **`DeleteTransformFeedback` has no row**: the driver object leaks on the server until P9's
  XFB namespace work; a bind of name 0 is what the backend does on delete of the bound one.
- **`MGPGridInfo::Block*` is 0 in P5b**; P7 Magma fills it from the reflection archive if it
  needs it.
- **The `Fatal` NAME a pull prints under a P5b draw is `DrawArrays`** for all twenty draw verbs
  (one stamp row, one kDraw mask). The wire's `UnmigratedVerb` names are exact; the poison's
  `@DrawArrays` is not, and d1's reports should say so once rather than be surprised.
- **P5's `.text` baseline moved** between c0 (10806323) and this head (10806611) with the
  landed P5 packages; `p5-before-libMobileGL.so` (`.READY` `4595163c`) is the comparand and
  the report's G1 is against it.
