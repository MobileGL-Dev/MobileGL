# P4a package B — `clientfb`, focused re-review v2

Reviewed: `refs/heads/p4a/clientfb` @ **`d1215fbc`** against `clientfb-review-v1.md` (M1–M4, nine minors,
the c0b/c0c reconciliation, D4), `clientfb-v2.md`, and the rulings ID-8 / ID-12 / ID-14 / ID-17 / ID-18 /
ID-19(c) / ID-21 / ID-25 / ID-27 / ID-28 / ID-33 / ID-34 / ID-35.

**Method — this round is a REPLAY, not a read.** B's eight commits were cherry-picked onto the real pipe
head `92dffb81` in a private worktree (`p4a/bint-review`), built (push, verify and pull), and the four lanes
run. Every number below is off that tree. Every finding carries file:line, a failure scenario and the
refutation I tried. The worktree and its three build dirs were removed afterwards.

---

## Verdict: **ACCEPT WITH MINORS**

All ten items of the v1 review and every ruling are closed, and closed by code rather than by argument:
the region tail reaches the applier and the case asserts on what the applier **stored**; the three
`GL_Texture.cpp` hooks cover every path that writes the built-in sampler and the latch reads both
counters; the clears are acceptance-gated with a refusal case that constructs a real refusal; the metadata
respecify moves the hint on an immutable texture without eating its pending upload; the Named sites cover
every DSA framebuffer entry point in the tree. Nothing new was introduced: G1's mechanism is intact
(every MG_State and GLImpl statement is inside `#if MOBILEGL_PIPE_PUSH`, the **pull build and its 1759-case
unit lane are green on the replayed tree** and G2 is 0 diff lines), the exit-order rule holds, MGLOG
discipline holds, and the file list is B's plus the two ID-8 grants.

The five findings are minors and one of them is a documentation inaccuracy in B's own report that the
integrator should not carry forward as a proved property.

**Both of B's findings are confirmed and neither is a B defect** — §5.

---

## 1. The lane numbers on the replayed tree

Replayed HEAD `aa0fd5aa` = `92dffb81` + B's seven cherry-picked commits (§4).

| lane | replayed tree | B's report (its own base) |
|---|---|---|
| `build-push` configure + build | **rc 0** | rc 0 |
| `build-verify` configure + build | **rc 0** | rc 0 |
| `build-linux` (pull) configure + build | **rc 0** | rc 0 |
| `ctest -L unit` **build-push** | **1759 / 1759** | 1756 / 1756 |
| `ctest -L unit` **build-verify** | **1759 / 1759** | 1750 / 1756 — *the six vanish, §5(b)* |
| `ctest -L unit` **build-linux** | **1759 / 1759** | 1756 / 1756 |
| **G2** pull vs push ctest name sets | **1759 == 1759, 0 diff lines** | 0 diff lines |
| emit suites (the eight `*Emit` + `CompositeResolver`) | **152 / 152** | 148 / 148 |
| `integration-gpu -R DirectGLES -j 8`, **default mask** | **444 / 491 — 47 failures** | 444 / 491 |
| the 47 by name vs `clientfb-v2.md`'s appendix | **IDENTICAL, 47 == 47, 0 diff** | — |
| `integration-gpu`, **`MOBILEGL_PIPE_PUSH=0x1ff`** | **491 / 491** | 491 / 491 |
| `integration-gpu`, **`MOBILEGL_PIPE_PUSH=0x3ff`** | **491 / 491** | 491 / 491 |

The three-count deltas (1756 → 1759, 148 → 152) are C v3's own new cases, which are on `92dffb81` and were
not on B's temporary base. **The default arm's shape and count reproduce exactly**, which is what makes
§5(a)'s ruling checkable rather than reported.

---

## 2. Findings

### n1 — `clientfb-v2.md` §2(5)'s M3 sentence is not what the code proves. `TextureEmit.h:1064-1097`

The report states *"`dispatched` is the `if constexpr` itself, so a discarded call never clears either."*
The code is

```cpp
Bool accepted = false; Bool dispatched = false;
if constexpr (MGPipeTextureRecordsReachTheApplier()) { dispatched = true; accepted = MGPipeApplyResourceSubData(...); }
...
if (dispatched && !accepted) { ++m_refusedSubDatas; MGLOG_E_ONCE(...); return false; }
mipmap->MarkStorageDirty(uploadTarget, level, false);
```

