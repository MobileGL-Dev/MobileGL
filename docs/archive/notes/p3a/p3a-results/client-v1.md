# P3a package B, commits b1..b4 — the client (`client-v1`)

Branch `p3a/client`, worktree `/home/swung/w7/p3a-client`, based on `39722687` (tag `p3a/contract`, ID-1's
base ref). Not pushed. Four commits, single-line messages, no bodies, no attribution lines.

| step | hash | message |
|---|---|---|
| b1 | `73bab3d6` | `[Feat] (Pipe, State): mint a {slot, gen} handle for every buffer object and publish its create, respecify, sub-data, flush, readback and destroy as pipe calls` |
| b2 | `1ae6b4f4` | `[Feat] (Pipe): push the bound VAO's format, its vertex buffers with an explicit baseInstance, and its index binding as their own calls` |
| b3 | `71805d59` | `[Feat] (Pipe): widen the dirty-surface scan to MG_State so the MGP_NOTE_MUTATION sites stop being outside the gate` |
| b4 | `a0ac79cc` | `[Test] (Pipe): pin the buffer and vertex-input emitters - every attribute field survives the wire, a bare baseInstance change still emits, and the index-buffer bit ignores unrelated writes` |

**b1's safety property holds as specified**: nothing in the tree registers `MGPipeResourceOps`, so
`MGPipeResourceSubsystemEnabled()` is false in every build, every buffer dispatch falls through to
`g_bufferBackendOps`, and the tree behaves exactly as it did. G1 is 0/0/0/0 after each of the four.

---

## 1. Per-file summary

10 files, +1613 / -45. Every one is in C.1's table except the three marked **[outside C.1]**, each of which is
a required consequence and is argued in §4.

