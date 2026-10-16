# c0e — the contract and the wire (P5e). Report v1

Branch `p5e/c0e`, head `a513c53ee3b9fa1497312b8eddb499995e94d16f` (base `2fde7034`), six commits,
35 files, +2276/−195. Everything is INERT: the caps bit is not published, opcode 80 has no route,
bit 13 is not in `kMGPipeWiredSubsystems`, and every new applier entry point and by-handle backend
signature aborts by name. `build` / `unit` / `isplit` / `gens` green; `strict` is the phase's
expected-red baseline, recorded below.

## Gate

| step | result |
|---|---|
| `build` | rc 0 (341 TUs on the first full pass) |
| `unit` | **2205/2205** (`tcache_count=0`; 2203 at base + the two names below) |
| `isplit` | **111/111** |
| `gens` | `gen_pipe --check` up to date; `gen_pipe --self-test` **12** controls (9 at base); `gen_pipe_field_ownership --check` up to date, `--self-test` 15 controls; include-closure 4 probes / 0 problems; dirty-surface `--check` rc 0, `--self-test` 27 controls; doc citations **3 problems at HEAD vs 4 at base** — all three are pre-existing `CONTRACT-P5.md` rows, c0e adds none and incidentally retires one |
| `strict` | 108 failed / 2 skipped / 1 passed out of 111 — **unchanged in shape**: `.github/workflows/test.yml:1199-1206` gates this lane on the run FAILING, and the one pass is `SplitLogPaths.PrivateAndDistinct`, which draws nothing |

### The strict baseline the later packages must shrink

From the 88 private logs under `build-split/MobileGL/MG_IntegrationTest/split-logs/`; 86 carry
`BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1`. Each scenario aborts at its FIRST pulled row, so
this is the first-blocker census and not the full pull set.

```
39  Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@Clear"}          -> fb
14  Fatal{UnmigratedPipeInput, "GetBoundVertexArray@DrawArrays"}           -> vi
13  Fatal{UnmigratedPipeInput, "GetImageTextureBinding@BindImageTexture"}  -> fb
 9  Fatal{UnmigratedPipeInput, "GetTextureUnitObject@Clear"}               -> tx2
 6  Fatal{UnmigratedPipeInput, "GetTextureObject@CopyImageSubData"}        -> sticky, P7 (stays)
 3  Fatal{UnmigratedPipeInput, "GetProgramForDispatch@DispatchCompute"}    -> pg
 2  Fatal{UnmigratedPipeInput, "ValidateProgramName@ShaderStorageBlockBinding"} -> §7 allowlist
```

## What changed, per file

**`MG_Pipe/MGPipeTypes.h`** (+144) — `kCapRunAheadApply` (bit 10) and the pure arm
`MGPipeRunAheadCapBitsFor(BackendType, Bool ready)`; `kMGPipeMaxBufferBindingPoints` (84);
`kMGPipeShaderBufferClass{Uniform,ShaderStorage,AtomicCounter,Count}`; `MGPProgramBindings` (32 B)
+ `MGPProgramSamplerUnit` (8 B) + `MGPProgramStorageOverride` (40 B) + the three declared bounds;
`MGPProgramDesc::LinkStatus`; `kDrawClientArrays` (bit 6).

**`MG_Pipe/MGPipeValueTypes.h`** (+49) — `MGPipeImageAccess` and the three constants, with
`MGPipeDecodeImageAccess` / `MGPipeImageAccessIsValid` / `…Reads` / `…Writes`. Pure: the
include-closure probe stays 0 problems.

**`MG_Impl/Pipe/ImageEmit.h`** — `MGPipeEncodeImageAccess` MOVED out of the emitter's private
section into a free function that names the shared enumerators. Not duplicated: the literals are
gone.

**`MG_Pipe/MGPipe.h`** — `enum MGPipeWaitClass`; `kMGPipeSubsystemBufferBindings` (bit 13);
`kMGPipeSubsystemsMigratedAtP5e` = `0x3fff` with its drift assert.

**`MG_Pipe/PipeCalls.def`** (+220/−…) — the fifth column on all 79 rows plus
`SetProgramBindings` appended as opcode 80 (`kCtxState`, `kVarTail|kHostSpan`, `kWaitNone`), count
79 → 80, `kCtxState` 18 → 19.

**`scripts/gen_pipe.py`** (+127) — the 5-arg `CALL_RE`, `parse_wait_classes()` read out of
`MGPipe.h`, `check_wait_classes_are_known()` (unknown token; `kReplySlot`↔`kWaitReply` BOTH ways),
`kMGPipeWaitClasses[]` + `MGPipeWaitClassFor(op)` + five spot static_asserts in `PipeWire.inc`,
three new `--self-test` controls.

**`MG_Remote/Wire/PipeWireCodec.{h,cpp}`** (+112) — `WireRecordLayout` grows to
`kMaxWireRecordTails = 3`; the `SetProgramBindings` layout arm (three counts refused against their
bounds, three tails always declared, each 8-aligned); the decode arm that validates and DECLINES,
walking the third tail's name spans through `CheckHostSpanIsHonest` itself — they are members of a
40-byte element, so the encoder's blanket second-tail pass would read one span and a quarter of
garbage. The tail cross-check's Fatal line now prints all three tail sizes.

