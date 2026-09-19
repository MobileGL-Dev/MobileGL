# P4a package C — `clientsp`. Adversarial review, v1

Target: `refs/heads/p4a/clientsp` = `ecd5b2e7`, four commits on `08192d72` (the tag `p4a/contract`).
11 files, +2426 / −52. Read-only review; no build, no worktree, nothing on the branch touched.

## Verdict — **REWORK**

The package is well built and the two hardest things in C.2 are right: the padding discipline (D1) is
correct at both the hash and the confirm, and the composite band is entered through exactly one door.
But a rework round is already mandatory (the c0b reconciliation, ID-13), and three defects in files C
owns must ride it, plus two cross-package under-fires that C's own subject matter surfaces and that
nobody else is positioned to notice:

* **C-M1** `MGPipeCompositeResolver::Reset()` permanently disarms the resolver's release path.
* **C-M2** a re-issued `create_shader_state` clears the applier's default uniform block (wire's W6) and
  the emitter's `(Cso, Version)` latch is never invalidated, so the block is never re-sent.
* **C-M3** an LRU eviction of a sampler CSO silently invalidates every handle another record still
  names — `MGPTextureParams::BuiltinSampler` above all, which wire says it deliberately does **not**
  resolve. This is the seam ID-14 rules on, and ID-14 does not close it.
* **C-M4 / C-M5** (owner **A**, `Tracker.h`): `bind_sampler_states` cannot see `glBindSampler`, and the
  whole program family is invisible to the dirty walk under a program pipeline.

None of these are visible on this branch — every applier entry point is a stub here — which is exactly
why they have to be written down before `wire` + `clientfb` + `esprytobj` are underneath them.

---

## 1. What was checked, and how

Every finding below was reached by opening the file, not by reading the report. The sources cross-read
were `INTEGRATOR-DECISIONS.md` (ID-8..ID-14), `BRIEF-P4A.md` D-F/D-G/D-H/D-I/D-J/D-K/D-L and C.2/C.7,
`contract-v2.md` §3 (the exact birth-hook declarations) and `wire-v1.md` §2.3/2.5/2.6/2.7/2.8/§4/§5 —
the last being what tells you what the applier actually stores and refuses, which several of C's
decisions have to be judged against and which C could not have read.