| file | what changed |
|---|---|
| `MG_Impl/Pipe/ResourceTracker.h` | **new.** D-A3's twelve `MGPipeBindBit`s and the `constexpr BufferTarget -> mask` table with `MGPipeEveryBufferTargetIsMapped()`'s completeness `static_assert` (no `default:` arm, so a new enumerator is a build break); the two buffer discriminators; the pure builders `MGPipeBuildResourceDesc` / `MGPipeBuildSubDataRecord`; the record-range walk `MGPipeForEachSubDataRecordRange` with its cap parameter; `MGPipeResourceTracker` (handle mint, the `slot -> BufferObject*` inverse with its `Gen` compare, the sticky bind mask with its epoch guard, the four emission observables); D-D's client callbacks `MGPipeClientOnBufferWriteback` / `MGPipeClientOnGpuWritten` and their one-shot installer. Header-only, push-only. |
| `MG_Impl/Pipe/VertexInputEmit.h` | **new.** D-G2's two pure wire conversions; D-H2.3's `MGPipeVertexBufferSetContentHash` (Start, Count **and BaseInstance** mixed in); `MGPipeVertexInputEmitter` with D-G3's three arms, the **per-handle** `Latch{Published, Gen, ConfigVersion}` table, D-H3's `MGPVertexBuffer[]` build, D-I's index record, the staging buffers that double as the emitted-record observable at no copy, and `Reset()` for a fresh context. Header-only, push-only. |
| `MG_Impl/Pipe/PipeFill.{h,cpp}` | the three stub emitters get bodies (no call site moved); the nine `MGPipeEmitResource*` definitions plus `MGPipeResourceSubsystemEnabled` / `MGPipeResourceOpsHaveSubDataResident` / `MGPipeMintResourceHandle` / `MGPipeEmitResourceDestroyAndFree`; `MGPipeSetPendingBaseInstance` + `MGPipePendingBaseInstance` and the **two** clears (the validate point consumes it after emission, `MGPipeLeaveVerb` clears it too); the emitter's `Reset()` on `FreshlyPrimed`; `kMGPipeWiredSubsystems` gains bits 7 and 8 (§3); `EmittedCallSuppliesTheWholeField` gains `GetBoundVertexArray` (§2). |
| `MG_Impl/Pipe/Tracker.h` | D-M's three `MGPipeSubsystemForDirty` arms (bits 5, 9, 10 -> `kMGPipeSubsystemVertexInput`); D-I's **narrowed** bit-10 shutter (the bound VAO's own element-slot version, widened, mixed with the bound object's lifetime id and the VAO identity) and its `MGPipeWidenedCounter`; bit 9 gains the pending base instance (§4 B-DEV-3); `SetPendingBaseInstance` / `PendingBaseInstance` / `ClearPendingBaseInstance` and their `Reset()`; `kMGPipeDirtyEmittedAtP3a`; the `:64-78` comment corrected (bits 9/10 are P3a) and the composite-shutter note re-scoped now that three composed bits are load-bearing. |
| `MG_Impl/Pipe/SetHashSuppressor.h` | comment only: slot `SetVertexBuffers` marked wired at P3a, plus the requirement a wired slot puts on its hash (why `BaseInstance` is in it). |
| `MG_Pipe/PipeMutation.h` | **declarations only**, beside the two notices: the eleven resource entry points and a forward declaration of `MG_State::GLState::BufferObject`. Why here rather than an `MG_Impl` include from `MG_State`: §4 B-DEV-1. |
| `MG_State/GLState/BufferState/BufferObject.cpp` | D-A1's dispatch branches at all nine op sites (`OnDestroy`, `Respecify`, `SubData`, `FlushMappedRange`, `ResidentSubData`, `ReadbackFromGpu`, and `AcquirePersistentMap` at its three call sites) plus the `ResidentSubData` **nullability probe** in `FillSubData`; `ResourceCreate` + the handle mint from the constructor; `ResourceDestroy` then `MGPipeSlots().Free` from `~BufferObject`, in that order. Every edit inside `#if MOBILEGL_PIPE_PUSH`, and the pull build's text is byte-for-byte what it was (G1 proves it). |
| `MG_Pipe/DirtySurface.def` | two new rows the widened scan found (§4 B-DEV-4); the header's "scans MG_Impl/GLImpl" corrected to both roots and both mechanisms; blind spot 3 marked CLOSED; `MarkBufferObjectForDeletion` / `MarkVertexArrayForDeletion` gain the comment naming the call that makes each true. |
| `scripts/gen_pipe_dirty_surface.py` | G9: `SCAN_ROOT` -> `SCAN_ROOTS` (`MG_Impl/GLImpl` + `MG_State/GLState`), `NOTE_MUTATION_RE` and the enclosing-function attribution, `scan_all` / `main`'s existence check / the report header updated. `--check`, `--self-test` and `--summary` all still work; the 21 canned negative controls all still trip. |
| `MG_Pipe/FillPoints.def` | comment only: the `MOBILEGL_PIPE_POISON_OMIT` sweep's verdict lands with the G15 caselist run, every row stays in place. |
| **[outside C.1]** `MG_Test/Pipe/TrackerTest.cpp` | `OnlyTheFiveEmittedBitsNameASubsystem` follows `kMGPipeDirtyEmittedAtP3a` instead of `...AtP2` (a mandatory consequence of D-M — see §4 B-DEV-5), **name unchanged**; the two `TheIndexBufferBit*` cases C.1's list asks for, plus their rows in the pull-skip list. |
| `MG_Test/Pipe/ResourceEmitTest.cpp` | four cases appended, disjoint from the contract commit's one. |
| `MG_Test/Pipe/VertexInputEmitTest.cpp` | seven cases appended, disjoint from the contract commit's one. |

---

## 2. `EmittedCallSuppliesTheWholeField`, field by field

The predicate answers "may the residual fill stop pulling this field, because an emitted call supplies it
completely". It is consulted only for a field whose emitter's subsystem is in `kMGPipeWiredSubsystems`, so
P3a's decision set is exactly the emitted-list rows whose emitter names one of the two new subsystems.

| field | emitter | decision | reason |
|---|---|---|---|
| `GetBoundVertexArray` | `BindVertexElements` | **false — keep pulling** | The storage is a `SharedPtr<VertexArrayObject>`, a frontend heap reference. The call carries an eight-byte `{slot, gen}` and nothing else, and the applier stores it in `MGPipeApplierState::BoundVertexElements`. It has no way to produce the pointer and P3a deliberately does not give it one — a payload never contains a pointer, and the whole point of the conversion is that the server stops holding frontend references. Skipping the pull would leave `m_boundVertexArray` **null on every draw of every push build**: not subtle staleness, every backend read of the bound VAO reading nothing. The row is therefore *emitted-and-still-pulled*, exactly like `GetPixelStoreParameters`. What retires the pull is not a better applier, it is P8, where the backend stops reading a frontend VAO at all. `Coverage.def`'s note asks for this decision to be taken here deliberately rather than inherited from the row's presence; it is, and the reasoning above is written into `PipeFill.cpp` beside the arm. |
| *(no other field)* | — | — | `MGP_COVERAGE_EMITTED_LIST` gained exactly one row at `c0` (`C4`). `GetBufferBindingSlot` is deliberately **not** in the emitted list (five of its targets are still pulled), and the resource family has no emitted row at all and cannot have one: its calls are dispatched at the GL call that causes them rather than filled into a `PipeInputs` field. |

