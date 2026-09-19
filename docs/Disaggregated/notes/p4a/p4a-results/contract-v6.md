# contract v6 — c0f: the four P4a families require a registered consumer (ID-39)

**Commit `29d51ab9`** on `p4a/c0f`, worktree `~/w7/p4a-c0f`, branched from
`refs/heads/feat/disaggregated` = `17396216` (contract c0..c0e + wire + clientsp v3 + clientfb v2 +
esprytobj v2). One commit, seven files, +570/-11. Not pushed.

```
[Fix] (MG_Pipe, MG_Impl): gate the four P4a families on a backend having registered
MGPipeResourceOps - Magma emitted, the applier accepted, and the acceptance-cleared dirty flags
left its legacy path nothing to upload
```

---

## 1. The defect, restated from the code

P3a's buffers have always had **two** conjuncts in their client gate — `PipeFill.cpp:612`
`MGPipeResourceSubsystemEnabled()` is `bit 7 AND MGPipeGetResourceOps() != nullptr` — and the
comment at `PipeFill.cpp:656` (now ~:700) spells out why the second one is load-bearing.

P4a's four families had only **one and a half**: `FamilyIsLive(subsystem, wired)` was the
operator's `MOBILEGL_PIPE_PUSH` bit AND the family's `kMGPipeWired*Subsystem` constant, with no
per-backend half at all. DirectVulkan (Magma) registers no `MGPipeResourceOps`
(`Managers.cpp:2556` is DirectGLES-only) and has none of P4a's twins, so with
`kMGPipeWiredTextureSubsystem = 1`:

1. the client emitted `resource_create` / `resource_respecify` / `resource_subdata` /
   `set_texture_params` for every texture;
2. the applier **accepted** every one of them (a non-buffer resource row is stored and returned —
   `MGPipeApplyResourceCreate`'s own comment says so);
3. the emitter cleared the level's per-level dirty flags on that acceptance (D-D5 as amended by
   ID-18 M3);
4. Magma's legacy pull path then read `IsStorageDirty` and found nothing to upload.

66 texture-upload-shaped DirectVulkan integration-gpu cases red on the push build at the default
mask, pull build 966/966 green. This is B's own finding (1) from ID-35, applied to Magma.

---

## 2. (a) The client gate — `MobileGL/MG_Impl/Pipe/PipeFill.{h,cpp}`

| file:line | what |
|---|---|
| `PipeFill.cpp:882` | `kMGPipeP4aFamilySubsystems` = `Framebuffer | TextureResources | Samplers | Programs` (bits 9/10/11/12). Deliberately **not** `kMGPipeSubsystemsMigratedAtP4a` (0x1fff, every bit through P4a) — the rule belongs to the four families this phase adds and to no earlier one. |
| `PipeFill.cpp:920` | `Bool P4aFamilyHasItsConsumer(Uint64 subsystem)` → `(subsystem & kMGPipeP4aFamilySubsystems) == 0 \|\| MGPipeGetResourceOps() != nullptr`. A P2/P3a bit answers `true` unconditionally, so nothing that emits today changes. |
| `PipeFill.cpp:933-935` | `FamilyIsLive` gains the conjunct. This is the predicate **every** P4a birth hook resolves through (`MGPipeEmitTextureResourceCreate/Respecify`, `MGPipeEmitTextureParams`, `MGPipeNoteTextureLevelDirty`, `MGPipeEmitRenderbufferResourceCreate/Respecify`, `MGPipeEmitSamplerCsoCreate`, `MGPipeEmitSamplerViewCreate`, `MGPipeEmitShaderCsoCreate` — nine hooks, ~:1105-1170). |
| `PipeFill.cpp:2257` | `wants()` gains it as a fifth condition, so the seven walk-side emissions (`EmitFramebufferState`, `EmitShaderState`, `EmitSamplerViews`, `EmitSamplerStates`, `EmitShaderImages`, `EmitGlobalConstants`) are inert too. |
| `PipeFill.cpp:2329` | the `DrainTextureSubData` gate — the one with no dirty bit over it — gains it. This is the path whose acceptance clears the level flags. |
| `PipeFill.cpp:2408` | the residual fill's `supplied` conjunction gains it: "supplied" means *a call went out carrying this field*, and with no consumer none did, so withholding the pull would leave the field unfilled at the verb that reads it. (Inert today — `EmittedCallSuppliesTheWholeField` answers `false` for every P4a field; it is forward protection for the first one admitted.) |
| `PipeFill.cpp:1097` / `PipeFill.h:63` | `Bool MGPipeP4aFamilyEmits(Uint64 subsystem, Uint64 wired)` — the gate exported as an observable, returning `FamilyIsLive(...)` itself rather than a second copy. `wired` is a parameter because the constant lives in the family's emit header, which `PipeFill.h` may not include. Unit gate only. |