Not done, and stated as an absence: **no build and no test run.** The report's §5 transcript is
internally consistent (I re-derived the "38 added ctest names" as 13 contract + 25 C's from the four
files' `TEST(` macros and the pull skip lists, and it reconciles exactly), and the integrator re-runs
the whole gate on the integrated tree anyway. The LFS claim (D11) is **verified**: the fixtures in
`~/w7/p4a-clientsp/tools/trace_replay/fixtures` are 7–13 MB real archives and
`find … -size -1k | wc -l` is 0, so the 79/79 was read on real fixtures.

---

## 2. Findings in files C owns

### C-M1 [MAJOR] `MGPipeCompositeResolver::Reset()` permanently disarms the release path

`MG_Impl/Pipe/CompositeResolver.h:138-140`, `:104-107`, `:173-174`.

```cpp
void Reset() { for (Entry& entry : m_entries) entry.Live = false; }          // :138-140
…
if (entry->Signature == signature && entry->Handle == handle) {              // :104-107
    ++m_counters.Reuses;
    return handle;                     // <-- returns WITHOUT restoring entry->Live
}
…
void ReleaseEntry(Entry& entry) { if (!entry.Live || …) return; … }          // :173-174
```

**Failure scenario.** Bind pipeline P, draw — `Observe` mints an entry with `Live = true`. A
make-current arrives: `PipeFill.cpp:1908` calls `MGPipeProgramEmitterInstance().Reset()`, which calls
`MGPipeCompositeResolverInstance().Reset()` (`ProgramEmit.h:300`), and every entry goes `Live = false`.
The next draw has the same signature and the same handle, so `Observe` takes the *Reuses* branch and
returns before `entry->Live = true` — the entry is now stuck at `false` for the life of the process.
When the stage set really moves, `Observe` calls `ReleaseEntry`, which sees `!entry.Live` and returns
immediately: **no `delete_shader_state`, no `Free`, and the counter does not move**. The entry is then
overwritten with the new signature and handle, so the old composite's handle is lost from the resolver
entirely.

**Refutation attempted, and how far it gets.** Today nothing leaks: `ProgramPipelineObject` holds a
*one-slot* cache (`ProgramPipelineObject.h:149-159` — `SetCachedDrawProgram` overwrites
`m_drawProgram`), so the overwrite drops the last `SharedPtr` and `~ProgramObject` →
`MGPipeEmitShaderCsoDestroyAndFree` frees the slot. So the observable damage is zero *right now*. That
is precisely the argument the file's own header refuses to accept: *"the resolver still speaks the
release, because 'usually' is not a contract and a client that only reacted to destructors would leak a
slot the moment the frontend started holding a second reference"* (`CompositeResolver.h:31-35`). The
code does not do what the comment says it does, after the first make-current.

**Not caught by any case** — see C-m9.

**Fix**: set `entry->Live = true` in the reuse branch (or, better, do not clear `Live` in `Reset()` at
all — the memo's freshness is `Signature`/`Handle`, and `Live` is a *release obligation*, not
freshness; the two were conflated into one flag).

### C-M2 [MAJOR] a re-issued `create_shader_state` drops the server's global constants and nothing re-sends them

`MG_Impl/Pipe/ProgramEmit.h:195-245` (the re-issue) against `:164-168` (the latch), read against
`wire-v1.md` §4 **W6**: *"a re-issued `create_shader_state` clears the default uniform block."*

```cpp
// ProgramEmit.h:168
if (cso == m_constantsCso && version == m_constantsVersion) return bytes;
```

`AcquireShaderCso` re-issues the create whenever `GetLinkVersion()` moves (`:200-203`), and
`m_constantsCso` / `m_constantsVersion` are touched only by `EmitGlobalConstants` itself, by `Reset()`
and by `NoteRecordDestroyed` (which nothing calls).

**Failure scenario.** Program P is in use; `set_global_constants(H, V)` has gone out. The application
attaches a shader that will not compile and calls `glLinkProgram(P)`. The link **fails**;
`ProgramObject.cpp:357` bumps `m_linkVersion` regardless, and `ProgramObject.cpp:371` records that the
phase-B output (`globalUboScratch` included) is deliberately **not** cleared — which is right, because
GL keeps a program that is active for a stage running on its previous executable and uniforms after an
unsuccessful re-link. Next verb: bit 6 fires, `AcquireShaderCso` re-issues `create_shader_state` on
handle H, and the applier **clears the record's global constants** (W6). `GetUBOContentVersion()` did
not move, so bit 8 does not fire; and even when it is reached for another reason, `cso == H` and
`version == V` are both unchanged, so `EmitGlobalConstants` returns without emitting. The server now
draws P with a zeroed default uniform block while the client's scratch still holds the live values.

**Refutation attempted.** (a) *A successful relink is safe* — yes: `ProgramSpirvTask.cpp:352` clears and
re-sizes `globalUboScratch`, GL resets uniforms to 0 on a successful link, so the applier's cleared
block and the frontend's zeroed scratch agree, and the first `glUniform*` afterwards bumps the version
and re-emits. The bug is specific to the **re-issue without a content change**, of which the failed
relink is the reachable case. (b) *`RelinkStageSetScenario` would catch it* — it may, once E lands, but
by then the cause is three packages away. (c) *W6 landed after C's v1* — true, which is why this is a
rebase item rather than a defect of judgement.

**Fix**: one line in `AcquireShaderCso`, on the branch that actually emits — if
`m_constantsCso == handle`, reset `m_constantsCso = kMGPipeNullHandle` and
`m_constantsVersion = kMGPipeGlobalConstantsNeverUploaded`.

### C-M3 [MAJOR] LRU eviction invalidates handles other records still name, and the applier does not police them

`MG_Impl/Pipe/SamplerEmit.h:299-313` (`Evict`), `:252-258` (the LRU victim choice), `:167-170` (the
`static_assert` that is presented as making this safe), against `wire-v1.md` §2.3:

> `set_texture_params` … **not checked**: the CSO the record names is **not resolved**: the sampler bit
> may legitimately be clear while the texture bit is set, so an unresolvable `BuiltinSampler` is an
> ordering fact, not a corrupt one.

**Failure scenario.** ID-14 makes package B acquire `MGPTextureParams::BuiltinSampler` from this cache.
`set_texture_params` is emitted rarely — once per texture, then only when the texture's params move —
so a built-in sampler's entry is *touched* rarely and is therefore among the **first** LRU victims. Any
workload with more than 256 distinct live sampler values (a CTS sampler sweep; a scene with many
distinct filter/wrap/border combinations) evicts it. `Evict` emits `delete_sampler_state` and frees the
slot; the slot is re-handed out to a different value at `gen + 1`. The applier's
`MGPipeResourceRecord::Params.BuiltinSampler` still names `{slot, gen}` — and the applier **does not
resolve it**, so nothing refuses, nothing counts, nothing logs. Espryt's
`SyncBuiltinSamplerToBackend` then either misses (no twin at that generation) or, worse, finds the
slot's *new* occupant. Nothing re-emits `set_texture_params`, because the texture's params version did
not move: **an eviction is not a parameter change**, and D-E2's `SamplerResync` is not raised by it.

The same hazard applies, with a much smaller window, to a `MGPBoundView`/`bind_sampler_states` tail
already published: the record the applier holds is "the last set as received" and outlives the pass.

**Refutation attempted.** (a) *The `static_assert` closes it* (`SamplerEmit.h:167-170`): it closes only
the intra-pass case — 256 > 192 means one `EmitSamplerStates` pass cannot evict a handle that same pass
is about to name. It says nothing about a record published in an **earlier** pass, and the texture-params
record is exactly that. (b) *256 is never exceeded in practice*: correctness must not depend on cache
capacity, and this is the phase that runs the whole CTS. (c) *The applier refuses the stale gen loudly*
— it does not; wire says so in as many words. (d) *`~SamplerObject` covers it* — no: C deliberately gives
the content-addressed CSO no lifetime-id mapping (`SamplerEmit.h:161-166`), so
`MGPipeEmitSamplerCsoDestroyAndFree` resolves nothing and the LRU is the only death path, which is the
whole point of the design and also the whole problem.

**Fix, three shapes, integrator picks:** (i) an eviction hook that raises D-E2's `SamplerResync` for
every texture whose `BuiltinSampler` was the victim (needs a reverse map, or a global "CSO epoch" that
B's texture-params latch mixes in — cheapest and does not couple the two headers); (ii) pin entries a
live `set_texture_params` names, with the pin taken by B and dropped in the texture death helper (D5);
(iii) do not evict — replace LRU with "grow past 256 and log", which contradicts D-F1's capacity ruling
and therefore needs a brief amendment. **(i) is the smallest change that makes the failure impossible.**

---

## 3. Cross-package findings C's subject matter surfaces (owner: A — `Tracker.h` / `PipeFill.cpp`)

### C-M4 [MAJOR] `bind_sampler_states` cannot see `glBindSampler`

`PipeFill.cpp:1940-1942` gates `EmitSamplerStates` on `wants(MGPipeDirty::NewSamplers)`; bit 13's
shutter is `Mix(textureParams, ctx.GetSamplingResolutionGeneration())` (`Tracker.h`, the object-class
block). The only writers of `BumpSamplingResolutionGeneration` are `SamplerObject.cpp:65` and
`TextureObject.cpp:53` — both **parameter** changes. `glBindSampler` goes
`GL_Sampler.cpp:384 → TextureUnit::SetSamplerObject` → `TextureUnit.cpp:39`
`BumpTextureBindGeneration()`, which is **bit 12** (`NewSamplerViews`), not bit 13.

**Failure scenario.** `glBindSampler(3, samplerA)`; draw; `glBindSampler(3, samplerB)` with samplerB's
parameters differing from samplerA's; draw. Bit 12 fires both times (the view set is re-resolved,
correctly, because completeness depends on the effective sampler), bit 13 fires **neither** time. The
server's `BoundSamplerStates[3]` still names samplerA's CSO. Wrong filtering, no gate that sees it.
`Tracker.h`'s own rule is that over-firing is free and under-firing is fatal.

**Refutation attempted.** (a) *Something else bumps it* — grepped; there are exactly two writers, both
parameter paths. (b) *`GetTextureBindGeneration` is in bit 13 too* — it is not; it is in bit 12's mix
only. (c) *The legacy pull path saves it* — no: `bind_sampler_states` has no pulled twin;
`GetTextureUnitObject` is pulled, but `EmittedCallSuppliesTheWholeField(GetTextureUnitObject)` being
`false` means the field is still copied, so Espryt's *legacy* arm still works — the **push** arm is the
one that goes stale, and D-K3 says the legacy arm does not survive `MOBILEGL_PIPE_LEGACY_MEMOS=0`.
(d) *C's own case covers it* — `SamplerEmitTest.cpp:442` calls `Ctx().BumpTextureBindGeneration()` by
hand and then calls the emitter directly, which is bit **12**'s generation; the case documents the gap
rather than closing it.

**Fix (A's)**: either mix `GetTextureBindGeneration()` into bit 13's shutter, or gate `EmitSamplerStates`
on `wants(NewSamplers) || wants(NewSamplerViews)` in `PipeFill.cpp:1940`. Both are one line and both are
in files C may not touch.

### C-M5 [MAJOR] the program family is invisible to the dirty walk under a program pipeline

`Tracker.h` (bits 6/7/8): `const auto& program = ctx.GetCurrentProgram(); … if (program) { … }` — and
`Core.h:186` / `Core.cpp:611` make `GetCurrentProgram()` **null** whenever the application drove
`glUseProgram(0); glBindProgramPipeline(p)`. `Tracker.h` contains no reference to
`ProgramPipelineObject` at all (grepped).

**Failure scenario.** Bind pipeline P and draw: the *first* walk after a make-current is `!m_primed`, so
every bit fires once and the composite is emitted. Then `glUseProgramStages(P, …, newVs)` changes the
stage set; `GetProgramForDraw()` builds composite **B**. Bits 6/7/8 all still read `0 == 0`, so
`EmitShaderState` is never called again: **B never gets a `ShaderCso` handle, never gets a
`create_shader_state`, and `set_draw_program` keeps naming composite A**, whose `ProgramObject` the
one-slot pipeline cache has already dropped.

**Refutation attempted, and it does soften this.** `EmittedCallSuppliesTheWholeField(GetProgramForDraw)`
is `false` (`PipeFill.cpp:1383-1386`) and `DirtySurface.def:309-319` rules
`BindProgramPipelineObject` `kPulledEveryVerb` precisely because the field is *emitted-and-still-pulled*
— so the residual fill copies `m_programForDraw` at every verb and the **backend still receives the right
`SharedPtr`**. Rendering is therefore not wrong today. What *is* wrong is that D's re-keyed twin tables
resolve a program through its client-minted handle, and composite B has no handle at all
(`MGPipeSlots().FindByLifetimeId(ShaderCso, B)` misses), so D/E will see a program the handle protocol
never announced. That is a seam defect of exactly P3a's class, and it should be found now rather than in
E's verification round. Also note bit 8 never firing under SSO means `set_global_constants` is never
sent for a pipeline draw, and there the pull does **not** rescue anything.

**Fix (A's)**: mix the bound pipeline's identity and `ComputeDrawProgramSignature()` into bit 6 — the
signature is `{lifetimeId, GetLinkVersion()}` per stage (`ProgramPipelineObject.h:94-105`), i.e. exactly
the non-artefact fields the tracker is already allowed to read without joining, so the "must not force a
compile" constraint (`Tracker.h`) is preserved.

---

## 4. Minors

| # | where | what, and the refutation |
|---|---|---|
| **C-m1** | `ImageEmit.h:82-86`, `ProgramEmit.h:154`, `SamplerEmit.h:545-546` | All three key on `GetProgramForDraw()`. `Core.cpp:772-783` makes `GetProgramForDispatch()` a *different* object whenever a pipeline is bound (GL 4.6 §7.4 keeps compute exclusive). A compute-only pipeline gives `GetProgramForDraw() == null`, so `MaxImageUnit == -1`, the sticky window stays 0 and `set_shader_images` is **never emitted for the dispatch**; `EmitGlobalConstants` likewise never sends the compute program's default uniform block. *Refutation*: with a plain `glUseProgram` both getters return the same object, which is what every `ImageEmitTest` case does (`MakeComputeProgram` + `GL::UseProgram`), so the suite cannot see it. Narrow reach, but it is a hole with a name. |
| **C-m2** | `SamplerEmit.h:602-614` + `:192-213` | **No version-first skip in front of the CSO acquire.** Per firing of bit 13, per bound unit: `memset(100)` + sixteen assignments + `XXH64` over 100 bytes + a **linear** scan of up to 256 entries. D-F1 says *"One copy per mint attempt, never per draw, because the version-first skip runs first"*, and the file repeats it at `:95-96` — but `EmitSamplerStates` reaches `Acquire` unconditionally and only *then* asks the suppressor. *Refutation*: bit 13 is not per-draw, so this is per-batch rather than per-draw; and the fix is cheap and local — latch, per unit, `(sampler lifetime id, ctx.GetSamplingResolutionGeneration())`, which is exactly the pair that decides whether a sampler's 100 bytes can have moved. Recorded under ID-19's budget. |
| **C-m3** | `SamplerEmit.h:197-211` | The collision branch `Evict(i); break;` (a) can evict an entry the **record currently being built already names** — the `static_assert` at `:167-170` claims only the LRU victim can never be in the current pass, and this is a second eviction path it does not cover; and (b) stops the probe on the first hash match, so a *second* entry with the same hash whose bytes really do match is never found and a duplicate is minted. *Refutation*: both need a genuine 64-bit XXH64 collision, so the probability is not engineering-relevant. The finding is that the comment at `:155-159` states a property the code has a path around, and that path has no test (`Collisions` is only ever asserted `== 0`). |
| **C-m4** | `CompositeResolver.h:124-132`, `:98-101` | `Forget()` has **no caller** anywhere in the tree, and `m_entries` is keyed on `pipeline.GetExternalIndex()` — a GL name `glGenProgramPipelines` recycles. A deleted-and-recreated pipeline name inherits its predecessor's entry. *Refutation*: harmless, because `SlotAllocator.cpp:190-195` erases the lifetime-id mapping on `Free`, so the stale `ReleaseEntry` resolves nothing; and the vector is keyed by name so it cannot grow past the highest name ever used. It is still a dead public method and a key that is documented as recyclable everywhere else in the design. |
| **C-m5** | `SamplerEmit.h:412-483` | `MGPipeProgramOpaqueUnits` is a **one-entry** memo shared by the sampler and image emitters. A frame that alternates two programs re-walks `GetMaxUniformLocation()` locations on every switch. *Refutation*: the shared instance is the right call (two memos could disagree, which is the stated reason), and a two-entry memo would fix it without giving that up. Perf note only. |
| **C-m6** | `ProgramEmit.h:225-232` | `MOBILEGL_ASSERT(moduleCount <= 6, …)` compiles out at INFO — which D-J3 says is *"all three gate builds and every shipped build"* — and the loop then silently drops modules 7+. D-J3's own rule is a counted refusal, not an assertion. *Refutation*: `generatedSpirv` cannot exceed six stages today, so it is a guard against a future stage, and the truncation is the safe direction. Still the exact idiom D-J3 exists to forbid. |
| **C-m7** | `SamplerEmitTest.cpp:425-446` | `ARedundantRebindOfTheSameSamplerEmitsNothing` has **no `EmitterScope`**, so its `ASSERT_EQ(StateSetCount, 1u)` and `EXPECT_EQ(Mints, 1u)` depend on the preceding case's destructor having cleared the emitter. *Refutation*: it also passes run alone (fresh process ⇒ zero counters), and the gates do not shuffle. It is exactly the suite-order dependence D1 was. One line. |
| **C-m8** | `SamplerEmitTest.cpp:284-294` | `AHashCollisionDoesNotAliasTwoSamplerStates` **cannot go red for its name.** Deleting the `memcmp` confirm at `SamplerEmit.h:199` leaves it green: the two states it builds differ by one float ULP, so their hashes differ and the confirm is never consulted. The case honestly says so in its comment. *Refutation*: a real collision cannot be manufactured without a test hook into the cache. The honest close is a `#if` test seam that lets a case force two entries onto one hash — or renaming the case to what it proves. As it stands the memcmp branch, the `Collisions` counter and the collision-eviction path are **entirely uncovered**. |
| **C-m9** | `CompositeResolverTest.cpp:282-321` | Both "FreesTheSlotExactlyOnce" cases call `MGPipeSlots().AllocateComposite()` and then `MGPipeEmitShaderCsoDestroyAndFree()` twice — **the resolver is not in the picture at all**. `Releases` is asserted only `== 0` (`:239`). So the pinned "released exactly once" property is pinned at the *allocator*, and the resolver's own release path — the one C-M1 breaks — has no case. |
| **C-m10** | report §5 | ID-8 requires *"a leak test per new kind, run on the DirectVulkan lane too"*. §5's transcript has three `integration-gpu` arms and one `integration-verify` arm and **no DirectVulkan arm**. Contract-v2 §4.3 assigns the leak cases to package F, so this may be discharged elsewhere — the integrator must confirm F's G8b cases run on the DV lane rather than assume it. |

**Nits.** (n1) `SamplerEmit.h:310` `m_entries[index] = m_entries.back();` is a copy-**assignment**, which
does not carry padding; the cache's "the bytes stored are the bytes compared" invariant survives only
because every entry's padding was zeroed once by the `memcpy` at `:284` and `Vector` growth memmoves a
trivially-copyable type. True, but it deserves the same sentence `:271-278` gives the other half.
(n2) `MG_Test/SanityTest.cpp:3898` constructs `MakeShared<ProgramObject>(0u)`, which C's D5 discriminator
would classify as a pipeline composite; harmless (it never reaches the emitter) but it is a second
producer of external index 0 and the D5 argument says there is one. It is D's file — tell D.

---

## 5. The deviations, judged

| # | verdict |
|---|---|
| **D1** out-parameter canonicaliser + `memcpy` | **Correct and necessary.** `SamplerEmit.h:103-128` / `:284`. The reasoning is right (memberwise copy of a trivially copyable type leaves padding unspecified) and the bug it describes is real. `PaddingCannotChangeTheHash` pins the *canonicaliser* half convincingly; the *storage* half is weaker than it looks (see n1), but the code is right. Accept. |
| **D2** the wired constants are not the emission gate | **Closed by c0b** (`contract-v2.md` §4.1). C's finding was correct at the time. **The consequence is a rework item**: the header comments that assert the opposite are now false — see §6.7. |
| **D3** death helpers hardcode `published = false` | **Closed by c0b** (§2/M1, D17). The latch is A's, not the emitter's, and C's own `RecordIsPublished`/`NoteRecordDestroyed` are no longer what the death path asks. Rework item §6.5. |
| **D4** `TwoPipelinesWithTheSameSignatureKeepTheirOwnComposite` | **Accept, and the argument is right.** Verified: the composite is minted off its **own** lifetime id (`ProgramEmit.h:326-338`), `SlotAllocator.cpp:190-195` keys the map on that id, and `ProgramPipelineObject.h:161-163` gives each pipeline its own `m_drawProgram`. Sharing one handle would put one id in the map for two objects. The brief's name was wrong; the case is right and says so. |
| **D5** no `Core.{h,cpp}` hook; composite = `GetExternalIndex() == 0` | **Accept.** Verified there are exactly two production constructors of `ProgramObject(0u)` (`Core.cpp:661`, and `ProgramState.cpp:16` which passes a real GL name that is never 0). A `Bool` member would resize the pull object and break G1 outright. See n2 for the test-file third producer. |
| **D6** `ProgramObject.h` gains `GetSpirvReflection()` inside `#if MOBILEGL_PIPE_PUSH` | **Accept.** Needed (`MGPipeApplyCreateShaderState` takes the whole struct; wire makes a null `spirv` Fatal), minimal, and invisible to a pull build — G1's 0 resized confirms. |
| **D7** the image window is program-derived | **Accept with C-m1.** The reasoning against touching `TextureState` is right (B's file + a pull-build resize). The sticky union is the right hedge. The residue is the dispatch case. |
| **D8** the unit→target inversion is memoised per program | **Accept, and it is the right call.** O(units × locations) per verb was the alternative. First-writer-wins matches DirectGLES read forwards. One shared instance is right (C-m5 is about its size, not its sharing). |
| **D9** `static_assert(capacity > kMGPipeMaxTextureUnits)` | **Accept as far as it goes** — see C-M3 for what it does not cover, and C-m3 for the second eviction path it does not cover either. |
| **D10** nothing in `Reset()` touches an object record | **Correct for the sampler, image and program emitters** (verified against D-J4 and `PipeFill.cpp:1898-1911`). **Not correct for the composite resolver**, whose `Reset()` clears a release obligation rather than a memo — C-M1. |
| **D11** LFS pointers | **Verified restored.** Fixtures are real; 0 files under 1 KB. |
| **D12** `p4a-trees2.log` | Accepted as reported; nothing checkable now. |
| **D13** six extra test names | Accept (additions only; G14 is about removals). Re-counted: 25 C names + 13 contract = 38, matches. |
| **D14** cases assert on counters because the GL call already emitted | **Accept, and the suppression it describes is correct**, not a dropped emission: `EmitShaderImages` rebuilds the whole window from live context state and hands it to the suppressor, so a second call with unchanged content *should* send nothing — the record the applier holds is already that content. The only way it could drop a needed emission is if the first emission ran with a **smaller** window, and the window is monotone (`ImageEmit.h:86`), so a later call with a larger one produces a different hash and does emit. Correct. |
| **D15** the sentinel is pinned as a predicate | **Accept.** Driving `GetUBOContentVersion()` to `~0u` needs 2³² bumps. The live-path half is a conditional `if (… > 0u)` (`ProgramEmitTest.cpp:207`), which is weak, but `kVs` declares `uniform vec4 u_value` so the branch is taken. |

---

## 6. The c0b reconciliation list — exactly what the rebase onto `refs/heads/feat/disaggregated` must change

Contract-v2 §3.4 is a **compile-time** contract: the moment `kMGPipeWiredSamplerSubsystem` and
`kMGPipeWiredProgramSubsystem` are non-zero, `PipeFill.cpp`'s `if constexpr` seam instantiates and a
missing or misspelled entry point is a build error **in C's own commit**. C's branch predates the
declarations, so on rebase the build breaks until all three exist.

1. **`MGPipeSamplerEmitter::EmitSamplerCso(MG_State::GLState::SamplerObject&)` — does not exist.**
   Add it. Its body is the family's handle rule, which for this kind is content addressing:
   `MGPipeSamplerCsoCacheInstance().Acquire(sampler.GetAllSamplerParameters(), bytes)`. Called from
   `MGPipeEmitSamplerCsoCreate` (`PipeFill.cpp:1058-1114`), i.e. from `SamplerObject`'s constructor —
   note that minting a CSO at *construction* time content-addresses the **default** parameters and will
   be superseded the moment the application sets one; that is correct (the cache dedupes) but C should
   say so in the body, because it means one `create_sampler_state` per distinct default-valued sampler
   and not one per object.
2. **`MGPipeSamplerEmitter::EmitSamplerView(MG_State::GLState::ITextureObject&)` — does not exist.**
   Add it as a wrapper over `AcquireSamplerView` (`SamplerEmit.h:647`), resolving the texture handle the
   way `EmitSamplerViews` already does at `:572`
   (`MGPipeSlots().Acquire(MGPipeKind::Texture, texture.GetLifetimeId())`) and discarding the byte count.
3. **`MGPipeProgramEmitter::EmitShaderCso(MG_State::GLState::ProgramObject&)` — does not exist.**
   Add it as a wrapper over `AcquireShaderCso` (`ProgramEmit.h:195`).
4. **The publication latch (contract-v2 §3.1) is called from nowhere.** Add, and nothing else:
   * `MGPipeNoteHandlePublished(MGPipeKind::SamplerCso, cso)` after `SamplerEmit.h:288`;
   * `MGPipeNoteHandleUnpublished(MGPipeKind::SamplerCso, …)` in `Evict` (`SamplerEmit.h:299-313`) **and**
     in `ResetForTest` (`:231-235`) — the latter frees slots without a delete, so a stale latch would
     make a future death helper emit a delete for a record that never existed;
   * `MGPipeNoteHandlePublished(MGPipeKind::SamplerViewCso, handle)` after `SamplerEmit.h:681`;
   * `MGPipeNoteHandlePublished(MGPipeKind::ShaderCso, handle)` after `ProgramEmit.h:237`.
5. **`RecordIsPublished` / `NoteRecordDestroyed` stop being the death path's authority** (D17). Keep them
   — the view latch and the program latch are what the version-first skip needs — but rewrite their
   comments (`SamplerEmit.h:215-218`, `:693-697`; `ProgramEmit.h:247-254`), which currently say the death
   path asks them. `MGPipeProgramEmitter::NoteRecordDestroyed`'s bound-mirror clearing
   (`ProgramEmit.h:271-277`) then has **no caller**: either state why the mirrors self-heal (the slot's
   `Gen` moves on reuse, so the next `EmitShaderState` sees a different handle and re-binds) or delete
   the three lines. Do not leave it as unreachable code with a live-looking comment.
6. **The sampler CSO has no lifetime-id mapping, and D must be told.** With C's design (right, per
   contract-v2 §3.4's open box) `MGPipeEmitSamplerCsoDestroyAndFree(m_lifetimeId)` resolves nothing, so
   `NotifyStateObjectDestroyed(SamplerCso, lifetimeId)` reaches D with **no handle behind it**. D must not
   key a sampler twin on a `SamplerObject` lifetime id; the twin's life is `create_sampler_state` →
   `delete_sampler_state` (LRU) and nothing else. Put that sentence in `SamplerEmit.h` and in the report.
7. **Two header comments are now false and must be rewritten**: `SamplerEmit.h:66-71` and
   `ProgramEmit.h:50-54` both say *"It is NOT the emission gate"*. After c0b, `wants()` consults
   `kMGPipeWiredSubsystems` (`contract-v2.md` §4.1) and the constants **are** the switch, in both the
   validate point and the client paths. A comment that says the opposite is worse than no comment.
8. **No change expected, but verify**: `CompositeResolverTest.TheCompositeBandHasExactlyOneDoor` reads
   `LiveCount(MGPipeKind::ShaderCso)` across a composite free; c0b deliberately kept `LiveCount` and
   `FreeCount` summing both spaces so that case stays green (contract-v2 §4.3). It should still pass
   unchanged.

---

## 7. ID-14 — the built-in sampler seam, answered

**The exact call B must make**, replacing `TextureEmit.h`'s
`MGPipeSlots().Acquire(MGPipeKind::SamplerCso, sampler->GetLifetimeId())` inside `EmitTextureParams`
(clientfb tree, `MG_Impl/Pipe/TextureEmit.h`, the block that starts *"THE BUILT-IN SAMPLER'S CSO SLOT IS
KEYED ON THE SamplerObject's OWN LIFETIME ID"*):

```cpp
Uint64 samplerBytes = 0;
const MGPipeHandle builtinSampler =
    MGPipeSamplerCsoCacheInstance().Acquire(sampler->GetAllSamplerParameters(), samplerBytes);
```

Both names are in `MobileGL::MG_Pipe`, so no qualification is needed inside `TextureEmit.h`;
`ITextureObject::GetSamplerObject()` returns `const SharedPtr<SamplerObject>&`
(`TextureObject.h:34`) and `SamplerObject::GetAllSamplerParameters()` returns
`const SamplerParameters&` (`SamplerObject.h:64`). `Acquire` takes a **non-const `Uint64&`**, so B needs
a real lvalue; B should fold `samplerBytes` into whatever it returns as payload bytes rather than
discarding it, or the `csob-blob` accounting under-reports by 100 bytes per mint. B must **delete** its
own `MGPipeSlots().Acquire(SamplerCso, …)` line: leaving it would mint a second, record-less slot that
`~SamplerObject` then frees.

**Include-closure check — usable, no cycle.** `TextureEmit.h` must add
`#include <MG_Impl/Pipe/SamplerEmit.h>`. `SamplerEmit.h`'s own closure is `SetHashSuppressor.h`,
`SlotAllocator.h`, `Tracker.h`, `MGPipe.h`, `MGPipeHostSpan.h`, `PipeApply.h`, `Core.h`,
`ProgramObject.h`, `SamplerObject.h`, `TextureObject.h`, `TextureUnit.h`, `PipeStats.h`, `xxhash.h`.
The only files in the tree that include `TextureEmit.h` are `PipeFill.cpp:34` and
`TextureEmitTest.cpp:54` (grepped), so nothing in that closure reaches back. `ImageEmit.h:36` already
includes `SamplerEmit.h`, so the shape is precedented.
**Re-run `check_include_closure.py --mode both --compiler clang++ --self-test --require-all
--expect-probes 4` after the edit** (ID-14 says so, and c0b's `--expect-probes` is new).

**When the sampler object's parameters later change — who re-emits `set_texture_params`?**
For the texture's *own* built-in sampler: `glTexParameter*` writes through `TextureObject.cpp:53`, which
bumps both `GetTextureParamsVersion()` and `BumpSamplingResolutionGeneration()`; a `glSamplerParameter*`
on a `SamplerObject` goes through `SamplerObject.cpp:65`. Bit 13 fires, and **B's `EmitTextureParams` is
the only re-emitter** — it re-`Acquire`s, the content has moved, the cache returns a **different**
handle, and the new handle goes into the record. So the content-addressed handle changing with content
is self-healing *provided B's own params gate is keyed on `GetTextureParamsVersion()` and not on
something a sampler-parameter write leaves alone*: **the reviewer of B must check that**, because C's
side gives no signal.

**What is not covered, and is the real risk:** eviction — **C-M3**. A parameter change moves the handle
and B re-emits; an LRU eviction moves the handle's validity and nobody re-emits. ID-14's ruling
("B re-emits whenever the built-in sampler's parameters change") is necessary and not sufficient.

Hazard **H1** (wire): unchanged and consistent — with B taking the handle from C's cache the
`BuiltinSampler` is never null in a build where both families are wired, and the `0x9ff` arm (bit 10
without bit 11) stays a **contract dependency refusal**, package F's scenario.

---

## 8. Exit order, hygiene, ownership

* **Exit order (ID-8): clean.** All six new process singletons are `static T* p = new T();` and are never
  destroyed — `MGPipeSamplerCsoCacheInstance` (`SamplerEmit.h:320-326`),
  `MGPipeProgramOpaqueUnitsShared` (`:480-483`), `MGPipeSamplerEmitterInstance` (`:765-772`),
  `MGPipeImageEmitterInstance` (`ImageEmit.h:167-172`), `MGPipeProgramEmitterInstance`
  (`ProgramEmit.h:396-403`), `MGPipeCompositeResolverInstance` (`CompositeResolver.h:186-192`).
  **No frontend `SharedPtr` is stored by any of them**, checked member by member: the cache's `Entry`
  holds `SamplerParameters` by value (a POD of enums/floats/vec4s); the resolver's `Entry` holds
  `Array<Uint64, N>` (`ProgramPipelineObject.h:94`), a `Uint`, a handle and a `Uint64`; the three
  emitters hold `Array`s of wire PODs and `Vector`s of trivial latches. Confirmed against the task's
  specific question: **the cache's entries do not hold `SharedPtr<SamplerObject>`.**
* **Hygiene: clean.** No `MGLOG_I`/`MGLOG_W`/`MGLOG_E`, no `TODO`/`FIXME`, no `printf`/`std::cout` and no
  `pGLContext` anywhere in the four headers (grepped). `pGLContext` appears only in the test files, which
  is where it belongs. G13's purity greps are unaffected (these are `MG_Impl`, the client side).
* **Ownership: clean.** The 11 changed files are all C's by C.7 — the four `MG_Impl/Pipe` headers,
  `MG_State/GLState/{SamplerState,ProgramState}/**`, and the four `MG_Test/Pipe/*Test.cpp`. Nothing under
  `MG_Backend/`, `scripts/`, `MG_Pipe/`, `.github/`, or A's `MG_Impl/Pipe` files. The C.7 grants
  (`GL_Program.cpp`, `Core.{h,cpp}`) are genuinely unused — D5's argument for not needing the `Core.cpp`
  hook holds up.
* **Shared logs / `RESOURCE_LOCK`: not needed.** Each of the four test `main()`s points
  `MOBILEGL_LOG_FILE_PATH` at a **pid-unique** file and removes it (e.g. `SamplerEmitTest.cpp:452-456`),
  so `-j 8` cannot make two suites share one log.
* **`EmittedCallSuppliesTheWholeField` (report §4): all six `false` verdicts confirmed.** The three that
  matter were re-derived independently: `GetProgramForDraw`/`GetProgramForDispatch` are pointer-valued
  and the emitted call carries a `{slot, gen}` (and `DirtySurface.def:309-319` depends on them staying
  pulled — see C-M5); `GetTextureUnitObject` hands back a live `TextureUnit&` with one slot per target
  while `set_sampler_views` carries the resolved single view, which is strictly less;
  `GetMaxTouchedTextureUnit` is the `Count` argument and the suppressor is what swallows the redundant
  re-bind. No row moves, so `PipeFill.cpp` needs no edit — correct.

---

## 9. The rework list, in order

1. **C-M1** — `CompositeResolver.h`: stop `Reset()` disarming the release path (restore `Live` in the
   reuse branch, or split freshness from the release obligation). Add the case C-m9 says is missing: a
   signature move must produce `Releases == 1` and must still do so **after** a `Reset()`.
2. **C-M2** — `ProgramEmit.h`: invalidate the global-constants latch whenever `AcquireShaderCso` actually
   re-issues a create (wire's W6). Add a case: emit constants, force a link-version move, emit again,
   assert the constants went out a second time.
3. **C-M3** — close the eviction/`BuiltinSampler` hazard. Preferred shape: a monotone "sampler-CSO epoch"
   the cache bumps in `Evict`, which B's texture-params latch mixes in, so an eviction re-emits
   `set_texture_params` through D-E2's existing `SamplerResync`. Whatever shape is picked, it needs a
   case that evicts a handle a texture-params record names and proves the record is refreshed.
4. **The c0b reconciliation, §6 items 1–8** — three entry points, four latch calls, two false comments,
   one sentence for D.
5. **§7** — hand B the exact call and the include; B deletes its own `SamplerCso` mint. Re-run the
   include-closure gate with `--expect-probes 4`.
6. **Minors worth taking in the same round because they are one line each**: C-m6 (counted refusal
   instead of a compiled-out assert), C-m7 (`EmitterScope` on the redundant-rebind case), C-m4 (delete
   `Forget` or call it), n1 (one sentence on `Evict`'s copy-assign).
7. **Minors to declare and defer**: C-m2 (the per-unit acquire latch — real ID-19 budget item, record it
   in `MEASUREMENTS.md` rather than leaving it silent), C-m5, C-m8 (either a test seam for the collision
   branch or an honest rename), C-m1 (the dispatch-through-a-pipeline window).
8. **Not C's, hand to the integrator**: **C-M4** and **C-M5** are `Tracker.h` / `PipeFill.cpp`, which A
   owns for the whole phase. Both are one-line fixes and both are under-fires, which `Tracker.h`'s own
   text calls the fatal direction. They should land as a `c0d` on `p4a/contract` before `esprytdraw` is
   verified, not after.

---

## 10. What the integrator must re-run on the integrated tree

1. **Before anything else** — `MGPTextureParams::BuiltinSampler` is non-null **and resolvable** on every
   `set_texture_params`: `grep -c 'Fatal{' ` on an `integration-verify` run must stay 0, and B's texture
   cases must show the handle coming from `MGPipeSamplerCsoCacheInstance()`. This is §7 and it is the
   single highest-risk seam in the phase.
2. `ctest --test-dir build-push -R 'SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.'` — on
   C's branch **no case ever reached a real applier**; with `wire` underneath, the `Start + Count`
   Fatal, the `Parameters.Size` Fatal, the `link/spirv` non-null Fatal, the band's table split and the
   verify build's program-archive round trip all come alive for the first time.
3. The whole of C's §5 transcript, in order: three builds, G1 (0/0/0/0 — P4a's admitted set is EMPTY),
   `gen_pipe.py --check`/`--self-test`, the generated-file diff, `gen_pipe_dirty_surface.py`,
   `check_include_closure.py --mode both --compiler clang++ --self-test --require-all --expect-probes 4`
   (the new flag), `p3a_untouched_regions.sh 37da3c3a HEAD`, `ctest -L unit` ×3, the pull/push ctest-name
   parity pair and the no-name-removed check, the three `integration-gpu` arms
   (default / `0` / `0x1ff`), `integration-verify` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`
   with `grep -c 'Fatal{'` = 0, and the retrace gate.
4. **A DirectVulkan arm for the death/leak cases** (ID-8) — confirm F's G8b cases really run there, or
   say explicitly that they do not (C-m10).
5. **After `esprytobj` + `esprytdraw`**: `MOBILEGL_PIPE_PUSH=0x9ff` (samplers set, texture resources
   clear) must be **refused at the contract dependency rule** and logged, never half-run — C emits
   `MGPBoundView::Texture` and `MGPImageView::Res` as `Texture` handles unconditionally
   (`SamplerEmit.h:572`, `ImageEmit.h:97-99`).
6. **A pipeline (SSO) scenario and a `glBindSampler` scenario on the integrated tree** — C-M4 and C-M5
   are invisible to every unit suite in the phase, by construction, because the unit cases drive the
   emitters directly. `ProgramPipelineScenario`'s sixteen cases and any scenario that rebinds a *different*
   sampler object to a unit are the only things that can see them.
7. `retrace_gate.py`, **after** confirming `tools/trace_replay/fixtures` are not LFS pointers (D11) — the
   false red is `passed 2 / 79` with `ssim=None`.