The two pre-existing `false` rows (`GetPixelStoreParameters`, `GetCurrentVertexAttribute`) are untouched.

**Consequence, stated so the integrator does not have to derive it:** P3a retires **no** pull. Every field the
residual fill copied at `c0` it still copies. That is the correct answer for this phase and it is also why the
verify lane cannot regress on account of a skipped field — there is none.

---

## 3. The `kMGPipeWiredSubsystems` change

```
c0:  RenderState | PixelPack | PatchState | VertexAttribDefaults
b1:  ... | kMGPipeSubsystemResources
b2:  ... | kMGPipeSubsystemVertexInput
```

Each bit is added by the commit that gives *its own* emitters their bodies, which is what B2 asks for. Both
additions are recorded in place with the reason they change nothing about the fill loop:

- **bit 7, `kMGPipeSubsystemResources`, names no emitted field at all.** `SubsystemForEmitter` can never
  return it — there is no `Coverage.def` emitted row for the resource family and there cannot be one. The bit
  is in the constant so that it states what this build emits for, which is what an operator reading a
  recorded `MOBILEGL_PIPE_PUSH` value has to be able to trust.
- **bit 8, `kMGPipeSubsystemVertexInput`, names exactly one emitted field**, `GetBoundVertexArray`, and §2
  says false for it. So the bit switches the **emission** on and changes nothing about the residual fill.

`MGPipeSubsystemForDirty`'s three new arms make `PipeFill.cpp`'s three conditional pairing `static_assert`s
(B1) hold as the equalities D-M asks for, with **no edit to them** — the contract commit wrote "no later edit
is required, and none is invited", and none was made.

---

## 4. Deviations, each with its reason

### B-DEV-1 — the resource entry points are declared in `MG_Pipe/PipeMutation.h`, not in `ResourceTracker.h`

D-A1 puts the dispatch in `BufferObject.cpp`, which is `MG_State`'s. `MG_State` may not include
`MG_Impl/Pipe/ResourceTracker.h`: that is the layering the `mutation-header` closure probe exists to protect
(*"it may only DECLARE the notice: reaching MG_Impl would pull the fill implementation into the state machine
that calls it"*). So the eleven entry points are **declared** in `PipeMutation.h` — B's file, already included
by `BufferObject.cpp`, and the one header in the tree whose job is exactly this — and **defined** in
`MG_Impl/Pipe/PipeFill.cpp`, the same split `MGP_NOTE_MUTATION` already has. The coupling is one forward
declaration of `BufferObject`; the probe still reports `mutation-header OK, 0 forbidden`.

### B-DEV-2 — the sticky `BindMask` is accumulated by sampling, not ORed at `glBindBuffer`

D-A3 asks for the OR *"at every `glBindBuffer` / `glBindBufferBase` / `glBindBufferRange` / VAO element-slot
bind"*. Those entry points are `MG_Impl/GLImpl/Buffer/GL_Buffer.cpp`'s (`BindBuffer_State`,
`BindBufferBase_State`, `BindBufferRange_State`), which C.5 assigns to **no package** and C.1 does not list for
this one, and hard rule (5) is "only your files". The mask is therefore accumulated by **sampling the
frontend's live binding state at every resource emission** — the only place its value is read — and ORed into
a per-slot sticky field that is never cleared. The scan is skipped unless a binding-slot version moved since
the last one (one `Uint16` load per global target, none per binding point), so a `glBufferSubData` pays a
compare and nothing else.

What this is exactly as good as the specified design for: the GL idiom the bit exists for — bind, then define
or update the store — which is what `glGenBuffers; glBindBuffer; glBufferData` is. It is sticky across
unbinds and across respecifies (both pinned by `ResourceEmit.ABindMaskBitIsStickyAcrossARespecifyThatDoesNotRebind`).