**The emit headers were not touched.** `MGPipeTextureRecordsReachTheApplier()` and its siblings are
compile-time (`constexpr`) restatements of the wired constant, and
`MGPipeTextureSubsystemEnabled()` / `MGPipeFramebufferSubsystemEnabled()` are B/C's runtime
copies; every path they guard runs *behind* `FamilyIsLive`, or (`RepublishMask`) behind
`MGPipeHandleIsPublished`, which no longer answers true. The single exception is documented in
(b).

**Bits 7 and 8 are untouched.** `MGPipeResourceSubsystemEnabled` and the vertex-input rows read
exactly what they read before.

**The register/unregister window** is the same one P3a lives with (`UnregisterBufferBackendOps`
nulls the table at context teardown, the re-register happens at the next MakeCurrent) and is
closed the same way: an object born in the window latches `Published = false`, and the family's own
self-healing create on the next respecify (`TextureEmit.h:576` / `:709`) publishes it. Written into
the comment at `PipeFill.cpp:914`.

**Make-current does not flap the gate.** `MGPipeApplierReset` explicitly does *not* clear
`g_resourceOps` (`PipeApply.cpp:1121`, and `ResourceEmit.TheResourceOpTableIsUnregisteredUntilA
BackendInstallsOne` pins it); `MGPipeApplierReleaseObjectRecords` does not touch it either and has
no non-test caller in the monolith. So the client's gate and the applier's belt read the same
signal and cannot disagree across a frame.

---

## 3. (b) The applier belt — `MobileGL/MG_Pipe/PipeApply.{h,cpp}`

`PipeApply.cpp:1116` `Bool NoP4aConsumer()` — `g_resourceOps == nullptr` → `++RefusedNoConsumer`,
return true. **Silent by design**: `RefusedResourceCalls` / `RefusedVertexInputCalls` /
`RefusedObjectCalls` each mean "a record named something this applier should have had" and a
non-zero value there is a seam defect; a non-zero value *here* is the designed steady state of a
backend with no P4a twins, so an `MGLOG_E_ONCE` would put an ERROR line in every ordinary Magma
run. The counter is the observable.

**Counter**: `MGPipeApplierState::RefusedNoConsumer` at `PipeApply.h:553`, beside the other three,
zeroed by `MGPipeApplierReset` (`PipeApply.cpp:1152`) like them.

**Fifteen entry points**, each returning `accepted = false` / declining:

| line | entry point | note |
|---|---|---|
| 1590 | `MGPipeApplyResourceCreate` | non-buffer target only — the buffer row is P3a's and its consumer question was already answered by the frontend |
| 1626 | `MGPipeApplyResourceRespecify` | non-buffer target only |
| 1759 | `MGPipeApplyResourceSubData` | the texture half; **the call whose acceptance clears the level flags** |
| 2255 | `MGPipeApplySetFramebufferState` | in front of `RecordAt` — see below |
| 2369 | `MGPipeApplyCreateSamplerState` | |
| 2428 | `MGPipeApplyCreateSamplerView` | |
| 2477 | `MGPipeApplySetTextureParams` | first in the body (it resolves before it validates) |
| 2535 | `MGPipeApplySetSamplerViews` | in front of `ApplyUnitWindow` |
| 2545 | `MGPipeApplyBindSamplerStates` | in front of `ApplyUnitWindow` |
| 2560 | `MGPipeApplySetShaderImages` | in front of `ApplyUnitWindow` |
| 2633 | `MGPipeApplyCreateShaderState` | after the blob rule, the slot bound and the verify round trip |
| 2670 | `MGPipeApplyBindShaderState` | first — the null-handle unbind is behind it too |
| 2727 | `MGPipeApplySetDrawProgram` | first |
| 2742 | `MGPipeApplySetDispatchProgram` | first |
| 2763 | `MGPipeApplySetGlobalConstants` | first (its bounds come out of the resolved program) |

Placement rule, written at `PipeApply.cpp:1108`: **after the record's own shape checks and before
anything moves**, so a malformed record stays `Fatal{ProtocolCorruption}` on every backend — a
trip wire that fires only on some backends is a trip wire nobody can trust. Two documented
exceptions: the three unit-set calls (`ApplyUnitWindow` validates and writes in one step, so the
question has to be asked in front of it), and the five entry points whose first act is a
resolution (asking after would report the designed state as `RefusedObjectCalls`).

**The death paths are deliberately not belted** — `resource_destroy`, `delete_sampler_state`,
`delete_sampler_view`, `delete_shader_state` are idempotent cleanup that must keep working
whatever the registration did, and with no consumer they find no record anyway. Asserted, so a
later commit that adds them has to change that line.

