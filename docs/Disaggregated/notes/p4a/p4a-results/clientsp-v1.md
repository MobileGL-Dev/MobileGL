# P4a package C — `clientsp`. Result, v1

Branch `refs/heads/p4a/clientsp`, four commits on top of `p4a/contract` = `08192d72`. **Not pushed.**
`git diff --stat 08192d72..HEAD`: **11 files, +2426 / −52.** Working tree clean.

| # | hash | message (exact, single line, no attribution) |
|---|---|---|
| c1 | `2700a058` | `[Feat] (Pipe): content-address sampler states, mint one sampler view per texture and push the three unit sets behind their own content hashes` |
| c2 | `384bff48` | `[Feat] (Pipe): publish a program's per-stage SPIR-V and reflection archive as a shader CSO and its default uniform block as global constants` |
| c3 | `7894f9bf` | `[Feat] (Pipe): resolve a program pipeline into one shader CSO out of the reserved composite band and release it exactly once` |
| c4 | `ecd5b2e7` | `[Test] (Pipe): wire the sampler and program subsystems and pin the padding-proof CSO identity, the program-resolved unit sets, the linked-snapshot stage mask and the composite band's single release` |

C.2's steps map one for one; c4's message is C's own (the brief prescribes none for step 4).

---

## 1. Verdict on the hard rules