**`MG_Pipe/PipeApply.{h,cpp}`** (+244) — `MGPipeBarriered(op, payload, st)` **implemented** (all
three clauses); `MGPipeApplierCurrentRecordIsBarriered()` + its writer, **implemented**, default
`true`; `MGPipeApplySetShaderBuffers` / `MGPipeApplySetProgramBindings` declared and aborting; the
sb applier window (`BoundShaderBuffers[3][84]`, `ShaderBufferStart/Count/WritableMask[3]`,
`ShaderBuffersSerial`), the pg record tails (`BlockBindings`, `SamplerUnits`, `StorageOverrides`,
`Signature`, `BindingsSerial`) and `MGPipeProgramStorageOverride`; `MGPipeApplierState::
IsTransformFeedbackActive`, mirrored by `MGPipeApplySetContextValues`. `PipeApply.h` now includes
`MGPipe.h` (sibling layer; the closure gate is unchanged).

**`MG_Backend/DirectGLES/Managers.{h,cpp}`** (+120) — eight by-handle seams declared, bodies in
ONE block at the end of `Managers.cpp` calling `MGPipeP5eSeamNotLanded(name, owner, handle)`:
`VertexArrayImpl::ResolveVaoTwin`, `TextureImpl::{SyncTextureToBackendByHandle, ResolveTextureTwin}`,
`BackendTextureObject::SyncMipmapsToBackendByHandle`,
`BackendFramebufferObject::SyncToBackendByHandle`,
`BackendRenderbufferObject::SyncToBackendByHandle`, `PrgramImpl::ResolveProgramTwin`,
`BackendProgramObjectImpl::SyncToBackendByHandle`.

**`MG_Impl/Pipe/{Tracker.h, SetHashSuppressor.h, PipeFill.cpp}`** — bits 15/16/17 → subsystem 13
and `kMGPipeDirtyEmittedAtP5e`; the three `SetShaderBuffers*` suppressor slots and
`SetProgramBindings`; `SubsystemForEmitter`'s and `EmittedCallSuppliesTheWholeField`'s new arms,
three pairing asserts, and the `kMGPipeMaxBufferBindingPoints == BufferBindingPointCount` pin.

**`MG_Pipe/{Coverage.def, FieldOwnership.def, PipeFields.def}`** — the shape-only
`GetBufferBindingPoint → SetShaderBuffers` emitted row; the seven object-class rows re-annotated to
`"P5e (Espryt unbarriered), P7 (Magma)"`; `F(LinkStatus)` and the three new verify payloads (82).

**`Config.h` / `ConfigLoader.cpp` / `MG_Backend/Init.cpp`** — `RunAhead`, `PresentCredit`, the log
line, the `PipeVerify` force, the push default `0x3fff`, `kMGPipeP5eRunAheadReady = false` and the
caps arm.

**Tests** — `PipeCatalogueTest` (macro arity, the sizes, opcode 80, the null-route pins, the draw
flag, and the new wait-class case); `MagmaPipeIdentityTest` (new case + its pull-build skip twin);
`TrackerTest`; `FieldOwnershipTest`'s strict string. **Docs** — the P5E contract, the ROADMAP row,
`CURRENT_STAGE_PROGRESS` §1 and §6.

## Red-once evidence (verbatim)

1. **Flip one row's `WaitClass`, do not regenerate** (`GenerateMipmap` `kWaitApplied`→`kWaitNone`):
   `gen_pipe: OUT OF DATE: MobileGL/MG_Pipe/generated/PipeWire.inc`, rc 1. Reverted → `gen_pipe:
   generated files are up to date`.
2. **The same flip, regenerated** (the stronger half — this is what the contract-vs-def case buys):
   `PipeCatalogue.EveryRowCarriesTheWaitClassTheContractGivesIt ***Failed`,
   `PipeCatalogueTest.cpp:747: Failure … Expected equality of these values: MGPipeWaitClassFor(op)
   … kWaitApplied … GenerateMipmap`. Reverted → 34/34.
3. **`sizeof(MGPProgramBindings)` 32 → 40** (a `Uint64 Pad1` plus the matching `MGP_ASSERT_POD`):
   `PipeCatalogue.LateArrivalsAreAppendedWithoutRenumbering ***Failed`, `Expected equality of these
   values: sizeof(MGPProgramBindings) Which is: 40 / 32u Which is: 32`. Reverted → 34/34.
4. **Publish bit 10 on the Magma arm** (`MGPipeRunAheadCapBitsFor` stops testing the backend type):
   `MagmaPipeIdentityTest.AMagmaServerNeverPublishesTheRunAheadCapBit ***Failed`,
   `MagmaPipeIdentityTest.cpp:241: Failure … MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::
   DirectVulkan, true) & kCapRunAheadApply Which is: 1024 / 0u Which is: 0`. Reverted → 5/5.

## G1 / G2 / G14