**What it cannot see, and this is handed on rather than hidden:** a bind that happens *after* the buffer's last
storage or content operation and is never followed by another one. Such a buffer would never publish that
bind. The specified design has a narrower version of the same hole (`BindMask` rides only on create and
respecify, so a bind after the last respecify is unpublished there too), but sampling widens it from "a bind
after the last respecify" to "a bind after the last resource emission of any kind". **The fix is three
one-line calls to a `MGPipeNoteBufferBoundAs(target, buffer)` in `GL_Buffer.cpp`'s three `*_State` binders,
and it belongs to whoever owns that file.** Nothing in P3a can observe the difference (the bit is read only
under split), so this is a P8 correctness item, recorded here and in `ResourceTracker.h` beside the function.

### B-DEV-3 — bit 9's shutter gains the pending base instance

D-H4 says *"Keep the shutter; wire the emission; add `BaseInstance` to the hash."* Kept literally, the design
does not work: bit 9's shutter is `MixShutter(anyVaoAttributeGeneration, vaoIdentity)`, and a draw whose only
change is its base instance moves **neither** — so the emitter is never reached, the content hash never gets
the chance to refuse suppression, and the server keeps the previous fetch shift. That is the same
silently-wrong-geometry the backend's `baseInstanceDirty` flag exists to prevent, one level further out, and
it is the under-firing direction `ARCHITECTURE.md 13.2` calls the dangerous one.

So the shutter is `MixShutter(MixShutter(anyVaoAttributeGeneration, vaoIdentity), pendingBaseInstance)`. It
over-fires only on the draws that actually carry a base instance and on the first plain draw after one — which
is exactly the set that has to re-publish. `VertexInputEmit.ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet`
pins the hash half; the shutter half is what makes that half reachable.

Related: the pending value is **consumed and cleared by the validate point** after step 3, as well as by
`MGPipeLeaveVerb`. D-H2.1 names only `MGPipeLeaveVerb`, which nothing in the GL entry points calls — so on its
own it would leave a base-instanced draw's value standing for the next plain draw, which is precisely the
property `DrawParametersScenario.APlainDrawAfterABaseInstancedOneSeesZeroAgain` asserts. Both clears are
present and neither relies on the other.

### B-DEV-4 — what the widened scan actually found, and the two rows it produced

G9 says the scan root is widened "so the four `MGP_NOTE_MUTATION` sites in `TextureState.h` stop being outside
it". Widening the root alone does **not** do that: `MUTATOR_RE` matches `pGLContext->Set*` and finds nothing at
all under `MG_State/GLState` that was not already found elsewhere. So the widening also teaches the scanner to
read `MGP_NOTE_MUTATION` as a publish mechanism of its own, attributed to its **enclosing function** (a notice
carries a field name, not a mutator name). Both halves were load-bearing:

- `BumpSamplingResolutionGeneration` — a mutator with **no row at all**, because every caller of it lives in
  `MG_State/GLState` (`SamplerObject::BumpVersion`, `TextureObjectBase`'s shape bump) and the scan did not
  read that directory. Its sibling `BumpTextureBindGeneration` had a row. Answer: `NEW_SAMPLERS`, derived and
  matching.
- `NoteUnitTouched` — which no `pGLContext->` pattern can ever see, because every texture and sampler bind
  routes *through* it. Answer: `kPulledPartialShutter|NEW_SAMPLER_VIEWS`, derived and matching, because the
  bind generation moves only on the `bindingChanged` arm while the high-water mark has no shutter at all.

`--check` went 73 -> 75 mutators, 8 -> 10 derived bit answers, 0 UNDECIDED, 0 COARSE. No new prose vocabulary
was minted: a `kNoteMutation` answer was drafted and dropped, because the existing `kPulledPartialShutter`
already states what is true and is *checked*, while a new prose word would have been another row no derivation
can back.

### B-DEV-5 — `TrackerTest.cpp` and the phase constant

Mapping bits 5/9/10 makes `TrackerWalk.OnlyTheFiveEmittedBitsNameASubsystem` fail, exactly as the contract
package's `PipeCatalogueTest` had to move `kMGPipeVerifiedPayloadCount` 69 -> 71 (its D2). The case now
compares against a new `kMGPipeDirtyEmittedAtP3a` (Tracker.h, beside the P2 constant, neither edited in place)
and asserts the three new arms by name. **The test name is unchanged** — G14 forbids removing one, and the
property it has always asserted ("a bit names a subsystem if and only if this build emits a call for it") is
the property it still asserts. The reason the name outlives the literal five is written into the case.

`MG_Test/Pipe/TrackerTest.cpp` is not in C.5's table (it is one of the "nobody" files), so no package can
conflict on it.

### B-DEV-6 — the sub-data splitter is a capped walk, and the cap is a parameter

D-A2 says a `false` from `MGPipeSetSubDataBufferRange` means "the emitter must split". With the record's own
bounds that instruction is **unreachable**: a second piece begins at least `2^32-1` bytes past the first, which
is already past the box's `2^31-1` offset cap. So an over-long range is *refused whole* (nothing emitted, one
log line) rather than split, and the walk's chunk cap is a parameter defaulting to the record's size bound —
because the transport segment is the tight one and is where splitting becomes live. The unit case drives the
split at a reachable cap (32 MiB) and the refusals at the record's, so the contiguity and
all-or-nothing properties P5 will rely on are pinned now rather than written under pressure later.

