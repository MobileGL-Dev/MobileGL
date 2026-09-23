# P4a package F — `gates`. Result, v1

Four commits on `refs/heads/p4a/gates`, branched from the tag `refs/tags/p4a/contract` = `08192d72`.
**Not pushed.** Working tree clean. 11 files, **+3831 / −57**.

| # | hash | message (exactly as C.5 gives it, single line, no attribution) |
|---|---|---|
| 1 | `0bc9aafa` | `[Test] (Pipe): reproduce the texture, framebuffer, renderbuffer, sampler, view and program handle ABA through public GL and prove the pre-rekey guards are what stops it` |
| 2 | `4f006f3c` | `[Test] (Espryt): assert a glTexParameter on a texture that only ever was an attachment, an image binding or a copy endpoint reaches the driver` |
| 3 | `70868f93` | `[Test] (Pipe): switch P4a's four object subsystems off against the shipping mask and record the texture upload shape the two counters make visible` |
| 4 | `303a9dc8` | `[CI] (Pipe): gate that the unpack ring, the attachment permutation, the depth-stencil sampling core and the format caveat did not move` |

**Commit 3's message is not in C.5** — C.5 names three. `ObjectSubsystemControlScenario` (G12) and
`TextureUploadShapeScenario` (D-D4) are in C.5's file table but belong to none of the three messages,
and putting them under either of the two `[Test]` messages would have made the message describe
something else. **Declared deviation F-1**, below. Every commit builds on its own: the CMake source
list and the lane registrations were staged so that no commit names a scenario file that does not
exist in it (verified by walking each commit's `CMakeLists.txt` against its own tree).

---

## 1. Verdict on the hard rules

| rule | result |
|---|---|
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 renamed / 0 resized**, rc 0. The pull `.so` is byte-for-byte the same size as `$BASE`'s: 19114360 file, 10806323 `.text`. None of this package's files reaches the library. |
| every ctest name at the tag still exists (**G14**) | `comm -23 ~/w7/p4a-before-ctest-names.txt <names at HEAD>` **empty**; **+119 added** |
| ctest names pull == push (**G2**) | `diff` empty, **2707** names in each |
| `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, seventeen regions |
| `bash scripts/p4a_untouched_regions.sh --self-test` | **rc 0**, 3 positive + **4 negative** controls, each tripped and named |
| `bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD` | **rc 0** (and `--self-test` rc 0) |
| `bash scripts/p4a_descriptor_negative_control.sh build-push` | **rc 2**, naming both absent field copies — the contract tree's correct answer |
| `ctest --test-dir build-verify -R 'HandleRecycle\|Leak'` | **180/180 passed, 0 failed**; every non-assertion is a named SKIP |
| `ctest --test-dir build-push -R 'ObjectSubsystemControl\|TextureParamsWithoutASamplerView\|TextureUploadShape'` | **18/18 passed, 0 failed** (8 + 10 across two runs); named SKIPs only |
| `ctest -L unit`, three builds | **1635 / 1635 / 1635**, 0 failed |
| `ctest --test-dir build-push -L integration-gpu -j 6` | **1072/1072 passed, 0 failed** |
| only this package's files | yes — `MG_IntegrationTest/**`, `.github/workflows/test.yml`, `scripts/p4a_*.sh`, and F's one granted DirectVulkan file `Renderer/MagmaPipeArms.h` |
| no push, no other worktree, no attribution lines | yes |

**Not run here, and why:** the retrace sweeps, the verify retrace and the device work are D.3/D.4's
and need `~/w7/pipe`, which package agents never touch (ID-3). `build-bench` does not exist in this
worktree (`wsl_tree.sh` creates `build-linux`, `build-push`, `build-verify`), so C.5's
`cmake --build build-bench && ctest -L benchmark` line could not run; **nothing in this package
touches `MG_Benchmark/`**, so there is nothing for it to have broken — the integrator's own bench
tree covers it.

---

## 2. Per-file summary

| action | file | what |
|---|---|---|
| MODIFY | `MG_IntegrationTest/Harness/PipeSlotPeek.{h,cpp}` | `PipeSlotKind` gains `Texture, Renderbuffer, Framebuffer, SamplerCso, SamplerViewCso, ShaderCso`. `Translate` is now an exhaustive switch with **no `default:`** — a kind added without a decision is a `-Wswitch` warning rather than a row that silently counts `VertexElementsCso` and reports "did not leak" about a kind it never looked at. `ShaderCso` covers ordinary programs **and** band composites, because that is what the allocator has. |
| MODIFY | `MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` | +1057 lines. Four new ABA cases (renderbuffer, sampler object, sampler view, program); the two existing texture/framebuffer cases become real controls; seven `…ReturnTheir…Slots` leak cases; three new gates (`ThisBackendsObjectRekeyHasLanded`, `ObjectAbaControlIsWiredHere`, `SkipUnlessTheObjectHandlePathIsAssertableHere`) and one shared assertion block (`AssertChurnReturnsEverySlot`). |
| MODIFY | `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h` | The **only** DirectVulkan file P4a touches. `MagmaPipeAbaControlDefeatsIdentity` keeps its body; beside it, `MagmaPipeAbaControlCoversKind(MGPipeKind)` states **per kind** what the knob defeats — true for `VertexElementsCso` and `Buffer` (the only two kinds `MagmaPipeIdentityTables` mints), false for P4a's six, with the one change that flips it written out. Inside the file's existing `#if MOBILEGL_PIPE_PUSH`, so the pull build is untouched (G1 0/0/0/0 confirms). |
| CREATE | `MG_IntegrationTest/Scenarios/TextureParamsWithoutASamplerViewScenario.cpp` | G9. D-E3's four cases, DirectGLES only, every setup step through the DSA entry points so no case ever binds its texture to a unit before the observation. |
| CREATE | `MG_IntegrationTest/Scenarios/TextureUploadShapeScenario.cpp` | D-D4, recorded not gated. |
| CREATE | `MG_IntegrationTest/Scenarios/ObjectSubsystemControlScenario.cpp` | G12. `0x1fff` vs `0x1ff`, plus the `0x9ff` dependency refusal. |
| MODIFY | `MG_IntegrationTest/CMakeLists.txt` | +280 lines: three sources, one new probe function, five new capability markers, every `0x1ff` phase pin raised, four new lanes. §4. |
| CREATE | `scripts/p4a_untouched_regions.sh` | G5, seventeen regions across three files. §5. |
| CREATE | `scripts/p4a_descriptor_negative_control.sh` | G7, two field drops. §6. |
| MODIFY | `.github/workflows/test.yml` | `BASELINE` → `37da3c3a`, two new `pipe-gates` rows, three new `-R` alternatives on the push-build control step. §7. |

---

## 3. What each scenario asserts, and how it skips on an unimplemented tree

Every arm that needs a subsystem this tree does not have **SKIPs with a named reason**. No
registration is withheld, no arm vanishes, and no arm asserts something that cannot exist yet
(ID-2). The decision is the BUILD's, through content probes with `CONFIGURE_DEPENDS`, never a
hand-maintained list.

### 3.1 `HandleRecycleScenario` — the six ABA cases (G8)

**On a complete tree** each case builds an ABA through public GL — an object is created, drawn with
for `kWarmupFrames`, unbound, deleted, and immediately replaced at the same GL name with a
byte-identical configuration and different contents — and asserts the pixels come from the
replacement. On the `AbaControl` arm, where the knob defeats the identity half of the memo key, the
**corruption is the assertion**.

| case | what a stale key would produce | observable |
|---|---|---|
| texture (existing, now a control) | the dead texture's texels | sampled colour |
| framebuffer (existing, now a control) | the clear lands in the DEAD framebuffer | **both** attachments read back: FRESH = (replacement green, dead red), STALE = dead turned green. P4a added the dead attachment's red warm-up clear so the corruption has a place to be visible, not just an absence |
| renderbuffer | the dead 4×4 storage under an 8×8 replacement | 8×8 readback all green, plus the framebuffer's completeness |
| sampler object | the dead sampler's border colour | `GL_CLAMP_TO_BORDER` sampled at a constant UV outside [0,1] — one colour for the whole viewport, and the border colour is the parameter with the fewest other paths to the driver |
| sampler view | the dead texture through a LIVE sampler object | a bound sampler object is what makes this a different question from the texture case: the view's key names the texture, and the sampler half deliberately does not move |
| program | the dead program's fragment output | the colour is baked into the source, not a uniform — a uniform would be re-set on the replacement and hide the inheritance |

**On this tree** the six SKIP on the `Handles` arm with *"subsystem not implemented on this tree: …
no source under `MobileGL/MG_Backend/<backend>` names any of
`kMGPipeSubsystem{Framebuffer, TextureResources, Samplers, Programs}`"* — and they PASS on the
`Legacy` arm (asserting today's address/`weak_ptr` guards, which is exactly what "the replacement has
to be at least as strong" means).

### 3.2 The `AbaControl` arm for the six kinds — a finding, not an omission

`armExpectsCorruption` is `ObjectAbaControlIsWiredHere()`, which is
`knob-consumer-for-this-backend AND object-re-key-for-this-backend`. **Both are false on every
backend today, and the reason is structural rather than temporal**:

* `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` has exactly one consumer in the tree,
  `MagmaPipeAbaControlDefeatsIdentity`, and its consumers are Magma's **vertex-input** keys;
* `MagmaPipeIdentityTables` mints `{slot, gen}` for **two** kinds, `VertexElementsCso` and `Buffer`.
  A texture, a framebuffer, a sampler, a view and a program are still reached from their frontend
  objects on that backend, and moving them onto handles is **P7's** (`ROADMAP.md:24`, D-Q);
* on DirectGLES the knob has no consumer at all, which is why the `AbaControl` lanes are
  DirectVulkan-only in the first place.

So **D.2's expectation that the six kinds' `.AbaControl` arms PASS by asserting the corruption cannot
be met on this tree, and no package in P4a owns the file that would change that** (the Espryt knob
consumer would live in `MG_Backend/DirectGLES/SlotTables.h`/`Managers.cpp`, which C.7 gives to
package D; the Magma one is P7). Rather than a lane that asserts a corruption nothing can produce —
a hard red on an always-on `integration-gpu` lane, the exact failure this file's own header records
having had once — the arm **asserts the correct pixels and says, on stdout and in
`RecordProperty("aba_control_wired", 0)`, that it is not a control for that kind yet**. One `if` in
a backend's `GetOrCreate`/`FindByHandle`, the shape `MagmaPipeClaimSlotMemos` already has, flips all
six. **Declared deviation F-2.**

### 3.3 `HandleRecycleScenario` — the seven leak cases (G8b)

Shape: two warm-up rounds, then `kChurn = 48` create/draw/destroy rounds, asserting
`liveAfter == liveBefore`, `highWaterAfter == highWaterBefore` and
`peakLive − liveBefore <= maxInFlight`. `peakLive` is sampled **inside** the round through an
`observe()` callback the round must call while its object is still alive — sampled after the destroy
the third assertion would be vacuous, and it is the one that catches a death path that works but runs
at the wrong time.

Three ways a case declines to assert, all of them named:
1. not the `Handles` arm → the other lanes run `MOBILEGL_PIPE_PUSH=0`, where there is no allocator;
2. `PeekPipeSlot*` returns false → "could not look" is not "did not leak";
3. **nothing of that kind was ever minted** (`highWater == 0 && peakLive == 0 && liveAfter == 0`) →
   *"the client minted no `<kind>` slot at all over 50 rounds"*. This third one is P4a's addition and
   it is what stops six of the seven cases from being `0 == 0` greens before the emitters land.

**Measured on the contract tree, DirectGLES `Handles` lane** (the numbers are in
`p4a-handlerecycle-before-verbose.log`):

| case | reading | verdict |
|---|---|---|
| `DestroyedVertexArraysReturnTheirVertexElementsSlots` (P3a's) | live 1→1 (peak 1), hw 3→3 | pass |
| `DestroyedTexturesReturnTheirTextureSlots` | live 3→3 (peak 4), hw 5→5 | pass — **the client already mints and frees Texture slots at `c0`** |
| `DestroyedRenderbuffersReturnTheirRenderbufferSlots` | live 0→0 (peak 1), hw 2→2 | pass |
| `DestroyedFramebuffersReturnTheirFramebufferSlots` | live 0→0 (peak 1), hw 2→2 | pass |
| `DestroyedSamplersReturnTheirSamplerCsoSlots` | live 0→0 (peak 1), hw 2→2 | pass |
| `DestroyedProgramsReturnTheirShaderCsoSlots` | live 1→1 (peak 1), hw 2→2 | pass |
| `EvictedPipelineCompositesReturnTheirShaderCsoSlots` | live 1→1 (peak 1), hw 2→2 | pass, **partially armed** — see below |
| `DestroyedTexturesReturnTheirSamplerViewSlots` | live 0→0 (peak 0), hw 0→0 | **SKIP**, "the client minted no SamplerViewCso slot at all" |

On the DirectVulkan `Handles` lane every one of the seven SKIPs on the same "nothing minted" rule,
which is the honest reading for a backend whose object kinds are P7's — and it is exactly where the
cases have to run once they arm, because P3a's C-1 was a **Magma** leak (ID-8).

**The composite case is only partially armed today.** `ShaderCso` is minted (so the "nothing minted"
skip does not fire and the case is not vacuous about the *kind*), but no slot comes out of the
composite band yet: `CompositeResolver` is package C's. The case therefore asserts today that
pipeline churn leaks no `ShaderCso`, and asserts the two-release-path property — LRU eviction **and**
the composite `ProgramObject`'s destructor, the pair D-H7 calls out — when C lands. It also depends
on contract review v1's **M5** (`HighWater(ShaderCso)` returning the band top makes the high-water
assertion vacuous); the fix is in flight on `c0b` and this case is one of its consumers.

### 3.4 `TextureParamsWithoutASamplerViewScenario` (G9) — **the mandatory red could not be produced, and the reason is a finding**

The four cases put a texture through exactly one reachability path, move a parameter while it is
reachable only that way (through `glCreateTextures` / `glTextureStorage2D` / `glTextureSubImage2D` /
`glTextureParameteri`, so no setup step ever binds it to a sampler unit), and sample it once at the
end.

**All four are GREEN on `08192d72`** — including
`AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver`, which D-E3 says must be RED.
`p4a-texparams-before.log` is the run. The mechanism, re-opened at the base ref:

* a texture parameter's only public-GL observable is a **sample** — nothing about an attachment, an
  image binding or a copy endpoint reads a swizzle or an aspect mode;
* a sample puts the texture on `SyncNeccessaryTextures`' **unit** list, and that walk calls
  `SyncTextureParamsToBackend` for every entry whose `IsDrawSyncClean` is false
  (`DirectGLES.cpp:1896-1898`);
* `IsDrawSyncClean` is false **whenever the frontend parameter version has moved** since the last
  sync (`Managers.h:1399-1416`: `m_syncedTextureParamsVersion != paramsVersion`).

So the D-E3 gap is real — between the parameter change and the next sampler binding the driver
really is not told — and it is closed by the only thing that can see it. **No public-GL integration
scenario on a monolith tree can produce ROADMAP.md:20's `落地前必须红` artefact for this case.**
Producing it needs an observation of the *driver's* texture object taken while the texture is still
read-attachment-only, i.e. a probe inside `MG_Backend/DirectGLES` — package D's files (C.7), not
this package's.

What the scenario is instead, stated in its own header so nobody reads a green as more than it is:
a **regression net around D10**. It goes red if any of the four reachability paths is ever made to
depend on the texture having a sampler **view** — which is the coupling P4a's resource-addressed
`set_texture_params` removes and a later phase could reintroduce. **The integrator's ruling is
needed** (§9). **Declared deviation F-3.**

### 3.5 `ObjectSubsystemControlScenario` (G12)

Workload: a user framebuffer with a texture attachment (bit 9), a texture with parameters and a
sub-region upload per iteration (bit 10), an explicit sampler object on the unit (bit 11), a program
with a default-uniform write (bit 12) — all four families, four times, in one counted window.

* **On (`0x1fff`)**: `fbe + sve + sse + sie + ctu > 0`, and `fbe > 0` on its own so a large upload
  count cannot hide a framebuffer family that never emitted.
* **Off (`0x1ff`)**: every one of those five is **zero** — the dead-switch reading.
* **Both**: the pixels do not move.
* **Refused (`0x9ff`)**: the library's own log must **name both bits** (the constant's name or its
  hex mask, so the assertion does not pin the wording), and the run must still draw what every other
  lane draws — a refusal runs the LEGACY arm, so the pixels are the one thing it may not change.

**On this tree** the on/off case SKIPs on `MGITEST_PIPE_OBJECT_EMITTER_PRESENT` (no `MG_Impl/Pipe`
source emits `FramebufferEmissions`, so the counters are structurally zero in *both* arms and their
difference is not observable). The refusal case SKIPs on `MGITEST_HANDLE_REKEY_OBJECTS_DirectGLES`:
D-K2's refusal lives in `ResolveSamplersSubsystemArm()`, package D's file, and a backend that does
not honour P4a's mask cannot refuse a dependency inside it. **That skip was added after measurement**
— the first cut of the case FAILED on the contract tree, which is precisely the "fail on an
unimplemented subsystem" ID-2 forbids.

### 3.6 `TextureUploadShapeScenario` (D-D4, recorded)

Asserts only what is assertable before the shape policy is finished: the numbers could be read at
all, the server bracket's own arithmetic (`box + rect == emit`, `jobs >= emit`), and that the two
sides agree **when both are non-zero**. Everything else is `RecordProperty` + stdout.

**Recorded on the contract tree**, 3 frames × (40 scattered 2×2 rects + one 64×8 contiguous band):

```
[ TextureUploadShape ] backend=DirectGLES frames=3 scattered_rects_per_frame=40
    server[emit=6 box=6 rect=0 jobs=6] client[ctu=0]
```

Six emissions (3 frames × 2 textures), **all six took the union-box shape**, 6 driver jobs — i.e.
Espryt collapsed 40 scattered rects into one box on every frame, on llvmpipe. The client side reads
zero and the case says so rather than reporting a divergence against an emitter that does not exist.
This is the P3b/P4b gate's baseline.

---

## 4. `MG_IntegrationTest/CMakeLists.txt`

**Every phase pin was re-read, not pattern-replaced.** Raised to the P4a constant:
`MGL_ITEST_HANDLES_ARM_KNOBS` (`0x1ff`→`0x1fff`), the two CSO **On** lanes, the CSO **Off** lanes'
`0x80000000000001ff`→`0x8000000000001fff` (the mask moves with the phase, the control is bit 63),
`ResourceSubsystemControl`'s On lane, both `MapPersistentRoundtrips` lanes and
`LargeArenaAdoption`'s On lane. **Left alone**: the two `0x7f` pins
(`ResourceSubsystemControl`'s Off lane and `LargeArenaAdoption`'s Off lane) — `0x7f` is P2's constant
and those lanes' named A/B control. The three comment blocks that explained the old masks were
rewritten to explain the new ones; a stale comment beside a moved pin is how the next phase gets it
wrong. **Measured: those lanes are green at the raised mask** — `CsoContentAddressing |
ResourceSubsystem | MapPersistentRoundtrip` is 28/28 on `build-push` and 36/36 on `build-verify`.

**New probes** (all inside the existing `if (MOBILEGL_PIPE_PUSH)`, all content probes over a
directory rather than filename globs, all `CONFIGURE_DEPENDS`):

| marker | probe | gates |
|---|---|---|
| `MGITEST_HANDLE_REKEY_OBJECTS_<backend>` | `kMGPipeSubsystem(Framebuffer\|TextureResources\|Samplers\|Programs)` under that backend | the six ABA cases' `Handles` arm; the refusal case |
| `MGITEST_HANDLE_ABA_OBJECTS_<backend>` | **conjunction**: `PipeHandleAbaControl` **and** a P4a subsystem constant, in ONE source | whether the six expect the corruption |
| `MGITEST_PIPE_OBJECT_EMITTER_PRESENT` | `FramebufferEmissions` under `MG_Impl/Pipe` | `ObjectSubsystemControl`'s emission case |

The conjunction needed a new helper, `mgl_itest_probe_for_two_symbols`. A single-regex probe for
`PipeHandleAbaControl` would hit `MagmaPipeArms.h` **today** and arm six controls on a backend where
the knob cannot reach any of their kinds; a single-regex probe for the subsystem constants will be
true for any backend that honours the mask, long before anyone wires the knob. What the arm needs is
one source that does both.

**New lanes** (all `DirectGLES`, all registered in every build so G2's name-for-name comparison
holds, each with a private `MOBILEGL_LOG_FILE_PATH` and a `TEST_FILTER` naming ONE case because the
library opens its log `fopen(path, "w")` and two readers in a lane race under `-j`):
`ObjectSubsystemControl.{On,Off,Refused}.` and `TextureUploadShape.`. Every environment goes through
`mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})`, so the `;` separators are escaped and the
EGL vendor / ICD pinning is not lost to the property that replaces the job environment.

**No `RESOURCE_LOCK` was needed**: it exists for lanes where a whole scenario shares one log and one
case reads it; each of the four new lanes selects a single case, which is the stronger form of the
same guarantee. `TextureParamsWithoutASamplerViewScenario` gets **no lane at all** — it reads no log
and no counter, and its claim is about the shipping configuration, so the ambient registrations are
exactly its arms.

---

## 5. `scripts/p4a_untouched_regions.sh` — exit contract and self-test

```
p4a_untouched_regions.sh <ref-a> <ref-b>   compare the seventeen at two git refs
p4a_untouched_regions.sh <ref>             print the seventeen shas at one ref
p4a_untouched_regions.sh --self-test       prove the comparison can go red
```

stdout is always the `<sha256>  <region>` list in the fixed order (a baseline capture is a plain
redirect); everything else is stderr. **0** identical / **1** at least one moved, first one named /
**2** could not run (bad ref, missing file, a name not defined exactly once, a self-test control that
did not trip). Both arguments are git refs, so an uncommitted edit is invisible by design.

The seventeen: P3a's eleven in `Managers.cpp` (which P4a must not touch either) plus P4a's six —
`StageBlocksIntoUnpackRing`, `UnpackRingAvailable`, `UnpackRingAllocate`,
`RecomputeBackendColorSlots` (`Managers.cpp`), `DepthStencilSamplingReadImpl` (`DirectGLES.cpp`) and
`ShouldUseCaveatTextureFormat` (`Utils.cpp`). Both preprocessor arms of the duplicated flush ladder
are hashed, exactly as in the parent (`FlushPendingRangesNow` in the `#else`,
`FlushPendingRangesFrom` in the `#if MOBILEGL_PIPE_PUSH`), so a push arm that re-spells the ladder is
a finding rather than an invisible drift.

**Self-test evidence, verbatim:**

```
[p4a-untouched] positive control: 17 regions extracted from the working tree
[p4a-untouched] positive control: an untouched copy compares equal
[p4a-untouched] positive control: an edit outside the seventeen regions is invisible, in all three files
[p4a-untouched] negative control 1: a perturbed ClearBufferPool body is reported, and named
[p4a-untouched] negative control 2: a perturbed FlushPendingRangesNow body is reported, and named
[p4a-untouched] negative control 3: a perturbed RecomputeBackendColorSlots body is reported, and named
[p4a-untouched] negative control 4: a perturbed StageBlocksIntoUnpackRing body is reported, and named
[p4a-untouched] self-test passed: 4 negative controls, all tripped and all named
```

The four are D-N's four, and they are four different shapes on purpose: the small no-overload body,
one of a PAIR of identically shaped ladders in two preprocessor arms, a `Class::method(` with a
multi-line signature and a call site of its own, and a `static` in an anonymous namespace between two
other protected bodies. The count is asserted (`!= 4` is exit 2), so a control cannot be dropped
silently.

**The seventeen shas at `37da3c3a` == at `HEAD`** (rc 0). For the record:

```
ba79b92d… IsPoolable            6563286e… EnrollIntoPool        dc9d3066… AcquireFromPool
12656cfd… TrimBufferPool        c44a1274… ClearBufferPool       15259e54… ProcessDeferredBufferReleases
2daaa525… CreateRingStorage     6221df2e… RingAvailable         709b6f41… RingAllocate
c6557002… FlushPendingRangesNow 37fc94ff… FlushPendingRangesFrom (the PINNED sha, unchanged)
791b54a3… StageBlocksIntoUnpackRing   1d6f15c9… UnpackRingAvailable   19e614af… UnpackRingAllocate
3ae7e347… RecomputeBackendColorSlots  822ca3b0… DepthStencilSamplingReadImpl
266710e0… ShouldUseCaveatTextureFormat
```

**Two declared deviations from D-N, both written into the script's header:**

* **D-N/1 — a region KIND.** `DepthStencilSamplingReadImpl` is a **namespace**
  (`DirectGLES.cpp:8117-8578`), not a function. D-N's table names it as though it were one, and the
  parent's extractor — which finds the one `<name> (` whose closing paren is followed by `{` — finds
  **zero** definitions of it and would exit 2 forever. Rows now carry `function` or `namespace`;
  hashing the whole namespace block is also the stronger reading of "the D24S8 sampling-emulation
  CORE".
* **D-N/2 — the pin is consulted unconditionally.** The parent falls back to
  `PINNED_SHA_FlushPendingRangesFrom` only when `<ref-a>` does not define the function; at P4a's base
  ref it **does**, so the fallback would never fire and the pin D-N names would stop being the
  baseline. This script always compares against the pin and says so on stderr if `<ref-a>` defines it
  differently. **Measured: they agree** (`37fc94ff…` at `37da3c3a` is the sha pinned at `3e298c9a`),
  so the two readings are the same answer on this tree.

---

## 6. `scripts/p4a_descriptor_negative_control.sh` — exit contract

```
p4a_descriptor_negative_control.sh <build-dir>
```

**0** both controls tripped **and named their field**; **1** a control did not answer (a suite stayed
green with its field dropped, or went red without ever naming it — both are findings about the TEST,
and both leave the tree restored, rebuilt and re-run); **2** a control could not run. **Exit 2
outranks exit 1**: "could not run" is never reported as "did not answer".

The two breaks: `MGPSurface::Layered` stops being copied in `FramebufferEmit.h`, and
`SamplerParameters::borderColorForm` in `SamplerEmit.h`. Each is patched by **regex over any
`.<field> = <expr>[;,]`**, because both headers belong to other packages (C.7: B and C) and their
spelling is theirs; the right-hand side is *replaced*, not deleted, so the record still has the
field, still asserts its size, and the break stays one the compiler cannot see.

**Restore is not enough; the rebuild is part of the contract.** Every exit after a patch goes through
`repair()` — restore from a byte-for-byte copy taken before the patch (never from git, so a dirty
tree gets its own state back), rebuild, re-run the suite — and `repair()` also runs from the `EXIT`,
`INT` and `TERM` traps, so a Ctrl-C during a rebuild cannot leave a hard-wired field in the tree. A
failed repair downgrades the verdict to 2. Both controls always run even when the first could not,
because "one emitter has landed and the other has not" is a different tree from "neither has". Logs
and backups live in `<build-dir>/p4a-g7-logs/`, never in the repository.

**On this tree it exits 2 and says why**, which is the required answer:

```
[p4a-g7] MobileGL/MG_Impl/Pipe/FramebufferEmit.h exists but assigns no .Layered.
[p4a-g7] On the P4a CONTRACT tree that is the expected answer: the contract commit creates all five
[p4a-g7] emit headers with STUB emitters (contract-v1 D1) that return 0 payload bytes and copy
[p4a-g7] nothing, and package B (clientfb) fills the body in. …
[p4a-g7] ---- G7 (P4a descriptor emission) ----
  Layered (MobileGL/MG_Impl/Pipe/FramebufferEmit.h): could-not-run
  borderColorForm (MobileGL/MG_Impl/Pipe/SamplerEmit.h): could-not-run
```

No patch was applied and no rebuild was needed, so nothing was left behind — verified by
`git status` after the run.

---

## 7. `.github/workflows/test.yml`

* `BASELINE: "44c2b5cf"` → **`"37da3c3a"`**, with the comment rewritten to say why the narrowing is
  safe: P3a's eleven were byte-identical against `44c2b5cf` at P3a's exit, so `37da3c3a` carries the
  same bodies (verified here: `p3a_untouched_regions.sh 37da3c3a HEAD` rc 0, and
  `FlushPendingRangesFrom` still hashes to its `3e298c9a` pin). Both gates now answer "did anything
  on the list move during **P4a**".
* **Two new `pipe-gates` rows**, `p4a_untouched_regions.sh "${BASELINE}" HEAD` and its
  `--self-test`, under the **same** `feat/disaggregated`-or-`workflow_dispatch` guard the P3a rows
  carry. The P3a rows stay and keep running: neither gate can be silenced by editing the other's
  list.
* The push-build control step gains three `-R` alternatives — `ObjectSubsystem`,
  `TextureParamsWithoutASamplerView`, `TextureUploadShape` — each the shortest string that selects
  only what it means to. **`ResourceSubsystem` does not match `ObjectSubsystemControl`**: the two
  families are named apart on purpose, and a filter that merged them would hide one behind the other.
  That step (in `integration-verify`) is the **only** CI job that unpacks a push build, which is why
  the new entries have to be there and not in the pull `integration` job.
* **The TEMPORARY trigger lines at the top of the file are untouched.**

---

## 8. Deviations, declared

| # | deviation | why |
|---|---|---|
| **F-1** | **four commits, not three.** C.5 gives three messages; `ObjectSubsystemControlScenario` and `TextureUploadShapeScenario` are in C.5's file table but under none of them | putting them under one of the two `[Test]` messages would have made the message describe something it does not. The three exact messages are present, in C.5's order, with the fourth between the second and the last |
| **F-2** | **the six ABA cases' `AbaControl` arm asserts the CORRECT pixels and says it is not a control yet**, instead of asserting the corruption | no backend keys P4a's six kinds on `{slot, gen}` *and* reads `Features.PipeHandleAbaControl`; Magma's object paths are P7 and the Espryt knob consumer is package D's file. A lane asserting a corruption nothing can produce is a permanent red on an always-on `integration-gpu` lane. §3.2 |
| **F-3** | **G9's mandatory red-before was not produced**; all four cases are green on `08192d72` | mechanically impossible through public GL on a monolith tree: the only observable of a texture parameter is a sample, and the sample repairs the state it was meant to catch. §3.4 |
| **D-N/1** | the byte-identity script gains a region **kind**; `DepthStencilSamplingReadImpl` is a namespace | D-N names it as a function; the parent's extractor finds zero definitions and exits 2 forever. §5 |
| **D-N/2** | the pinned baseline is consulted **unconditionally** | D-N says that row is compared against the `3e298c9a` pin, and the parent's fallback would never fire at P4a's base ref. Both readings agree here. §5 |
| **F-4** | `MagmaPipeArms.h` gains `MagmaPipeAbaControlCoversKind` rather than changing `MagmaPipeAbaControlDefeatsIdentity`'s behaviour | C.5 asks for the knob to defeat "P4a's memo keys on the Magma side too"; there are none to defeat. The per-kind function states that as code rather than as a comment, and is where the answer changes when a consumer appears |
| **F-5** | `build-bench` does not exist in this worktree, so C.5's benchmark verification line did not run | `wsl_tree.sh` creates three build dirs; nothing in this package touches `MG_Benchmark/` |
| **F-6** | `~/w7/p4a-trees2.log` never contained `REBUILT gates` | the file was 0 bytes with no build process running when work started, and the tree's own build dirs and per-tree logs were complete. Building proceeded from there; all three builds reconfigured and relinked cleanly, and the pull `.so` is byte-identical to `$BASE`'s, which is the check that would have caught a stale tree |

---

## 9. What the integrator must decide, and re-run on the finished tree

**Two rulings are needed before the phase verdict:**

1. **G9's missing red-before artefact (F-3).** ROADMAP.md:20 requires it by hand. The options, in the
   order I would rank them: (a) accept the scenario as a regression net and record in
   `MEASUREMENTS.md` that the artefact is unobtainable through public GL, with §3.4's mechanism as
   the reason — this is the honest reading and it costs nothing that is real; (b) ask **package D**
   (which owns `Managers.cpp`/`DirectGLES.cpp`) for an Espryt-internal probe or a one-off
   instrumented run taken *before* its own change, which would produce a genuine red-before but
   inside a file this package may not touch; (c) drop the requirement, recorded.
2. **The six ABA controls' arm (F-2).** Either accept that they arm when a backend wires the knob
   over its P4a object slot tables — the probe is already in place and the change is one `if` — or
   grant package D (or E) that one `if` explicitly, in which case the six flip to expecting the
   corruption with no change to this package.

**Re-runs the integrator owes on the integrated tree** (this package integrates LAST, so all of them
run against the finished code):

| what | why it must be re-run here |
|---|---|
| `ctest --test-dir build-verify -R 'HandleRecycle'` | the six ABA cases' `Handles` arm and the seven leak cases **arm themselves** when D and E land. The leak cases are the phase's only defence against P3a's C-1 shape, on both backends |
| `ctest --test-dir build-push -R 'ObjectSubsystemControl'` and `MOBILEGL_PIPE_PUSH=0x9ff ctest -R 'ObjectSubsystemControl'` | both cases SKIP today; the On/Off A/B arms when B and C land, the refusal when D lands. The refusal lane already pins `0x9ff` itself, so D.3's separate `0x9ff` invocation is a second reading of the same thing rather than the only one |
| `ctest --test-dir build-push -R 'TextureUploadShape'` | ~~the client half (`ctu=`) is zero today; once B lands, the `ctu == emit` assertion is live~~ **DONE, P3b/P4b wave 2-D (R-11).** The client emitter landed (`MG_Impl/Pipe/TextureEmit.h`'s `EmitOneLevel`), the `ctu == emit` comparison is live, and the scenario is now a GATE against a gold row rather than a record. The workload gained a third texture written through a `glTextureView`, so the numbers are not comparable to the P4a baseline `emit=6 box=6 rect=0 jobs=6`: the row measured on `cf7ca59f` is **`emit=9 box=9 rect=0 jobs=9`, client emitter 9**, identical on monolith / inproc / spawn / tcp. See `notes/p34b/espryt-d2.md` |
| `bash scripts/p4a_descriptor_negative_control.sh build-push` | exits 2 today because both emit headers are the contract's stubs. It becomes a real G7 control the moment B and C fill them in, and it is **not** a CI lane (it rebuilds the library up to four times) |
| `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` | this is the gate D and E can break. It is rc 0 here because nothing on the list has been touched yet |
| `ctest --test-dir build-push -R 'TextureParamsWithoutASamplerView'` | green here; after `esprytobj` it is the regression net that says the parameter sync did not move onto the sampler view |
| the full `-L integration-gpu` on the **pull** build | run here on the push build only (1072/1072). G2 wants both, and the pull build's ambient entries include the three new scenarios' skips |

**Artefacts kept** in `~/w7/notes/p4a/p4a-results/`: `p4a-handlerecycle-before.log` (180/180, the six
kinds' arms per lane), `p4a-handlerecycle-before-verbose.log` (the leak cases' live/high-water
numbers and every skip reason in full), `p4a-texparams-before.log` (G9's four cases, all green — the
D.2 evidence, such as it is), `p4a-object-subsystem-before.log` (G12 and the upload shape).
Everything else this package wrote under `~/w7` has been deleted.