so with the constant back at 0 `dispatched == false`, the guard is false, and the flag **is** cleared. The
same shape is in `PublishCreate` (`:1050`, `if (dispatched && !accepted) return;` — a discarded create still
takes `MGPipeNoteHandlePublished`) and `NoteRespecified` (`:1069`, the discarded arm always advances
`LastDesc`).

*Failure scenario.* The A/B arm the predicate exists to keep compilable — an operator or a bisect setting
`kMGPipeWiredTextureSubsystem = 0` — would clear a level's dirty flag with the record nowhere, i.e. v1's M3
data loss reintroduced by the very arm that is supposed to be inert, and `MGPipeHandleIsPublished` would
answer true for handles the applier never received.

*Refutation attempted, and it succeeds — for reachability, not for the sentence.* `PipeFill.cpp:1059-1092`
gates all six birth hooks on `FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)`
and forwards through `ForwardWhenWired<kWired>`; with the constant 0 `NoteLevelDirty` is never reached,
`m_drain` is permanently empty, and `DrainTextureSubData` (`TextureEmit.h:786`) returns at
`if (m_drain.empty())` before `EmitOneLevel` exists on the call graph. `PublishCreate` and `NoteRespecified`
are unreachable for the same reason. So all three sites are dead code on that arm and the property holds —
**because the family is gated off upstream, not because of `dispatched`.**

**Not a defect.** But the integrator should not carry the sentence forward as a local guarantee, and
`d1215fbc`'s comment ("the two places a discarded call may not be treated as an accepted one") names
`PublishCreate` and `NoteRespecified` while the third site, the one M3 is actually about, reads the other
way. One line — `if (!dispatched || !accepted) return false;` — makes the paragraph true at zero cost on
the live arm; `PublishCreate` / `NoteRespecified` must keep their current shape, since on the A/B arm their
"pretend it landed" bookkeeping is what keeps the dedupe coherent.

### n2 — a bound-target record emitted from a DSA site latches on dispatch, not on acceptance. `FramebufferEmit.h:319-360`

`EmitFramebufferByName` writes `m_lastEmitted[kDraw|kRead]` (or `m_named[slot]`) from `Emit()`, which counts
bytes. `MGPipeApplySetFramebufferState` returns `void` and can still refuse — a null handle,
`Fbo.Slot >= kMGPipeMaxFramebufferSlots`, or an out-of-range `DrawBuffers[i]` (`PipeApply.cpp:2157-2190`).
A refused record therefore latches and is never re-sent until its content moves: M3's class, one family over.

*Refutation, and it holds today.* Two of the applier's three refusals are unreachable from this client:
`HandleFor` (`:254`) returns `kMGPipeDefaultFramebuffer` `{0,1}` or an allocator handle, and
`MGPipeSlotAllocator::Allocate` documents and enforces *"never returns slot 0 (reserved: null, and the
default framebuffer for kind Framebuffer)"*, so the null-handle arm cannot fire; and m2's client-side gate
(`FramebufferEmit.h:160-172` + `:479-495`) refuses an over-wide draw-buffer token before the record is
built. The third needs 65 536 live framebuffers and is a loud `MGLOG_E` on the far side. Pre-existing in v1,
untouched by the rework, and out of B's scope — **record it for wire's next round**, since the resource
family now has acceptance returns and this one does not.

### n3 — the built-in sampler's cache reference is given back only when the texture's SLOT is recycled. `TextureEmit.h:1017-1024`

`RetireIfRecycled` (and `ResetForTest`, `:911-918`) are the only `MGPipeSamplerCsoCache::Release` callers on
this side; I confirmed `MGPipeEmitTextureDestroyAndFree` (`MG_Impl/Pipe/PipeFill.cpp:1197-1228`) never
reaches this emitter, so a destroyed texture leaves one reference standing.

*Failure scenario.* A workload whose live-texture count falls — a dimension unload, a resource-pack swap —
parks (peak − live) references the LRU cannot evict; C's cache then mints over capacity and
`OverCapacityMints` / referenced evictions climb, unattributably.