### B-DEV-7 — `kMGPipeResourceTargetBuffer` and the bind bits are spelled client-side

`MGPResourceDesc::Target` documents a discriminator (`Buffer | Tex1D.. | Renderbuffer | TexBuffer`) for which
the contract package minted no enum, and `BindMask` documents twelve bits with no names. Both are spelled in
`ResourceTracker.h` (`kMGPipeResourceTargetBuffer = 0`, since `Buffer` leads the documented list and is what a
zero-initialised record already says; `MGPipeBindBit` in the documented order) with a note that the integrator
moves them beside the fields when a second producer appears — P4a's texture family. **Coordination item: if
`espryt` or `wire` assumes a different value for the buffer arm of `Target`, one of the two has to move, and
the client is the cheaper side.**

### B-DEV-8 — `delete_vertex_elements` is not emitted by this package

D-G1 frees the CSO on the VAO's existing death notice, which `~VertexArrayObject` already raises and
`Managers.cpp`'s `OnFrontendStateObjectDestroyed` already consumes for the `VertexElementsCso` kind. That
consumer is `MG_Backend`'s, so the `delete_vertex_elements` emission and the slot free ride with package C's
`e1`/`e4`. C.1's emitter list agrees (it names only the three). **On this tree, VAO CSO slots are therefore
never freed** — harmless for a test binary, and closed the moment `espryt` lands. Recorded so the integrator
does not read the leak as a client defect.

### B-DEV-9 — the client's reverse-channel callbacks are installed, not just written

D-D says "the client's implementation of that callback". `MGPipeCallbacks` is owned by nobody, so the two
client-side entries (`OnBufferWriteback`, `OnGpuWritten`) are installed from `MGPipeMintResourceHandle`'s
one-shot installer and **only over a null slot** — a backend that installs its own is never overwritten. They
are unreachable until a backend calls them.

---

## 5. Verification transcript

All commands in `/home/swung/w7/p3a-client`, `CCACHE_BASEDIR=/home/swung/w7`, at `a0ac79cc` unless a row says
otherwise. Every gate below was also run after b1, b2 and b3 individually and was green at each.

**G1 — pull build symbol identity**

```
python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0            rc 0
  symbol-report: Total 17209979 -> 17209979 (+0)
  symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
  `.text` 10792739 -> 10792739 (+0, +0.000%)
```

Run three times — after b1, after b2 and after b4 (b3 touches no C++) — clean every time. P3a's
admitted-resize set is empty and stayed empty.

**One resize was found and fixed during b1** rather than admitted: an early edit to `SyncGpuWrites` dropped
`m_size == 0 ||` from the *pull* arm of its guard while moving the check into the push arm, which shrank the
function by 7 bytes and, through inlining, six of its callers. The gate caught it on the first run; the fix
restored the pull text verbatim. It is recorded because it is exactly the failure mode D-K predicts (an edit
that escaped a `#if MOBILEGL_PIPE_PUSH` guard) and because it is the evidence that G1 can go red for the
reason it exists.