**One known, bounded residue**: for the four create-shaped calls the bound check *is* `RecordAt`,
which grows the table to the slot on its way to answering, so a refused create can leave a zeroed
non-live row behind. It costs nothing where it matters: on a backend with no consumer the client's
gate emits none of these, and the one record that reaches this applier *without* passing that gate
— `set_framebuffer_state`, published by name from `GL_Framebuffer.cpp:650`'s
`PipePublishFramebufferByName`, which calls the framebuffer emitter directly at the fifteen DSA
sites — is declined in front of its `RecordAt`. That direct call is also the reason the belt is not
redundant with the gate.

---

## 4. (c) Tests

| test | file | what it pins |
|---|---|---|
| `TextureEmit.WithNoBackendConsumerTheFamilyGateIsFalseAndNothingReachesTheApplier` | `MG_Test/Pipe/TextureEmitTest.cpp` | the **client** half. All four `MGPipeP4aFamilyEmits` rows false, `kMGPipeSubsystemVertexInput` still true; the handle is still minted but `MGPipeHandleIsPublished` is false, `CreateCount`/`RespecifyCount`/`SubDataCount` 0, `DrainListSize` 0, `TextureResources` empty, **`texture->IsStorageDirty(...)` still true**, and `RefusedNoConsumer == 0` (the *gate*, not the belt, is what stopped it). Second half: with the consumer back, create published, drain list 1, `SubDataCount` 1, `RefusedSubDataCount` 0, flag cleared. |
| `ResourceEmit.EveryP4aFamilyEntryPointDeclinesWhenNoBackendRegisteredTheConsumer` | `MG_Test/Pipe/ResourceEmitTest.cpp` | the **applier** half. All fifteen entry points driven with no table: `RefusedNoConsumer == 15` (the number is the assertion — an entry point added later that forgets the belt fails this line), the other three refusal counters and `StaleFramebufferRecordLookups` all 0, nothing live in any of the five object tables, `FramebufferRecords` empty, the three unit windows 0, the three program bindings null. Then with a table: every one lands, the counter does not move, and the two death paths run uncounted. |

**Fixture change, and why.** The five P4a emit suites and `ResourceEmitTest`'s texture/renderbuffer
case drive the emitter/applier pair as they behave under a backend that *consumes* the records —
the shipped DirectGLES configuration — so they now install the same signal that backend installs:
an empty `static const MGPipeResourceOps` registered in `main()` of `TextureEmitTest.cpp` and
`FramebufferEmitTest.cpp`, and a scoped `ScopedResourceOps` in `ResourceEmitTest.cpp`'s
`TheThreeResourceKindsKeepTheirOwnSlotSpaceAndDoNotSeeEachOther`. The table is empty because none
of its hooks is on a non-buffer path (every non-buffer row is stored and returned, never
dispatched), so what the registration changes is the consumer question and nothing else; neither
file constructs a `BufferObject`. The two cases above take it away again
(`ScopedNoResourceOps` / the un-scoped arm). `SamplerEmitTest`, `ProgramEmitTest`, `ImageEmitTest`
and `CompositeResolverTest` needed nothing. `ResourceEmit.TheResourceOpTableIsUnregisteredUntilA
BackendInstallsOne` is unchanged and still asserts `nullptr` at entry.

### Mutations (run, not committed)

| # | mutation | result |
|---|---|---|
| M1 | `FamilyIsLive` drops `&& P4aFamilyHasItsConsumer(subsystem)` | `TextureEmit.WithNoBackendConsumer...` **RED** (1/1 failed). The 54-case DirectVulkan texture subset stayed **green** — because the belt then caught every record, which is exactly the belt's job. |
| M2 | `NoP4aConsumer()` never fires | `ResourceEmit.EveryP4aFamilyEntryPointDeclines...` **RED** (1/1 failed). |
| M3 | the walk-side conjuncts removed (`wants()` + the drain gate), `FamilyIsLive` kept | DirectVulkan **475/475 green** — reported honestly: with the birth hooks gated, the drain list is empty and the walk-side rows clear no client flag, so those two conjuncts are defence in depth (and the `supplied` one is forward protection), not independently lane-proved. Both are kept because ID-39 names them and because a family that later emits from the walk without a birth hook would otherwise be ungated. |

---

## 5. Gate numbers (all on `~/w7/p4a-c0f` @ `29d51ab9`)