| rule | result |
|---|---|
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **0 added / 0 removed / 0 renamed / 0 resized.** File bytes and `.text` byte-identical (19114360 / 10806323), 27811 → 27811 defined symbols. rc 0. |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (7 controls tripped) |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0 (27 controls, all tripped); the two UNDECIDED rows are the contract's D6 and are unchanged |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all` | rc 0, 4 probes, 0 skipped, 0 problems, 6 controls tripped |
| `p3a_untouched_regions.sh 37da3c3a HEAD` | rc 0, the eleven byte-identical |
| three builds (pull / push / verify) | all three, rc 0 |
| `ctest -L unit` in all three | **rc 0 in each**, no failures |
| ctest names pull == push | `diff` empty |
| no baseline name removed | `comm -23 ~/w7/p4a-before-ctest-names.txt <names>` empty; **38 added** (13 the contract's, 25 C's) |
| `ctest -L integration-gpu -j 8` on `build-push`, default arm | **966/966**, rc 0 |
| same, `MOBILEGL_PIPE_PUSH=0` | **966/966**, rc 0 |
| same, `MOBILEGL_PIPE_PUSH=0x1ff` (P4a's subsystems off) | **966/966**, rc 0 |
| `ctest -L integration-verify -j 4` on `build-verify` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` | **844/844**, rc 0, `grep -c 'Fatal{'` = **0** |
| `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … -j 4` | §5 |
| only C's files (+ granted) | yes — §4 |
| no push, no other worktree, no attribution lines | yes |

---

## 2. Per-file summary

### `MG_Impl/Pipe/SamplerEmit.h` (+714, the stub replaced)
Four things, in this order.

- **`MGPipeCanonicaliseSamplerParameters(src, out)`** — `memset` to zero, then sixteen field
  assignments in `MGP_FIELDS_SamplerParameters`' order, `borderColorForm` included.
  **It takes an OUT-PARAMETER and does not return a value** (§4 deviation D1).
  `MGPipeHashSamplerParameters(canon)` is XXH64 over those 100 bytes;
  `MGPipeHashOfSamplerParameters(src)` is the two in one step.
- **`MGPipeSamplerCsoCache`**, capacity 256: hash, probe, **memcmp-confirm**, LRU `Evict` that
  emits `delete_sampler_state` and frees the slot, counters
  `{Mints, Acquisitions, Hits, Collisions, Evictions}`, `RecordIsPublished(handle)` and a
  test-only `ResetForTest()`. `MGPipeSamplerCsoCacheInstance()` is never destroyed and is
  **package B's seam** for `MGPTextureParams::BuiltinSampler`.
- **`MGPipeSamplerUniformTextureTarget(GLenum)`** — DirectGLES' `SamplerUniformTextureTarget`
  moved to the side of the boundary that now owns the question, and
  **`MGPipeProgramOpaqueUnits`**, one inversion of the program's opaque-uniform table
  (`unit → sampled target`, plus the highest image unit), memoised on
  `(lifetimeId, linkVersion, backendStateVersion)`. `MGPipeProgramOpaqueUnitsShared()` is one
  instance for the whole family, shared with `ImageEmit.h`.
- **`MGPipeSamplerEmitter`** — `EmitSamplerViews`, `EmitSamplerStates`, `AcquireSamplerView`,
  `RecordIsPublished` / `NoteRecordDestroyed`, `Reset`, `ResetCounters`, and the
  no-copy accessors a unit case reads. `kMGPipeWiredSamplerSubsystem` = `kMGPipeSubsystemSamplers`.

### `MG_Impl/Pipe/ImageEmit.h` (+120, the stub replaced)
`MGPipeShaderImageSetContentHash` and `MGPipeImageEmitter::EmitShaderImages`: the window, the
zero early-out taken **before** any hash, the per-unit `MGPImageView` build carrying the
application's format and access unrecast, `MGPipeEncodeImageAccess` (the three GL names folded
into one byte, with an assert on a fourth), the suppressor slot, `Reset` (the window is
per-context), and the accessors. It rides `kMGPipeWiredSamplerSubsystem`.

### `MG_Impl/Pipe/ProgramEmit.h` (+339, the stub replaced)
`kMGPipeGlobalConstantsNeverUploaded` + `MGPipeGlobalConstantsVersionIsEmittable`,
`MGPipeStageMaskOf` (off `GetLinkedShaderStages()`), and `MGPipeProgramEmitter`:
`EmitShaderState` (both joins, the composite observation, `bind_shader_state`,
`set_draw_program`, `set_dispatch_program`), `EmitGlobalConstants`, `AcquireShaderCso` (the
re-issue on the same handle), `AcquireShaderCsoHandle` (the one place the composite band can be
entered), the **two** latch tables (ordinary and band, never one wide one),
`RecordIsPublished` / `NoteRecordDestroyed`, `Reset`, `ResetCounters`, accessors.
`kMGPipeWiredProgramSubsystem` = `kMGPipeSubsystemPrograms`.

### `MG_Impl/Pipe/CompositeResolver.h` (+194, new)
`MGPipeProgramIsPipelineComposite(program)` (external index 0, Core.cpp:661's own invariant) and
`MGPipeCompositeResolver`: the signature-keyed memo, `Observe`, `Forget`, `Reset`, counters
`{Mints, Reuses, Releases}`. Entries hold a GL name, a signature of plain integers, a handle and
a lifetime id — **no frontend `SharedPtr`**, which is the exit-order rule.
`MGPipeCompositeResolverInstance()` is never destroyed.
**It is included BY `ProgramEmit.h`, not the other way round**, so the resolver is live in the
build that matters rather than only in a test.

### `MG_State/GLState/SamplerState/SamplerObject.cpp` (+17/−4)
The `#if MOBILEGL_PIPE_PUSH` destructor body's bare `NotifyStateObjectDestroyed(SamplerCso, …)`
becomes `MG_Pipe::MGPipeEmitSamplerCsoDestroyAndFree(m_lifetimeId)` — edited, never added
(G1). For a content-addressed CSO the helper correctly frees nothing (§3).

### `MG_State/GLState/ProgramState/ProgramObject.cpp` (+17/−4)
The same edit, `MGPipeEmitShaderCsoDestroyAndFree(m_lifetimeId)`, and it is the one line an
ordinary program and a pipeline composite share. `#include <MG_Pipe/PipeMutation.h>` added.

### `MG_State/GLState/ProgramState/ProgramObject.h` (+17)
One new accessor, **inside `#if MOBILEGL_PIPE_PUSH`**: `const SpirvArtifacts& GetSpirvReflection() const`
— the phase-B twin of `GetLinkReflection()`. `MGPipeApplyCreateShaderState` takes both whole
structs and the class had no public whole-struct SPIR-V accessor (§4 deviation D6).

### Tests
`MG_Test/Pipe/SamplerEmitTest.cpp` (+372, 10 cases), `ImageEmitTest.cpp` (+214, 5),
`ProgramEmitTest.cpp` (+259, 7), `CompositeResolverTest.cpp` (+215, 6). Each keeps the contract
commit's shape-pinning case verbatim, carries the pull-build SKIP list so `ctest -N` stays
name-for-name identical, and uses an RAII scope rather than a `TEST_F` (a fixture would file
every case under the fixture's name and out of the gates' `ctest -R` reach).

---

## 3. The identity rules, as implemented

### Sampler CSO — CONTENT-addressed, capacity 256
Key = XXH64 over a **zero-initialised canonical copy** built field by field, confirmed with a
`memcmp` over the same canonical bytes before any handle is reused. Slots come from
`MGPipeSlots().Allocate(SamplerCso)` **without a lifetime id**, deliberately: the CSO belongs to
a VALUE, not to a frontend object, so `~SamplerObject` must not free it — another live
`SamplerObject` may hold the same value. Its **only** death path is the cache's LRU eviction,
which is client-side and therefore backend-neutral from the first commit.
`static_assert(kMGPipeSamplerCsoCacheCapacity > kMGPipeMaxTextureUnits)` makes "a handle named
in the tail of the record being built now can never be the LRU victim" a build-time property:
one pass acquires at most 192 and touches every one of them.

### Sampler view — IDENTITY-addressed, one per `ITextureObject`
`MGPipeSlots().Acquire(SamplerViewCso, texture.GetLifetimeId())`, `create_sampler_view` re-issued
on the **same handle** whenever the restrictions move (`Gen` moves only on slot reuse).
The version-first skip latches `(GetShapeVersion(), GetTextureParamsVersion(), Gen, textureHandle)`
per slot. The shape version is the load-bearing half — it is what `BumpShapeVersion` moves, i.e.
exactly the inputs `MGPSamplerView` carries; the params version rides beside it belt-and-braces
(no field of the record depends on it, so a wrap of that `Uint16` can only cost a skipped
re-issue of an identical record). The four view restrictions are emitted from
`GetViewMinLevel/NumLevels/MinLayer/NumLayers`, which are `0` for an ordinary texture and written
by `glTextureView` for a view — so **zero unambiguously means "unrestricted"** and there is no
second spelling of that case to disagree with the first.

### Shader CSO — IDENTITY-addressed, one per `ProgramObject`
`Acquire(ShaderCso, program.GetLifetimeId())` for an ordinary program;
`AllocateComposite(lifetimeId)` for a composite. `create_shader_state` re-issued on the same
handle whenever `GetLinkVersion()` moves; the latch is `(RecordLive, RecordGen, LinkVersion)`.
All seven `MGPBlobRef`s declare `Seg = kMGHostSpanSegNone, Size = 0` (the one Blob rule) and the
artefacts ride beside the record through the entry point's companion pointers — **the codec is
never called on the monolith path**. `Offset` carries the staging address, diagnostics only.

### Composite — the band, and exactly one free
Minted off the composite `ProgramObject`'s **own** lifetime id out of
`[kMGPipeShaderCsoCompositeSlotBase, kMGPipeShaderCsoSlotLimit)` through the allocator's one
door. Two release paths, both through `MGPipeEmitShaderCsoDestroyAndFree`: the resolver's
signature-move release, and the composite's own `~ProgramObject`. Whichever runs second is a
proven no-op (`Free` refuses a slot that is not live at that generation and bumps no generation
of its own). Both orders are pinned.
**Two pipelines with the same signature do NOT share one handle** — see §4 deviation D4.

### The two unit sets and the image set
`set_sampler_views` is the **program-resolved** set (`Start = 0`,
`Count = GetMaxTouchedTextureUnit() + 1` clamped to 192); a unit the program does not sample, a
unit with nothing bound, an undefined default texture and a texture that
`SamplesAsIncompleteTexture` all carry the null view — which is the resolution DirectGLES
performs by leaving the native target unbound. `bind_sampler_states` is deliberately **not**
program-resolved (a sampler object is bound to a UNIT and applies to whatever it holds), which
is `BindCurrentUnitSamplers`' shape. All three hash field-wise over zero-initialised staging
arrays with `Start` and `Count` mixed in, and all three go through their `SetHashSuppressor` slot.

---

## 4. `EmittedCallSuppliesTheWholeField` — the decisions, and why none of them moved

All six P4a rows were placed in the **false** arm by the contract commit. C's implementation
confirms every one of them for its own families, so `PipeFill.cpp` (A's for the phase) needed no
edit and no field stopped being pulled:

| field | verdict | reason as implemented |
|---|---|---|
| `GetProgramForDraw` | **false** | pointer-valued. The emitted call carries a `{slot, gen}` HANDLE, not the `SharedPtr<ProgramObject>` the field returns — exactly `GetBoundVertexArray`'s reason. |
| `GetProgramForDispatch` | **false** | the same, and it is a second, independent slot. |
| `GetTextureUnitObject` | **false** | the field hands back a live `TextureUnit&` with one binding slot per target; `set_sampler_views` carries the RESOLVED single view per unit, which is strictly less. |
| `GetMaxTouchedTextureUnit` | **false** | the applier's `Count` can lag the frontend's mark by exactly the case the suppressor exists to swallow: a redundant re-bind moves `GetTextureBindGeneration()` and the mark, and the resolved set's hash does not move, so nothing is sent. |
| `GetImageTextureBinding` | **false** | the field is a live `ImageTextureBinding&` including its `SharedPtr`; the record carries a handle plus an access byte. And C's window is program-derived (§4 D7), so a unit outside it is not described at all. |
| `GetFramebufferBindingSlot` | **false** | package B's, untouched here. |

---

## 5. Verification transcript

```
$ git log --oneline -5
ecd5b2e7 [Test] (Pipe): wire the sampler and program subsystems and pin the padding-proof …
7894f9bf [Feat] (Pipe): resolve a program pipeline into one shader CSO out of the reserved …
384bff48 [Feat] (Pipe): publish a program's per-stage SPIR-V and reflection archive …
2700a058 [Feat] (Pipe): content-address sampler states, mint one sampler view per texture …
08192d72 [Feat] (Pipe): land the P4a contract - …

$ for d in build-linux build-push build-verify; do cmake --build $d -j 12; done        all rc=0

$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
| before | ~/w7/p4a-before-libMobileGL.so | 19114360 | 10806323 |
| after  | build-linux/libMobileGL.so     | 19114360 | 10806323 |
.text 10806323 -> 10806323 (+0, +0.000%). 27811 -> 27811 defined symbols:
0 added, 0 removed, 0 resized, 0 renamed, 27072 unchanged.                            rc=0

$ python3 scripts/gen_pipe.py --check                                                  rc=0
gen_pipe: 71 calls (11 screen, 60 context), 72 verify payloads, 63 PipeInputs fields, 69 verbs
gen_pipe: inventory 477 rows: ... 0 UNMAPPED; generated files are up to date
$ python3 scripts/gen_pipe.py --self-test        7 negative-control trip(s), positive OK  rc=0
$ git diff --exit-code -- MobileGL/MG_Pipe/generated                                    rc=0

$ python3 scripts/gen_pipe_dirty_surface.py --check                                     rc=0
dirty-surface: 79 mutators, all mapped, no stale rows; 45 render-state answers derived
               ... 0 COARSE; 2 UNDECIDED (the contract's D6, unchanged); 26 prose answers
$ python3 scripts/gen_pipe_dirty_surface.py --self-test   27 negative controls, all tripped rc=0

$ python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all
include-closure: 4 probes, 0 skipped, 0 problem(s); self-test 6 negative-control trip(s)  rc=0

$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
[p3a-untouched] the 11 pool / deferred-release / ring / flush-drain functions are
[p3a-untouched] byte-identical between 37da3c3a and HEAD                                rc=0

$ for d in build-linux build-push build-verify; do ctest --test-dir $d -L unit --no-tests=error -j 12; done
                                                                            all rc=0, 0 failed
$ diff <(names build-linux) <(names build-push)                                         (empty)
$ comm -23 ~/w7/p4a-before-ctest-names.txt <(names build-linux)                         (empty)
$ comm -13 ~/w7/p4a-before-ctest-names.txt <(names build-linux)                    38 added

$ ctest --test-dir build-push -R 'SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.' \
        --no-tests=error --output-on-failure                     100% passed, 29/29     rc=0

$ ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8
100% tests passed, 0 tests failed out of 966                                            rc=0
$ MOBILEGL_PIPE_PUSH=0     ctest --test-dir build-push -L integration-gpu -j 8
100% tests passed, 0 tests failed out of 966                                            rc=0
$ MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu -j 8
100% tests passed, 0 tests failed out of 966                                            rc=0

$ GLIBC_TUNABLES=glibc.malloc.tcache_count=0 \
      ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4
100% tests passed, 0 tests failed out of 844 ; grep -c 'Fatal{' = 0                     rc=0
```

```
$ MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p4a-clientsp \
      --lib ~/w7/p4a-clientsp/build-verify/libMobileGL.so --out ~/w7/retrace-out/p4a-clientsp -j 4
79 tests selected of 79
...
passed 79 / 79; failed: []
retrace_gate rc=0
$ grep -l 'Fatal{'         ~/w7/retrace-out/p4a-clientsp/*.log        (empty)
$ grep -L 'MGPipe verify:' ~/w7/retrace-out/p4a-clientsp/*.log        (empty - all 79 armed)
$ rm -rf ~/w7/retrace-out/p4a-clientsp
```

**The first attempt of this run reported `passed 2 / 79` with `ssim=None` on every Minecraft
case.** That is the LFS-pointer false red, not a regression — see deviation D11.

---

## 6. Granted sites

**None used.** C.7 grants C `MG_Impl/GLImpl/Program/GL_Program.cpp` ("nothing today —
`create_shader_state` is emitted from the validate point"); that stayed true and the file is
untouched. `MG_State/GLState/Core.{h,cpp}`, which C.7 gives to C after the tag, is **also
untouched** (deviation D5).

Every file C edited is one C.7 names as C's: `MG_Impl/Pipe/{SamplerEmit,ImageEmit,ProgramEmit,
CompositeResolver}.h`, `MG_State/GLState/{SamplerState,ProgramState}/**`, and
`MG_Test/Pipe/{SamplerEmit,ImageEmit,ProgramEmit,CompositeResolver}Test.cpp`.
Nothing under `MG_Backend/`, `MG_IntegrationTest/`, `.github/`, `scripts/`, `MG_Pipe/`,
`MG_Impl/Pipe/{PipeFill,Tracker,SetHashSuppressor,SlotAllocator,CsoCache,ResourceTracker,
VertexInputEmit}`, or any of B's `MG_State` directories.

---

## 7. Deviations, each with its reason

**D1 — the canonicaliser takes an out-parameter, and the cache stores its bytes with `memcpy`.**
D-F1 says "hashed with XXH64 over the copy". A copy is not enough: a copy of a trivially
copyable type leaves the padding **unspecified**, so a canonicaliser that returned by value
would hand its caller three bytes of whatever the copy left there, and `Entry::Params = canon`
would leave the cache's own stored bytes with different padding from the probe's.
**This is not theoretical — it was the bug.** With member-wise assignment the first probe's
`memcmp` rejected the cache's own entry, counted a collision that never happened, evicted and
minted a fresh CSO: a 256-entry cache with a hit rate of zero, whose failure depends on heap
contents, so `TwoIdenticalSamplersShareOneCso` **passed run alone and failed run in a suite**.
Both are now `memset`/`memcpy` and `PaddingCannotChangeTheHash` covers the hash and the confirm.

**D2 — [FINDING] `kMGPipeWired*Subsystem` is NOT the emission gate, so c1–c3 are not inert.**
The validate point's `wants()` (`PipeFill.cpp`) asks `MGPipeSubsystemForDirty(bit)` and the
runtime `MOBILEGL_PIPE_PUSH` mask **and nothing else**; only the texture drain additionally
tests `kMGPipeWiredSubsystems`. So the moment an emitter has a body it runs at the phase default
mask `0x1fff`, whatever its family constant says. The tree is still behaviourally unchanged —
every applier entry point is a stub on this branch — but C.1's b1 wording ("the emission is
proven semantically free before anything is switched over") does not hold for the framebuffer,
sampler, image or program families as `PipeFill.cpp` is written, and **package B inherits the
same fact**. It surfaced as a real test symptom: a `glBindImageTexture` reaches the validate
point and emits the set, so a direct `EmitShaderImages` afterwards is correctly suppressed
(deviation D14). Recorded rather than fixed, because `PipeFill.cpp` is A's for the phase.

**D3 — [FINDING] the six death helpers hardcode `published = false`, so no wire delete can go
out from a frontend destructor.** `PipeFill.cpp:899-980` reads
`const Bool published = false; // the … emitter publishes nothing yet` for all six kinds, and
that file is A's for the entire phase (ID-9 D1: "never touch `PipeFill.cpp`"). C therefore
cannot make `delete_sampler_view` or `delete_shader_state` go out at object death.
**Impact is bounded and no slot leaks**: `NotifyAndFree` still frees the slot, so `HighWater`
and the leak cases are unaffected; what stays behind after `wire` lands is a `Live` applier
record on a freed slot, which no live handle can reach and which a create on the recycled slot
starts over. **The fix is one line per helper and C has already provided the other half**: both
emitters expose `RecordIsPublished(handle)` and `NoteRecordDestroyed(handle)` with P3a's exact
signature, so each body becomes
`const Bool published = emitter.RecordIsPublished(handle); if (published) { …ApplyDelete…; emitter.NoteRecordDestroyed(handle); }`.
`SamplerCso` needs no fix at all: it is content-addressed, has no per-object slot, and its
delete rides the cache's own LRU eviction.

**D4 — `TwoPipelinesWithTheSameSignatureShareOneComposite` is implemented and named
`TwoPipelinesWithTheSameSignatureKeepTheirOwnComposite`.** Sharing one handle between two
composites is not safely implementable and the property that is true is the opposite one. A
composite is an ordinary `ProgramObject` with its own lifetime id, and the death helper resolves
the handle **from** that id; two composites sharing one handle would put only one id in the
allocator's map, so the first `~ProgramObject` would free a slot the second still names — a
premature free that reappears later as slot theft, which is the class the reserved band exists
to prevent. The frontend does not share either: each `ProgramPipelineObject` carries its own
one-slot draw-program cache. The case asserts both handles are distinct, both are in the band
and both stay live; the reason is written into the case.

**D5 — no `Core.{h,cpp}` hook; the composite is identified by `GetExternalIndex() == 0`.**
C.2 grants `Core.{h,cpp}` for "the hook the composite resolver observes". None is needed:
`Core.cpp:661` constructs the composite as `MakeShared<ProgramObject>(0u)` **deliberately**, so
that it is not a named program, does not answer `glIsProgram` and consumes no name — and
`glCreateProgram` never returns 0. A `Bool` member on `ProgramObject` would resize the pull
build's object and break G1 outright. `CompositeResolverTest.ACompositeIsMintedFromTheReservedBand`
pins the discriminator against a really flattened pipeline.

**D6 — `ProgramObject.h` gains one push-guarded accessor.** `MGPipeApplyCreateShaderState` takes
`const SpirvArtifacts*`, and the class exposes only granular SPIR-V getters — no public
whole-struct accessor, and `reservedNumSamplesOffset` has no getter at all.
`GetSpirvReflection()` is the phase-B twin of the existing `GetLinkReflection()`, inside
`#if MOBILEGL_PIPE_PUSH` so a pull build's symbol set and this class's layout are untouched
(G1 confirms: 0 resized).

**D7 — the image-unit window is derived from the program, because the frontend has no
image-unit high-water mark.** D-G2 says `Count` is "the image-unit high-water mark + 1", and
D-G4 property 1 is the zero early-out. `g_imageUnitHighWaterMark` is **DirectGLES'**, written
from inside its own per-unit sync; `TextureState::NoteUnitTouched` is the TEXTURE-unit path and
`glBindImageTexture` does not reach it. Adding a counter to `TextureState` would edit package
B's file **and** resize the pull build's object. So the window is
`max(highest image unit the current program names + 1, the emitter's own sticky window)`, taken
from the shared opaque-unit inversion. A program with no image uniform gives 0 and the set is
not emitted at all — which is property 1's intent exactly ("one integer test for a feature it
does not use"), and it costs no 192-entry walk to reach. The window is sticky so a program that
stops naming a unit does not silently stop describing it.
`ImageEmit.AZeroHighWaterMarkEmitsNothingWithoutHashing` pins it **with an image really bound**.

**D8 — the unit→target resolution is INVERTED once per program, not searched per unit.**
DirectGLES asks it the other way round (`sampledTargetForUnit` walks every uniform location for
one unit) and gets away with it because it only asks on an aliasing conflict. Per unit per verb
that would be O(units × locations), which is the per-draw cost the phase's budget forbids.
`MGPipeProgramOpaqueUnits` walks the locations once and memoises on
`(lifetimeId, linkVersion, backendStateVersion)` — the third being the counter
`SetUniformSamplerOrImageUnitIndex` bumps, i.e. the one thing that moves a uniform's unit
without a relink. First writer wins on a shared unit, which is DirectGLES' arbitration read
forwards. **One instance shared by the sampler and image emitters**, so the two halves cannot
be resolved against two different readings of one program.

**D9 — the sampler CSO cache's capacity is a `static_assert`ed safety bound, not only a size.**
`static_assert(kMGPipeSamplerCsoCacheCapacity > kMGPipeMaxTextureUnits)`.

**D10 — nothing in C's `Reset()` paths touches an object record.** D-J4 is obeyed literally:
`MGPipeSamplerEmitter::Reset()` invalidates only the per-context opaque-unit memo,
`MGPipeImageEmitter::Reset()` only the window, `MGPipeProgramEmitter::Reset()` only the three
bound-handle mirrors and the global-constants key, and the CSO cache and the two record halves
(`RecordIsPublished`) are **not** reset — re-publishing a record the applier still holds would
move its `Serial` for nothing. The cache's `ResetForTest()` is named for what it is.

**D11 — the trace fixtures in this worktree were LFS pointers.** The integrator's 12:24 reset
to the tag pulled `tools/trace_replay/fixtures` back to 131/133-byte pointers, and the first
retrace run reported `passed 2 / 79` with `ssim=None` — the exact false red `MEASUREMENTS.md:413`
names. Restored from `~/w7/pipe/tools/trace_replay/fixtures` (7.7 MB → 803 MB) and re-hidden with
`git ls-files … | xargs git update-index --assume-unchanged`; `git status` clean, 0 files under
1 KB remain. **Every other P4a worktree that was reset at 12:24 is in the same state and must be
checked before its retrace verdict is believed.**

**D12 — `~/w7/p4a-trees2.log` never received `REBUILT clientsp`.** The file was created empty at
12:25 and no build, cmake or `wsl_tree.sh` process was ever running. Before the first build C
confirmed no build process was active in the shared trees and built `build-{linux,push,verify}`
itself; `build-linux` needed 2 targets, so the 11:32 build was already valid.

**D13 — six test names beyond C.2's list, additions only** (G14 is about removals):
`SamplerEmit.AHashCollisionDoesNotAliasTwoSamplerStates`,
`ImageEmit.TheApplicationsFormatAndAccessTravelUnrecast`,
`ProgramEmit.{TheDrawAndDispatchProgramsAreTwoIndependentSlots, AnUnchangedProgramEmitsNothingAtAll}`,
`CompositeResolver.{ASignatureThatHasNotMovedReusesOneComposite, DestructionThenEvictionFreesTheSlotExactlyOnce}`.

**D14 — cases assert on the emitter's counters, not on a direct call's return value**, wherever
a GL entry point in the fixture already reached the validate point and emitted (D2).
`SamplerEmitTest` also shares ONE process-wide `MobileGL::Initialize()` context rather than
`VertexInputEmitTest`'s per-case fresh `GLContext`, because half its cases need a really linked
program and glslang lives behind that call; each case uses GL names of its own.

**D15 — `ProgramEmit.TheNeverUploadedSentinelIsNeverEmitted` pins the PREDICATE.** Driving
`GetUBOContentVersion()` to `~0u` takes four billion `MarkUBOContentDirty` calls, which is not a
test. `MGPipeGlobalConstantsVersionIsEmittable` is what `EmitGlobalConstants` consults, so
pinning it pins the behaviour and deleting the guard is a compile error in the case; the case
additionally asserts that the live path's last record carries an emittable version.

---

## 8. What must be re-run after the rebase onto `wire` + `clientfb`

Ordered by risk, highest first.

1. **`MGPTextureParams::BuiltinSampler` must be non-null on every `set_texture_params`** — the
   applier makes a null there `Fatal{ProtocolCorruption}`. Package B's `TextureEmit.h` gets that
   handle from **C's seam**, `MGPipeSamplerCsoCacheInstance().Acquire(texture.GetSamplerObject()->GetAllSamplerParameters(), bytes)`
   in `MG_Impl/Pipe/SamplerEmit.h`. If B minted it any other way, or not at all, every texture
   will trip that Fatal the moment `wire` lands. **Check this before anything else.**
2. **Deviation D3's one-line fill-in** in each of the six `PipeFill.cpp` death helpers, then
   re-run `ctest -R 'HandleRecycle'` and G8b's leak cases: `delete_sampler_view` and
   `delete_shader_state` only start going out once `published` is asked rather than assumed.
3. `ctest --test-dir build-push -R 'SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.'`
   — on this branch every applier entry point is a stub, so **no case has yet exercised a real
   applier**: the `Start + Count` bounds gate, the record lifecycle, the refusal counters and the
   verify build's program-archive round trip in `MGPipeApplyCreateShaderState` all come alive
   with `wire`.
4. The full transcript of §5, in order — three builds, G1, the three generators, the closure
   probe, `p3a_untouched_regions.sh`, unit ×3, the name parity pair, the three integration-gpu
   arms, integration-verify under `GLIBC_TUNABLES`, and the retrace gate.
5. `MOBILEGL_PIPE_PUSH=0x9ff` (samplers set, texture resources clear) once package D's
   `ResolveSamplerSubsystemArm` exists — C emits `MGPBoundView::Texture` and `MGPImageView::Res`
   as `Texture` handles unconditionally, which is precisely the arm D-K2 says must be refused
   and logged rather than half-run.
6. The retrace corpus, **after checking the fixtures are not LFS pointers** (D11).