> **THE SHARED BASELINE WAS REPLACED MID-ROUND — this needs the integrator's attention, and it is not this
> package's doing.** At the final sweep the same command reads `27811 -> 27799: 0 added, 12 removed, 1
> resized`, and `~/w7/p3a-before-libMobileGL.so` / `~/w7/p3a-before-ctest-names.txt` both have an mtime
> *later* than this package's last build. The twelve "removed" symbols are all
> `MG_Util::SelfTest::*PersistentBufferOrdering*` and the resized one is
> `SelfTest::(anonymous namespace)::kGlesDriverBugProbes` — i.e. the POST probe of `dev@9eae9858`, which is
> **not in P3a's base ref**; the replaced ctest-name file likewise carries
> `PersistentBufferOrderingProbeTest.*` (15 names, `dev@9eae9858`) and
> `LargeArenaAdoptionScenario.Respecified*` (`dev@d7655247`). So the baselines were re-taken from a tree ahead
> of `44c2b5cf`, and a comparison against them says nothing about P3a. This package touches no `MG_Util` file
> and adds no `SelfTest` symbol; its `build-linux/libMobileGL.so` is 19,100,448 bytes, byte-size-identical to
> the pull build of **every** P3a worktree including `p3a-contract`'s at the tag, while the replaced baseline
> is 19,114,360. **The integrator should re-take both baselines at `44c2b5cf` and re-run G1/G14 for every
> package**, or the gate is measuring the wrong thing for all four.

**Generators and closure**

```
python3 scripts/gen_pipe.py --check                                   rc 0  "generated files are up to date"
                                                                            71 calls, 71 verify payloads,
                                                                            63 PipeInputs fields (7 sticky, 34 emitted)
git diff --exit-code -- MobileGL/MG_Pipe/generated                    rc 0  (clean)
python3 scripts/gen_pipe_dirty_surface.py --check                     rc 0  "75 mutators, all mapped, no stale rows;
                                                                            45 render-state answers derived and matching;
                                                                            10 other answers derived; 0 COARSE; 0 UNDECIDED"
python3 scripts/gen_pipe_dirty_surface.py --self-test                 rc 0  "21 negative controls, all tripped;
                                                                            positive controls OK"
python3 scripts/gen_pipe_dirty_surface.py --summary                   rc 0  (report renders; scan roots printed)
python3 scripts/check_include_closure.py                              rc 0  "4 probes, 0 skipped, 0 problem(s)"
                                                                            mutation-header OK 84 headers, 0 forbidden
python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all   rc 0
                                                                            "6 negative-control trip(s), parser checks OK"
```

(`--compiler clang++-20` does not exist on this host; the contract package recorded the same substitution.)

**Three builds** — `cmake --build <dir> --parallel 24`, rc 0 in all three (`build-linux`, `build-push`,
`build-verify`).

**Three unit runs** — `ctest --test-dir <dir> -L unit -j 12 --no-tests=error`:

```
build-linux   100% tests passed, 0 failed out of 1584
build-push    100% tests passed, 0 failed out of 1584
build-verify  100% tests passed, 0 failed out of 1584
```

**Name sets (G2's name half, G14)** — taken against the ORIGINAL baseline, before it was replaced (above).

```
ctest -N names: build-linux 2500, build-push 2500, build-verify 3328
diff <build-linux names> <build-push names>                      -> empty
comm -23 ~/w7/p3a-before-ctest-names.txt <build-linux names>      -> empty   (nothing removed)
comm -13 ...                                                     -> 18 added: the contract's 5, plus
    ResourceEmit.{EveryBufferTargetSetsItsBindMaskBit,
                  ABindMaskBitIsStickyAcrossARespecifyThatDoesNotRebind,
                  ADestroyedBufferReleasesItsSlotAndAStaleHandleResolvesToNothing,
                  AWholeBufferSubDataBeyondTheRecordBoundIsSplitIntoContiguousRecords}
    VertexInputEmit.{EveryAttributeFieldSurvivesTheWireConversion,
                     ABindingModelStrideOfZeroSurvivesAsZero,
                     IsLongAndFloat64TravelSeparately,
                     ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet,
                     AnUnchangedSetWithAnUnchangedBaseInstanceEmitsNothing,
                     RebindingTheSameVaoEmitsABindAndNoCreate,
                     PingPongingBetweenTwoVaosNeverRecreatesEither}
    TrackerWalk.{TheIndexBufferBitDoesNotFireOnAnUnrelatedBufferWrite,
                 TheIndexBufferBitFiresWhenTheSlotVersionWrapsOntoADifferentBuffer}