- **G2/G14, new ctest names (both added, none removed):**
  `PipeCatalogue.EveryRowCarriesTheWaitClassTheContractGivesIt` (compiles in both flavours) and
  `MagmaPipeIdentityTest.AMagmaServerNeverPublishesTheRunAheadCapBit` (in
  `MGL_MAGMA_PIPE_IDENTITY_TEST_LIST`, so the pull build declares its skip twin).
- **G1: NOT MEASURED HERE** — no pull-build baseline `.so` exists on this machine and the gate
  wrapper has no pull flavour. The deltas to shared code, named:
  (a) `MGPipeScreen`/`MGPipeContext` each gain one function-pointer member and the three generated
  per-opcode arrays each gain one entry — **the identical delta P5b (five opcodes) and P5c (three)
  made, both of which measured 0/0/0/0**;
  (b) `MGPProgramDesc` 192 → 200 bytes — the payload is instantiated only under
  `MOBILEGL_PIPE_PUSH` (`PipeApply.cpp`, `PipeRoute.cpp`, `MG_Impl/Pipe/ProgramEmit.h` and the
  codec are all push-only sources), so the pull build holds none;
  (c) `MGPipeValueTypes.h` gains four `constexpr` functions and an enum — no symbol unless
  odr-used, and the pull build uses none of them;
  (d) `Config.h`'s `IpcTable` grows two `Uint32` — `IpcTable` is inside
  `#if MOBILEGL_BUILD_DISAGGREGATED`, so the pull build does not compile it at all.
  The integrator should run the real G1 pair at landing.

## Rows of my "family" (the contract), against `kimi-audit.md`

c0e retires no apply-thread read: it is the inert contract commit. What it DID do for the audit's
benefit is give every row a place to be retired INTO — the wait class that decides whether a row's
apply may read client memory at all, and the eight by-handle signatures the five per-draw families
replace bodies in. The audit's rows stay exactly where they are; the strict census above is the
first-blocker half of that list as measured at this head.

## Rulings I need from the integrator

1. **`MGPShaderBuffers::WritableMask` is a `Uint32` and the window is 84.** The mask can describe
   only the first 32 binding points of a class. The contract now records this (§5.6) and hands sb
   the choice — narrow the window, widen the field, or declare the bound — but it is a WIRE field,
   so it is not sb's to pick. This was not caught in the brief or in S5.
2. **`MGPProgramDesc` grew 192 → 200.** The draft's "in the descriptor's existing pad, size
   unchanged" is arithmetically impossible: the fixed head is exactly full at 24 bytes and
   `MGPBlobRef` is 8-aligned. I landed the honest version and argued it in the contract and in the
   header. If the integrator would rather pay a bit in `StageMask` or shrink
   `ReservedNumSamplesOffset`, that is a different record and a different commit.
3. **Consumer bit 13 in `Init.cpp`'s `ConsumedSubsystemsFor`.** I did NOT add it: withholding a
   consumer bit is the safe direction (the file says so itself), no emitter exists yet, and
   `Init.cpp`'s consumer mask is not in c0e's brief scope. sb must add it in the same commit that
   gives `ShaderBufferEmit.h` its wired constant, or the client's R-8 gate will withhold the whole
   family. Flagged so it is not discovered at the merge.

## Seams I assumed

- `MGPipeApplySetProgramBindings` takes the three tails PLUS a `const char* const*` of RESOLVED
  override names, because the names ride SEG_STAGE and SEG_STAGE retires with the record (rule C).
  pg must copy them into the record before the call returns. If pg wants the spans instead, the
  signature is mine to change — ask.
- `MGPipeBarriered`'s `payload` may be null and clause 3 then falls through. ra's emit-side caller
  must pass the real `MGPDrawInfo` it is about to publish.
- `MGPipeApplierCurrentRecordIsBarriered()` is `thread_local` and defaults `true`. id sets it from
  `MGPipeBarriered` at the top of `ApplyOne`; nothing clears it, because every dispatch sets it.

## What I did not do

- **G1 measurement** (above) and the strict lane's *comparison against a base run* — the lane is
  expected-red by CI design at this head, so what I recorded is the marker census, not a diff. No
  base build exists on this machine.
- **`MOBILEGL_IPC_RUN_AHEAD` / `PRESENT_CREDIT` have no reader yet.** They parse, they log, they
  are forced by `PipeVerify`. `ClientSession` is ra's file and c0e does not touch it.
- **`ARCHITECTURE.md` §11/§14/§17 and `README.md`'s knob table** still describe the lockstep and
  list neither knob. The brief gives `ARCHITECTURE.md` to ra; `README.md` is nobody's — the
  integrator should hand it to ra with the rest.
- **`.github/workflows/test.yml`'s strict step** is untouched (ra's, per BRIEF §2).
- **`MG_Test/Wire/ServerLoopTest.cpp`'s bit-10 pin.** BRIEF §6 names it beside
  `MagmaPipeIdentityTest`; the unit case I added covers the arm as a pure function, which is the
  half a unit lane can reach. A `ServerLoopTest` pin over a real `CallMask` belongs with ra's
  session work.