*Refutation.* `NotifyAndFree` returns the slot to the free list at death, `MGPipeSlotAllocator::Acquire`
pops the free list before the high-water mark, and `AcquireTexture` runs `RetireIfRecycled` on the very next
texture creation — so the residency bound is the texture-slot **high-water mark**, which is exactly the bound
`m_textures` itself already carries. It is monotone-bounded, not a leak, and C v3 counts rather than asserts
(`92dffb81`). **Accepted as declared** (B says "bounded by live texture slots"; "peak" is the exact word).
The integrator should read C's two counters once D lands and the texture family runs real traffic.

### n4 — the never-destroyed singleton's stated reason is now false. `TextureEmit.h:1119-1124`

The comment justifies "never destroyed" with *"~TextureObjectBase reaches this emitter through the death
helper's RecordIsPublished / NoteRecordDestroyed pair, which read and WRITE its tables"*. That pair was
deleted in this very rework (the latch is A's), and the death helper does not touch the emitter at all.
Documentation only: ID-8's rule is unconditional for every MGPipe process singleton, the object is still
`new`'d and never deleted, and `FramebufferEmit.h:631-637` states the unconditional reason correctly.
Reword at the next touch of the file.

### n5 — `RepublishMask` now runs inside the framebuffer record build. `FramebufferEmit.h:539+` → `TextureEmit.h:1084-1101`

`SurfaceOf` calls `NoteTextureBoundAs`, which after M4 can emit a `resource_respecify`. So attaching a
texture to an FBO emits a resource record from inside `BuildFramebufferState`'s attachment walk.

*Refutation, and this one is right by construction.* The ordering is the correct one — the resource record
precedes the surface that names it — there is no re-entrancy (`RepublishMask` touches only `m_textures` and
the applier; `BuildFramebufferState` holds no reference into either), and `NoteTextureBoundAs`'s
`if (now == before) return;` makes every attachment after the first free. **Not a defect**; recorded because
it is a new emission on the validate-point path whose cost will show up in D's payload histogram, and
because on the `0x3ff` arm `RepublishMask`'s own `MGPipeTextureSubsystemEnabled()` gate is what keeps it
silent (verified: 0x3ff is 491/491).

---

## 3. The per-item closure table

| item | verdict | evidence on the replayed tree |
|---|---|---|
| **M1** region tail reaches the applier | **CLOSED** | `TextureEmit.h:1068-1069` passes `m_regions.empty() ? nullptr : m_regions.data()`. `TextureEmit.TheApplierStoresTheRegionListTheEmitterBuiltAndNotAnEmptyOne` (`TextureEmitTest.cpp:731-765`) asserts on `MGPipeApplier().TextureResources[slot]`'s `PendingUpload::Regions`, field by field (X, Y, W, H, SrcOffset, SrcRowStride, SrcSliceStride) — **the STORED list**, and it first asserts `RefusedSubDataCount() == 0` so a declared-but-missing tail fails loudly. |
| **M2** three `GL_Texture.cpp` hooks | **CLOSED** | **All 36** `GetSamplerObject()->Set*` writes in the file (`:1262-1320`, `:1379-1422`, `:2083-2142`) are inside one of the three hooked functions `TextureParameterObject_State` (hook `:1365`), `TextureParameterObjectf_State` (`:1467`), `TexParameterf_State` (`:2181`). Every other family funnels in: `TexParameteriv/IIv/IUiv → TexParameteri_State:2196 → TextureParameterObject_State`, `TexParameterfv:2233 → TexParameterf_State`, DSA `:6334/:6340 → the two Object forms`. **Every `return;` before the hook in all three switches is a `RecordError` arm** (checked at `:1296 :1331 :1341 / :1433 :1443 / :2115 :2157`); every success arm `break`s and reaches the hook. The four border-colour helpers at `:44-70` are outside the three, and correctly so: `MGPTextureParams` (`MGPipeTypes.h`) carries `Res, BuiltinSampler, BaseLevel, MaxLevel, Swizzle[4], DepthStencilMode, ForceResync, SamplerResync, MinLod, MaxLod, LodBias` and **no border colour**, and `MGPipeBuildTextureParams` (`TextureEmit.h:303-331`) reads only those three floats off the sampler object. So the record has no field a path outside the hooks can move. |
| **M2** both-versions latch | **CLOSED** | `TextureEmit.h:660-673` reads `GetTextureParamsVersion()` **and** `sampler->GetVersion()` **and** `ForceParamsResync`. `ALodWriteOnTheBuiltinSamplerRepublishesTheParams` asserts the texture's own version did **not** move before driving the sampler write — the case cannot pass for the wrong reason. MinLod/MaxLod/LodBias all asserted. |
| **M2** no double Release, no leak | **CLOSED (with n3)** | `:675-700`: one `Acquire` per emission; the duplicate reference is handed straight back when the value did not move, the previous handle released when it did; `SamplerEmit.h:305` makes `Release(null)` a no-op. Exactly one reference per entry. Death: n3. |
| **M3** clear on acceptance | **CLOSED (with n1)** | `TextureEmit.h:1090-1097`; `ARefusedUploadLeavesTheLevelDirtyAndOnTheDrainList` constructs a real refusal via `MGPipeApplierReleaseObjectRecords()` and asserts flag + drain entry + counter, then drives the self-heal. The if-constexpr-discarded case is n1. |
| **M3** refusal survives | **CLOSED** | as above; plus the second finding B made itself (a refused respecify republishes the create and retries once, `:600-621`) is pinned by the same case's second half. |
| **M4** metadata respecify | **CLOSED** | `RepublishMask` (`:1084-1101`) starts from `entry.LastDesc` and writes only `BindMask` + `ImageBindableHint`. `AnImmutableTexturesImageBindableHintReachesTheApplierAfterItsAllocation` (`:819-853`) allocates immutable storage, stands a pending upload, then image-binds, and asserts on the **applier's** record: hint 1, mask set, `Width` unmoved, serial advanced, pending upload survived. |
| **ID-19(c)** the Named sites | **CLOSED** | All 23 `Named*Framebuffer*` symbols in the tree enumerated. **17 call sites over 12 entry points**, not sixteen — B's table counts `glBlitNamedFramebuffer`'s two objects as one row. Mutators, published after the mutation: `:1461 :1490` (Texture), `:1516 :1546` (TextureWithUploadTarget, i.e. the 1D/2D helper), `:1603 :1708` (TextureLayer), `:1780 :1792` (Renderbuffer), `:1994` (DrawBuffers), `:2011` (DrawBuffer), `:2023` (ReadBuffer) — 11. Consumers, published before `MGP_FILL`: `:672 :673` (blit, both objects), `:688 :702 :716 :730` (the four clears) — 6. **Nothing is missed:** every `->AttachTexture` / `->AttachRenderbuffer` / `->Detach` / draw- or read-buffer mutation in a `Named*` entry point is followed by a publish. `NamedFramebufferTexture3D` is a pure `RecordUnsupported…` stub; `InvalidateNamedFramebuffer{Data,SubData}` validate only; the three getters and `CheckNamedFramebufferStatus` are queries. `NamedFramebufferParameteri` is declared-not-hooked and that is **safe by a stronger argument than B gives**: it moves only the no-attachment defaults `FillGeometry` reads, and every *consumer* republishes before handing the object over, so a stale record can never reach the server. |
| ID-19(c) default framebuffer | **CORRECT, not wrongly emitted** | Every `framebuffer == 0` path reaches `pDefaultFramebufferInfo->defaultFBO`, `HandleFor` returns `kMGPipeDefaultFramebuffer {0,1}` and `IsDefault = 1` with the colour surface under `Color[0]` (`:504-512`). That is the record ID-19(c) wants for `glClearNamedFramebufferfv(0, …)`. No collision in `m_named[0]`: the allocator never returns slot 0 for kind Framebuffer. |
| ID-19(c) ReadSurface | **CLOSED** | `out.ReadSurface = SurfaceOf(fbo, fbo.GetReadBuffer())` (`:520`); `BuildFramebufferState` lost its `readFbo` parameter entirely, so the v1 shape is unrepresentable. |
| ID-19(c) sticky mask + content hash | **CLOSED** | `MGPipeCopySurfaceForHash` copies the new `TextureTarget` (`:205-209`); `Target` is a hash input, so a Named record can never be suppressed against a bound one; the per-object `m_named` table with the generation checked buys the other direction; `Reset()` clears it in the safe direction. |
| ID-19(c) no Named record for a null handle | **CLOSED** | `PipePublishFramebufferByName` returns on `!fbo`; `HandleFor` cannot produce null. |
| **c0b** no MG_State TU includes an emitter | **CLOSED** | `grep -rn "MG_Impl/Pipe/" MG_State/` returns three **comment** lines and no `#include`. `RenderbufferObject.cpp` now includes `MG_Pipe/PipeMutation.h`. |
| **c0b** helper/latch deletions | **CLOSED** | all eight free functions, `Entry::Published`, both `RecordIsPublished`, both `NoteRecordDestroyed`, `PublishedIn`, `Retire`, `ArmForTest`/`m_armed` gone; both destructors call one helper. |
| **c0c** helper/constant deletions | **CLOSED** | `MGPipePackSubDataTarget`/`…ResourceTargetOf`/`…UploadTargetOf`, `kMGPipeDepthStencilModeDepth/Stencil`, `kMGPipeSurfaceKind*` all deleted; `MGPipeDepthStencilModeByte` kept. |
| **c0c** `TextureTarget` filled, 0xFFFF elsewhere | **CLOSED** | `MGPipeEmptySurface()` (`:113-118`) sets `kMGPipeSurfaceNoTextureTarget` (= 0xFFFF, `MGPipeTypes.h:533`) **and** `TextureUploadTarget::Unknown`, used on the empty point and both `SurfaceOf` early returns; the texture arm writes `GetTarget()`. |
| **D4** flip verified, expected-failing set empty | **CLOSED** | `kMGPipeWiredTextureSubsystem = kMGPipeSubsystemTextureResources`; the five named cases all pass in the 1759/1759 push lane, and again in verify and pull. |
| **the six minors fixed** | **CLOSED** | m1 `targets.size() == 1` (`:88-91`); m2 the second refusal loop (`:479-495`); m3 the owner acquired before any `Entry&` (`:544-590`); m4 `RetireIfRecycled` in both `Acquire*`; m5 dissolved by c0e's per-object rule; m6 the Unknown sentinels. Each has its own named case. |
| **the three declared** | **JUDGED SOUND** | §3 below. |
| **exit order (ID-8)** | **HOLDS** (n4 on the comment) | both emitters `new`'d and never deleted; `Entry::Texture` is a raw pointer; the new `Entry::BuiltinSampler` is a POD handle — no new static holder of a frontend `SharedPtr`. |
| **MGLOG discipline / no leftovers** | **CLEAN** | B's two headers contain exactly four log statements, all `MGLOG_E_ONCE` on error paths (`TextureEmit.h:623 :1092`, `FramebufferEmit.h:472 :489`); B's added lines in the two GLImpl files contain **no** log statement at all; no `TODO`/`FIXME`/`printf`. |
| **files vs ownership** | **CLEAN** | 15 files: B's six MG_State + two MG_Impl/Pipe + two MG_Test/Pipe + the granted `MG_Test/State/ObjectLifetimeIdTest.cpp` + the two ID-8 grants `GL_Texture.cpp` and `GL_Framebuffer.cpp`. Nothing outside. |
| **clientsp v3 API shapes** | **CONFIRMED** | B calls `MGPipeSamplerCsoCacheInstance().Acquire(params, Uint64& payloadBytes)` and `Release(handle)` — `SamplerEmit.h:266` / `:305` on `92dffb81` — plus `RefCountOf` / `RecordIsPublished`, which is exactly the seam C v3 names as B's. Nothing else. It compiles and the emit suites are 152/152, so the v2→v3 move did not touch it. |
| **wire v3 acceptance returns** | **CONFIRMED** | `PipeApply.h:786 :834 :863` all return `Bool` on the pipe head; B reads all three. |

### 3.1 The three declarations, judged

- **m8** (`SetInternalFormat`'s zero-extent respecify) — **accept.** Gating it needs a `TextureObjectBase`
  member, which resizes the pull build's object and breaks G1; every applier body tolerates a zero-extent
  respecify, and it is not byte-equal to the create so it is a real record rather than a duplicate.
- **m9** (no DSA-named path in G6's case) — **accept, and it is now stronger than a declaration:** ID-19(c)'s
  two new cases (`AFramebufferHandedOverByNameGetsANamedRecordWithoutMovingABinding`,
  `ANamedRecordIsSuppressedPerObjectAndNeverAgainstABoundRecord`) give the DSA paths coverage one level up.
- **D14** (the three `GL_Texture.cpp` sites unpinned by a unit case) — **accept.** A `MG_Test/Pipe` binary
  cannot drive `glTexParameterf`; the unit case drives the exact call the sites make, and the wiring rides
  the integration lane's texture family. Note this is exactly the class of thing the default arm will
  exercise once D lands.
- **D15** (the ten mutators publish after the mutation) — **accept.** `FillPoints.def` is A's and has no verb
  for any of them; "the point at which the object is final for this call" is the only available reading, and
  the five **consumers** — the calls that actually hand the object over — do publish before `MGP_FILL`.
- **D12** (no clean arm on the drain list) — **accept, verified.** `EmitOneLevel`'s third statement is
  `if (!mipmap->IsStorageDirty(uploadTarget, level)) return true;` and `true` drops the entry from both
  lists; `ALevelMarkedCleanIsCollectedAtTheNextDrain` pins the collection **and** the re-append after a later
  write, which is the half that would have made this a texel-loss bug.
- **D13** (clientsp merged as a fourth head) — **dissolved by the replay.** On `92dffb81` the cache is in the
  tree; the merge disappears with the other three.
- **D16 / D17** — recorded; D17 (the dedupe can suppress a respecify the applier no longer holds) is honest
  and its blast radius is bounded by the loud refused-upload path that heals it.

---

## 4. THE VERIFIED REPLAY RECIPE

Verified end to end: it applies with exactly **one** conflict, builds in all three configurations, and
reproduces B's lane numbers.

```sh
cd /home/swung/w7/pipe
git worktree add -b p4a/bint-review /home/swung/w7/p4a-bint refs/heads/feat/disaggregated   # = 92dffb81
cd /home/swung/w7/p4a-bint
git submodule update --init --recursive
cp -a /home/swung/w7/pipe/3rdparty/glslang/External 3rdparty/glslang/
cp -a /home/swung/w7/pipe/tools/trace_replay/fixtures/. tools/trace_replay/fixtures/
git ls-files -m tools/trace_replay/fixtures | xargs -r git update-index --assume-unchanged

# B's three v1 commits, as they sit on the temporary base:
git cherry-pick -x 5e87d3d2     # mint {slot,gen} for texture + renderbuffer        -> clean
git cherry-pick -x 5fac004a     # push the bound framebuffers                       -> clean
git cherry-pick -x e2bf183a     # wire bit 9 + the emitter cases                    -> CONFLICT, see below

#   549ef30b IS SKIPPED — see §4.1

# B's four remaining rework commits:
git cherry-pick -x a390bb1a     # birth hooks, D4 flip, region tail, acceptance     -> clean
git cherry-pick -x d013b7ec     # the two granted GLImpl files                      -> clean
git cherry-pick -x 4ec43194     # the twelve new cases and the six changed          -> clean
git cherry-pick -x d1215fbc     # the docs refresh                                  -> clean
```

Configure with `~/w7/notes/tools/wsl_tree.sh`'s `COMMON` flags, `CCACHE_BASEDIR=/home/swung/w7`, `-j 8`:
`build-push` (`-DMOBILEGL_PIPE_PUSH=ON`), `build-verify` (`-DMOBILEGL_PIPE_VERIFY=ON
-DMOBILEGL_ITEST_REQUIRE_GPU=ON`), `build-linux` (no extra flag).

### 4.1 `549ef30b` must be SKIPPED, not resolved

It retires wire v3's local `kMGPipeFramebufferTargetNamed` for c0e's enumerator. **Wire v3 already did
that itself on the pipe** (`712c9467`, ID-21/ID-27/ID-28): the constant does not occur anywhere in
`92dffb81` — `git grep kMGPipeFramebufferTargetNamed 92dffb81` is empty — and wire's own cases already spell
`static_cast<Uint8>(MGPipeFramebufferTarget::Named)`. Cherry-picking it conflicts in `PipeApply.h` against a
hunk that no longer exists. B's own §7 item 7 anticipated exactly this. `git cherry-pick --skip`, or simply
omit it from the list as above. Nothing in B's remaining commits references the retired constant.

### 4.2 The one conflict, and its recorded resolution

`e2bf183a` conflicts in **`MG_Test/Pipe/FramebufferEmitTest.cpp`** and **`MG_Test/Pipe/TextureEmitTest.cpp`**
— wire's cases (already on the pipe, from `ae1a1c50` / `5ab90dec` / `8f1eaafa`) and B's cases are appended at
the same anchor.

**One conflict hunk per file, and the base region of each is EMPTY** (Framebuffer: ours 359 / base 0 /
theirs 360; Texture: ours 620 / base 0 / theirs 477) — i.e. both sides are pure additions with nothing
removed, the same shape B resolved its own wire merge with. **Resolve as an overlap-checked union with
B's block FIRST** (`theirs` before `ours`), which reproduces the layout `p4a/clientfb` itself has: on B's
branch its own cases sit above wire's (`TextureEmitTest.cpp:319..991` are B's, `1074..` are wire's).

> **This ordering is load-bearing.** Resolving `ours`-first also compiles, but the later `4ec43194` then
> fails to apply (4 hunks, non-empty bases) because its context assumes B's block comes first. With
> `theirs`-first every remaining commit applies clean.

Checks run on the union result, all green: **0 duplicate `TEST(` names** in either file, exactly **1**
`MGL_*_CLIENT_TEST_LIST` macro per file (the macro region merged automatically, outside the conflict, and
carries both sets: 13 framebuffer + 21 texture client entries), and no overlapping helper names — B's side
adds `MakeTexture2D` / `MakeColorTexture` / `BindDrawAndRead`, wire's adds `TextureDesc` / `TextureParams` /
`TextureUpload` / `Region` / `TextureRecordOf` / `FramebufferRecord`, each inside its own
`namespace { … }` under its own `#if` guard.

### 4.3 Fidelity of the replay

`git diff d1215fbc <replayed HEAD>` over B's own paths is **empty**:
`MG_Impl/Pipe/TextureEmit.h`, `MG_Impl/Pipe/FramebufferEmit.h`,
`MG_Impl/GLImpl/Texture/GL_Texture.cpp`, `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp` and **all of
`MG_State/`** are byte-identical. The only residue is wire v3's and C v3's own tails
(`CompositeResolver.h`, `SamplerEmit.h`, `ProgramEmit.h`, `PipeApply.{h,cpp}`, C's four test files) plus
3 lines in the two conflicted test files: wire's Named-constant retirement applied to **wire's own** cases,
and one blank line from the union. So the replayed tree carries B's work exactly.

---

## 5. B's two findings, checked

### (a) The 47 default-arm failures are bit 10 with no server consumer — **confirmed, and it is NOT a B defect**

Reproduced exactly: **444/491, and the 47 names are identical to `clientfb-v2.md`'s appendix, 0 diff.**
`0x1ff` and `0x3ff` are both 491/491 on the replayed tree, so bit 9 — every line of B's framebuffer work
including the 17 Named sites — is independently clean and the cause is bit 10 alone.

**An independent negative control, stronger than B's probe-variable one.** On this tree
`MGPipeResourceRecord::PendingUploads` is read by `MG_Pipe/PipeApply.{h,cpp}` (the applier's own storage),
`MG_Test/Pipe/TextureEmitTest.cpp` and `MG_Impl/Pipe/TextureEmit.h` — **and by nothing in `MG_Backend/`**
(`VkTextureManager::FlushPendingUploads` is DirectVulkan's unrelated internal staging, not the applier's
set), and no file under `MG_Backend/` reads `MGPipeApplier().TextureResources[…]` at all. So the accepted
uploads land in a set that has no reader: package D is the consumer and is not in this tree. The client half
of D-D5's inversion is present and correct and is destructive on its own, exactly as ID-35 rules.

**Concur with ID-35 and with B's recommendation 1**: the one line `TextureEmit.h:98` is the whole switch, so
moving the flip into D's landing commit keeps every intermediate tree green and keeps a bisect meaningful.
If it stays in B's commit, the B→D window is knowingly red on the default arm and the retrace and must be
gated at `0x1ff`.

**D-K2 gains the row B asks for, and the wording is right:** *bit 10 requires the server-side texture
consumer*, and it is the only P4a subsystem bit that is not independently switchable, because it is the only
one whose emission **takes state away from the legacy path** rather than adding a record the server may
ignore. (Bits 9, 11 and 12 are each 491/491 alone.)

### (b) The six verify-lane unit failures vanish on `92dffb81` — **confirmed**

`build-verify` on the replayed tree: **1759 / 1759, zero failures.** All six of B's named cases pass:

```
SamplerEmit.ARecordThatDoesNotDescribeItsOwnParametersIsRefusedNamingTheLength      Passed
SamplerEmit.AUnitWindowPastTheMergedUnitSpaceIsRefusedRatherThanTruncated           Passed
ImageEmit.AnImageWindowPastTheImageUnitSpaceIsRefusedRatherThanTruncated            Passed
ProgramEmit.ACreateWithNoArtefactsAnOversizedBlockOrACorruptSlotIsRefusedNamingTheProgram  Passed
ProgramEmit.TheDefaultUniformBlockLandsOnTheProgramsRecordAndTheSentinelIsRefused   Passed
CompositeResolver.ASlotAtTheShaderCsoLimitIsRefusedWhileTheLastBandSlotIsNot        Passed
```

They were the wire × clientsp-v2 seam that C v3's `79b58198` fixed (*the forked refusal drive unlinked the
log while `Initialize()` held it open, so the child wrote into a deleted inode*) — the empty-child-log shape
B described. B's control run was right and its diagnosis was right. **Nothing owed here.**

---

## 6. B's post-integration checklist

1. **Decide the flip's home first** (§5a). Recommendation: move `TextureEmit.h:98` into D's landing commit.
   If it stays with B, gate the B→D window's default arm and the retrace at `0x1ff` and say so in the log.
2. **Skip `549ef30b`** and use §4's cherry-pick list; resolve the one test-file conflict **theirs-first**
   per §4.2 — `ours`-first breaks `4ec43194`.
3. After the merge, re-run what this round could not: **G1** (`symbol_report --threshold 0` vs
   `~/w7/p4a-before-libMobileGL.so`), **G5** (`scripts/p3a_untouched_regions.sh 37da3c3a HEAD`),
   `gen_pipe.py --check`, `gen_pipe_dirty_surface.py --check`, and
   `check_include_closure.py --mode both --compiler clang++ --self-test --require-all` (B's new
   `TextureEmit.h → SamplerEmit.h` and `GL_Framebuffer.cpp → FramebufferEmit.h` edges). G2 is already
   verified on the replayed tree at 0 diff lines.
4. **After D lands**, and these are the checks B's work is actually gated on:
   - the default arm and the retrace back to **491/491 and 79/79**;
   - the **residual-record sweep** (ID-12): the 183 "no applier record" refusals to zero, counting all
     **eight** families per ID-34's N-2 — a residual one on a texture or renderbuffer handle is a B seam
     defect;
   - `PipeStats::ClientTextureUploadEmissions` against the server's `TextureUpload{Emissions,Box,Rect,Jobs}`
     on the 79-case retrace: **a rect count of zero on `rei`/`xaero-*` means M1's tail is still not
     arriving**, and it is the only place the region list is exercised at volume;
   - C's `OverCapacityMints` and referenced-eviction counters, for n3.
5. `integration-verify` on the **DirectVulkan** lane under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`
   for ID-8's per-kind leak test — still owed; this tree's `integration-verify` label runs DirectGLES.
6. Package F's `0x5ff` / `0x9ff` dependency-refusal arms cannot be read off any tree before D
   (`0x5ff` is 444/491 for §5a's reason, not for D-K2's).
7. Optional, and one line each: n1's `EmitOneLevel` guard, n4's comment. n2 goes to wire's next round.

---

*Reviewer note.* Replay worktree `/home/swung/w7/p4a-rereview-clientfb` on branch `p4a/bint-review`, its
three build directories and all scratch logs were removed; `git worktree prune` run and the branch deleted.
No device work. `~/w7/pipe` and `~/w7/p4a-clientfb` were read only.