| gate | result |
|---|---|
| builds | `build-linux` rc 0, `build-push` rc 0 |
| **G1 `symbol_report --threshold 0`** vs `~/w7/p4a-before-libMobileGL.so` | **0 added / 0 removed / 0 resized / 0 renamed**; `.text 10806323 -> 10806323 (+0, +0.000%)`; 27811 -> 27811 defined symbols |
| **G5 `scripts/p3a_untouched_regions.sh 37da3c3a HEAD`** | **rc 0** — "byte-identical between 37da3c3a and HEAD" |
| `gen_pipe.py --check` | rc 0 — "generated files are up to date" |
| `gen_pipe_dirty_surface.py --check` | rc 0 |
| `gen_pipe_dirty_surface.py --self-test` | rc 0 — 27 negative controls, all tripped |
| `check_include_closure.py --compiler clang++` | rc 0 — 4 probes, 0 skipped, 0 problems |
| `ctest -L unit` build-linux | **100%, 0 failed of 1762** (was 1761; +1, the ResourceEmit case is a visible SKIP in the pull build) |
| `ctest -L unit` build-push | **100%, 0 failed of 1763** (was 1761; +2) |
| **`ctest --test-dir build-push -L integration-gpu -R DirectVulkan -j 8`** | **100% tests passed, 0 failed out of 475** — was 409/475 (66 failed) on the same tree before the fix, measured here as the baseline |
| `ctest --test-dir build-push -L integration-gpu -R DirectGLES -j 8` | 98%, **8 failed of 491 — the gate's exact eight, unchanged** |

### The DirectGLES eight, unchanged and unchased

Identical set and identical count to `~/w7/p4ag-igpu-push.log` (which reported 74 = 66 DirectVulkan
+ 8 DirectGLES out of 966):

- six `DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.*` (Subprocess aborted) — the 0x1ff
  itest pins, package F's;
- `DirectGLES.SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenASamplerObjectCompletesTheTexture`;
- `DirectGLES.IntegerBorderColorScenario.SamplerParameterIivBorderColourSurvivesToAnIntegerSampler`.

**No DirectGLES case was fixed or broken by c0f**, which is what the design predicts: DirectGLES
registers the table unconditionally at `RegisterBufferBackendOps`, so every predicate answers
exactly what it answered before.

---

## 6. What packages B / C / D / E must know

- **Nothing changes on Espryt.** DirectGLES registers `MGPipeResourceOps` at bring-up,
  unconditionally, so `FamilyIsLive`, `wants()`, the drain gate and all fifteen applier entry
  points behave exactly as they did at `17396216`. Every existing assumption, counter and lane
  number holds.
- **On Magma the client now emits nothing for bits 9/10/11/12.** The legacy pull path runs
  untouched, exactly as on a pull build. Do not write a scenario that expects a P4a record to
  exist on DirectVulkan.
- **B**: `TextureEmit.h`/`FramebufferEmit.h` are unchanged. Your emitters are simply not called on
  a backend without the table; `MGPipeHandleIsPublished` stays false there, so the death helpers
  emit nothing and your self-healing creates (`:576` / `:709`) are what recover if a table appears
  later. `RepublishMask`'s own `MGPipeTextureSubsystemEnabled()` is now a second, weaker gate — it
  is harmless because the `MGPipeHandleIsPublished` test beside it already answers false.
- **B, and this one matters**: `GL_Framebuffer.cpp:650`'s `PipePublishFramebufferByName` is the
  **only** P4a emission in the tree that does not pass through `PipeFill`. On a backend with no
  consumer those Named records reach the applier and are declined by the belt (counted in
  `RefusedNoConsumer`, silent). If you want them gated at the client instead, the gate to add is
  `MGPipeGetResourceOps() != nullptr` in `MGPipeFramebufferSubsystemEnabled()` — your file, your
  call; the belt makes it optional rather than required.
- **C**: `SamplerEmit.h` / `ImageEmit.h` / `ProgramEmit.h` unchanged; the CSO cache is never
  reached on a backend with no consumer because the hooks that call it are gated upstream.
- **D / E**: the applier gains one member, `MGPipeApplierState::RefusedNoConsumer`
  (`PipeApply.h:553`). It is **not** a seam-defect counter — do not fold it into the eight-family
  "zero refusals" census on the integrated tree, which is about `RefusedResourceCalls`,
  `RefusedVertexInputCalls`, `RefusedObjectCalls` and `StaleFramebufferRecordLookups`. On Espryt
  it must read **0**; on Magma a non-zero value is the designed state (framebuffer Named records
  only, per the point above).
- **F**: the P4a A/B arms are unaffected — the conjunct is per-backend, not per-mask, and every
  subsystem-control and dependency-refusal scenario runs on DirectGLES.
- **New test-suite convention**: `TextureEmitTest` and `FramebufferEmitTest` register an empty
  `MGPipeResourceOps` for the whole binary; a case added to either that wants the no-consumer arm
  must scope it off (`ScopedNoResourceOps` exists in `TextureEmitTest.cpp`).