ctest --test-dir build-push -N -R 'VertexInputEmit\.|ResourceEmit\.'   -> Total Tests: 13
```

The suites are `ResourceEmit` and `VertexInputEmit` and the new cases are plain `TEST`s with an RAII scope,
**not** `TEST_F`: a fixture files its cases under the *fixture's* name, which would put every one of them
outside `ctest -R 'VertexInputEmit\.'` and silently disarm G6 and G7. (This was caught by running the
binaries; the first draft used `TEST_F` and produced `VertexInputEmitFixture.*`.)

**G7's property, verified by hand** (the script itself is package D's): with
`wire.IsBgra = attrib.IsBgra ? 1 : 0;` deleted from `MGPipeBuildVertexAttribWire`,
`VertexInputEmit.EveryAttributeFieldSurvivesTheWireConversion` fails **naming `wire.IsBgra` / `attrib.IsBgra`**
(six occurrences, one per BGRA slot). Reverted and re-run: 8/8 pass. So G6 can go red for the reason it exists
and G7's scripted control has a target that answers by field name.

**The four integration lanes** — all green.

```
ctest --test-dir build-push  -L integration-gpu -j 8                      100% passed, 916 tests
MOBILEGL_PIPE_PUSH=0    ctest --test-dir build-push -L integration-gpu -j 8   green (all-pull arm)
MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu -j 8   green (P3a subsystems off, G12)
ctest --test-dir build-verify -L integration-verify -j 4                     green
```

**A flake worth recording, because a reader of the raw log would otherwise read it as a regression.** The
first `integration-gpu` run — taken while the 79-case retrace was running beside it — failed four cases, all
of the shape `TheXIsActuallyArmedWhenTheEnvironmentPinsItOn`:
`DirectGLES.UnlocatedIoBlocks.*`, `DirectVulkan.PrimGenReroute.*` and both `PointSizeDemotion.*`. Every one of
them asserts by **reading back a shared log file** whose path the lane's ctest `ENVIRONMENT` sets, and
`MG_IntegrationTest/CMakeLists.txt:1040` already records that shape as racing a neighbour's bring-up. Chased
down rather than waved off:

- re-run serially while the retrace was still running: 3 of the 4 passed, 1 (`PrimGenReroute`) still failed;
- that one, run alone: passes at the default mask, at `0x7f`, at `0x1ff`, at `0`, and in the **pull** build —
  i.e. it passes with this package's emission compiled out entirely, so nothing here can be its cause;
- the whole lane re-run **alone**, with no competing load: `100% tests passed, 0 failed out of 916`.

So the four are load-induced races on shared log files, not regressions. The recorded lane result is the
quiet one.

**The retrace gate, verify-armed** — `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree
~/w7/p3a-client --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/p3a-client -j 4`:

```
passed 79 / 79; failed: []
grep -l 'Fatal{'          ~/w7/retrace-out/p3a-client/*.log   -> empty
grep -L 'MGPipe verify:'  ~/w7/retrace-out/p3a-client/*.log   -> empty   (every case armed)
```

This is the strongest single result in this document: with the emission live in a verify build, whose
compare-at-read re-reads **every** field from the live context at **every** backend read, the whole corpus
replays with zero divergence and zero unmigrated reads on both backends. `~/w7/retrace-out/p3a-client` was
deleted afterwards.

**A trap the next package will hit.** This worktree's `tools/trace_replay/fixtures/*` were **Git-LFS pointers**,
not archives, so the first retrace run failed all 79 cases in 0 s with `Unrecognized archive format` — which
looks exactly like a catastrophic regression and is not one. The fix is to copy the real files from
`~/w7/pipe/tools/trace_replay/fixtures/` and `git update-index --assume-unchanged` them, which is what the
tree script normally does; `git status` is clean afterwards and the fixtures are left materialised for the
next package. (Second trap, same run: an inner script written to `/tmp` and launched with `setsid nohup`
vanishes when the WSL VM cycles — `/tmp` is tmpfs. Put the inner script under `~/w7/`.)

---

## 6. Left undone, and what it costs

1. **The three `MGPipeSetPendingBaseInstance` call sites.** D-H2.1 asks for one line immediately before
   `MGP_FILL` at `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp`'s `DrawElementsInstancedBaseVertexBaseInstance`,
   `DrawElementsInstancedBaseInstance` and `DrawArraysInstancedBaseInstance`. That file is outside this
   package's ownership (C.5: nobody; C.1: not listed), so the setter, its consumption, its shutter input, its
   content-hash input and its unit coverage all landed here and **the three call sites are owed**. Until they
   exist the emitted `BaseInstance` is 0 on every draw — which is what the tree does today and is therefore
   behaviour-neutral, but it means package C **must not** delete `g_pendingFetchBaseInstance` before those
   three lines exist, or instanced-array base-instance emulation loses its input. This is the single highest
   priority follow-up in this document. The exact line is `MG_Pipe::MGPipeSetPendingBaseInstance(baseinstance);`
   and it is documented beside the declaration in `MG_Impl/Pipe/PipeFill.h`.
2. **The `GL_Buffer.cpp` bind hook** (B-DEV-2): three one-line `NoteBoundAs` calls that would close the
   sampling window. Invisible in monolith; a P8 correctness item.
3. **`delete_vertex_elements` and the VAO CSO slot free** (B-DEV-8): package C's, by the ownership split.
4. **`map-persistent-roundtrips` reads 0 on this tree**, necessarily: the counter is incremented at the
   `MapPersistent` *emission*, and nothing registers `MGPipeResourceOps`, so the emission never happens. It
   becomes non-zero the moment `espryt` registers the table, which is where G10 is scheduled anyway.
5. **The `MOBILEGL_PIPE_POISON_OMIT` caselist sweep** was declared off the critical path by the task; the
   `FillPoints.def` comment now says the verdict lands with the G15 caselist run and every row stays.

---

## 7. What could NOT be verified on the contract base, and must be re-run after the rebase onto `wire`

The applier's fourteen entry points are **stubs** at `39722687`, so on this tree every call this package emits
is applied to nothing. That is not a gap in the emission — it is the ordering `ID-2` chose — but it means the
following are unproven here and are the integrator's re-run list once `wire` is merged underneath:

1. **Every assertion about an applier record.** The unit cases assert on what the *emitter built and handed
   over* (its own staging buffers, at no copy, plus the four/four emission counters), never on
   `MGPipeApplier()`. After the rebase, `ResourceEmit` and `VertexInputEmit` should gain the mirror-image
   assertions — the stored `MGPipeResourceRecord::Desc`, `MGPipeVertexElementsRecord::Attributes[]`,
   `VertexBuffers[]`, the three `Serial`s — and `VertexInputEmit.EveryAttributeFieldSurvivesTheWireConversion`
   in particular becomes a *round-trip* rather than a conversion check. G6's statement is about what the
   backend reads, and only the merged tree can say that.
2. **The applier's blob-bounds gate and its protocol refusal.** `EmitCreate` sends
   `AttributeCount = BindingPointCount = 32` and a `Blob.Size` of exactly
   `32*24 + 32*16 = 1280`; the case asserts the arithmetic, but nothing on this tree refuses a record that
   disagrees. Re-run once the applier can.
3. **`kNeedsAck`'s per-record predicate on a live path.** `MGPipeResourceRespecifyNeedsAck` is pinned by
   `PipeCatalogue` at `c0`; the emitter reads it only in prose (the monolith ack is `((void)0)`).
4. **The reverse channel end to end.** `MGPipeClientOnBufferWriteback` / `OnGpuWritten` are installed and
   correct by inspection, and unreachable until a backend calls them. Their first real exercise is `espryt`'s
   `e3`.
5. **`map-persistent-roundtrips`** — §6.4.
6. **G12's substance.** `MOBILEGL_PIPE_PUSH=0x7f` versus the default `0x1ff` is green in both arms, but on
   this tree the two arms differ only in whether the *emission* happens; nothing consumes it, so the A/B
   cannot yet show a behavioural difference. `ResourceSubsystemControlScenario` (package D) is what makes it
   prove the switch is not dead.
7. **The `Target` discriminator value** (B-DEV-7) needs one look from `wire`/`espryt`.
8. **G1 and G14 themselves**, because the shared baselines were replaced mid-round (§5). Re-take them at
   `44c2b5cf` and re-run; this package's readings against the original files are in §5.

Nothing in this list is expected to change a line of this package; they are assertions to *add* on the merged
tree, and the emitters are shaped so that adding them is a second `EXPECT` beside an existing one.

---

## 8. Intermediate logs and working state

Deleted: `~/w7/p3a-client-{lanes,retrace,laneA}.log`, their inner scripts under `~/w7/`, and
`~/w7/retrace-out/p3a-client/`. The tree-creation logs (`p3a-client-{submodule,build-linux-*,build-push-*,
build-verify-*}.log`) are the tree script's and were left alone.

`git status` in `/home/swung/w7/p3a-client` is clean at `a0ac79cc`. The trace fixtures are materialised (real
archives, not LFS pointers) and hidden with `git update-index --assume-unchanged`, which is the state the next
run of the retrace gate needs; nothing about them is committed.
