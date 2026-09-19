# P4a package D — `p4a/esprytobj` rework v2, focused re-review

Reviewed: `refs/heads/p4a/esprytobj` `cce81d19..ef6beb28` (the seven rework commits) against
`esprytobj-review-v1.md`, `INTEGRATOR-DECISIONS.md` ID-12 / ID-15 / ID-18 / ID-19 / ID-23 / ID-27 /
ID-28, `wire-v3.md` + `wire-review-v2.md`, `contract-v3/v5.md`, `gates-review-v1.md` R1,
`gates-review-v2.md`, `esprytdraw-v2.md`. Read in full: the whole `git diff cce81d19 HEAD`
(Managers.cpp +892 / Managers.h +98 / SanityTest.cpp +218) plus the surrounding bodies of every
function it touches, `PipeApply.{h,cpp}`'s `FramebufferRecordFor` / `AccumulatePendingUpload` /
`SubDataTextureFault`, `SlotTables.h`'s handle overloads, and — only to refute findings —
`p4a/clientfb`'s `TextureEmit.h` and E's `DirectGLES.cpp` call sites.

Experiments in a private worktree `~/w7/p4a-rereview-esprytobj` (**removed at the end of this
round**). Every number in §4 was measured **on the rebased tree** — D's thirteen commits replayed
onto the real pipe head `712c9467` — not on the temporary merged base.

## Verdict: **ACCEPT WITH MINORS**

The critical and all four majors are closed, and closed the right way rather than papered over.
ID-12, ID-15's fourth row, ID-18's M4, ID-19(d), E's item (4) and the R1/G9 probe are all in and I
could break none of them. Nine minors below; **two of them are owed before the verification round**
(N-1's log wording, N-2's census under-count, because the integrator's residual-refusal check is
written from that census and would miss two of the eight shapes). Nothing blocks the merge.

**The one thing the integrator must not do is run ID-32's rebase command as written** — it fails,
and §5 gives the recipe that works and builds.

---

## 1. Per-item closure table

| v1 item / ruling | verdict | where |
|---|---|---|
| **CRITICAL** (C-1) framebuffer record keyed by the bound target | **CLOSED** | `PushedFramebufferRecord(fbo)` = `MGPipeApplier().FramebufferRecordFor(fbo)` (`Managers.cpp:8803`, decl `Managers.h:1879`). The `asTarget` parameter and the `record.Fbo == fbo` test are gone; both DSA entry families reach it through `SyncToBackend` unchanged. `FramebufferRecordFor` (`PipeApply.cpp:2235`) is silent for the null handle and for an undescribed slot and **loud + counted** (`StaleFramebufferRecordLookups`, `MGLOG_E_ONCE`) on a stale generation — exactly what `Managers.h:1888-1898` claims |
| … refusal BEFORE the bind | **CLOSED in the twin**, but see **N-1** | the record block is `:9054-9091`, `Bind(asTarget)` is `:9092`. The claim the refusal line makes about the *resulting driver state* is false at every call site |
| … the record's `Target` no longer compared to `asTarget` | **CLOSED** | the old `expected`/`Both` block is deleted. What stays gated on `asTarget` is what reaches the driver's bound target: `glDrawBuffers` (`:9151`), the four masks (`:9159-9192`), `glReadBuffer` (`:9220`) |
| **ID-27** — any remaining `record.Target == Both`-style test in D | **NONE** | `grep -n 'Target *[=!]=' Managers.cpp` at HEAD: 20 hits, every one of them `glTarget`/`fbTarget`/`uploadTarget`/`glTextureTarget`/`asTarget`/`surface.TextureTarget`/`color.UploadTarget`. `MGPipeFramebufferTarget` appears **only** at `:8814-8815`, as the `BoundFramebuffer` index. D has nothing of E's MAJOR-1 shape |
| **M-1** server re-dirty arms the applier's `PendingUploads` | **CLOSED** | `RearmPipeTextureLevelUpload` (`:3834`, decl `Managers.h:717`), called from `RequireImageBindableStorage` (`:5277`). Entry is whole-level, `RegionCount 0`, **merged** with an existing entry, loud refusal at `kMGPipeMaxPendingUploads` (256). The three server-side `MarkStorageDirty` sites are enumerated at `:3836-3856` and the third is correctly identified as the *clear* half |
| … keyed with the PACKED target so the consume site finds it | **YES, verified end to end** | the write is `entry.UploadTarget = packedTarget` (`:3888`) built with `MGPipePackSubDataTarget(MGPipeResourceTargetForTextureTarget(GetTarget()), uploadTarget)` (`:5279`); `FindPipeTextureUpload` (`:3799`), `ConsumePipeTextureUpload` (`:3821`) and the re-arm's own scan (`:3872`) all decode with `MGPipeSubDataUploadTargetOf`. Same key, both directions. See **N-7** for the one asymmetry against wire |
| … consumed exactly once | **YES** | `ConsumePipeTextureUpload` swap-and-pops the first match and returns; at most one entry can exist per `(uploadHalf, level)` because `RearmPipeTextureLevelUpload` and wire's `AccumulatePendingUpload` both merge before inserting |
| … does the re-arm actually re-open the gate? | **YES** | the serial gate is `m_isInitialized && m_syncedResourceSerial == Serial && PendingUploads.empty()` (`:6353-6357`). `RequireImageBindableStorage` clears `m_isInitialized` **and** fills the set, so the gate misses on both counts. No recursion: the function early-returns on `m_imageBindableStorageRequired` |
| **M-2** storage from `Desc` | **CLOSED** | seven `MGB_STORAGE_*` macros (`:6244-6270`), applied at `currentTextureInfo`/the storage-kind switch, `mipmapCount`, `IsImmutable` at both allocator branches, the two multisample allocations, and all sixteen `GetFormat()` reads |
| … every `StorageKind` incl. buffer textures / multisample / cube / array | **YES** | buffer texture: `Desc.BufferForTexBuffer` + `EnsureBufferResourceForHandle` + the `BufOffset`/`BufSize` window with `kMGPipeWholeBuffer` resolved live (`:7324-7360`, `:7421-7443`); multisample: `MGB_STORAGE_SAMPLES` / `MGB_STORAGE_FIXED_SAMPLE_LOCATIONS` at `glTexStorage2D/3DMultisample`; cube/array: the base extent is `Desc.{Width,Height,Depth}` and **B fills those three from `GetBaseSize()` verbatim** (`p4a/clientfb:TextureEmit.h:185-188`), deriving `ArrayLayers` *in addition* rather than instead — so §5-item-7's feared seam does not exist. I checked it so the integrator need not |
| … metadata respecify re-derives flags without reallocation | **CLOSED, with N-4** | `:6335-6339`: `ImageBindableHint != 0 \|\| (BindMask & kMGPipeBindShaderImage)` and `!m_imageBindableStorageRequired` → `RequireImageBindableStorage`. Idempotent, no reallocation from the mask alone (the reallocation is the *widening*, which is what the flag means on this backend) |
| … `HasDefinedContent` honoured | **NOT READ, and not declared** — see **N-8** | it is storage-defining per c0e; the buffer family reads it (`:1911`, `:1956`, `:2846`), the texture arm never does and `:6230-6237`'s "what the descriptor does NOT answer" does not list it |
| **M-3** `SyncAttachmentObject` from `MGPSurface` | **CLOSED** | `PushedSurfaceForAttachment` (`:8605`) + `SyncAttachmentSurface` (`:8631`); call site `:9299-9322`. Res → twin by **adoption**, and `Layered`/`Level`/`Layer`/`UploadTarget`/`TextureTarget` off the surface |
| … `TextureTarget` for the four masks, D-N functions byte-identical | **CLOSED, shas recomputed** | `PushedSurfaceTextureTarget` (`:8539`) gates on `Kind == kMGPipeSurfaceKindTexture` and refuses `kMGPipeSurfaceNoTextureTarget` loudly. `ShouldUseCaveatTextureFormat` / `BackendTextureFormatAddsAlpha` and the two renderbuffer siblings are untouched — `p4a_untouched_regions.sh 37da3c3a HEAD` rc 0 on the rebased tree, and all six printed shas equal `esprytobj-v2.md` §4 **exactly** (`791b54a3… 1d6f15c9… 19e614af… 3ae7e347… 822ca3b0… 266710e0…`) |
| … `kMGPipeSurfaceNoTextureTarget` never fed to the masks on a renderbuffer point | **YES** | the renderbuffer arms of all four take the `Kind == Renderbuffer` branch and call the renderbuffer siblings, which take no target; `IsIntegerColorSurface` reads the format alone. `PushedSurfaceTextureTarget` is only reachable under `Kind == Texture` |
| … E's READ-only attachment parameter sync still reachable | **YES** | E's `SyncReadFramebufferTextureAttachments` lives in `DirectGLES.cpp` and calls `SyncTextureParamsToBackend` / `SyncBuiltinSamplerToBackend`; neither those two entry points nor their signatures changed, and nothing in this round touches E's file (`git diff --stat`: three files, `DirectGLES.cpp` not among them) |
| **M-4** `AdoptTwinByHandle` wired; `ReleaseByHandle` gone | **CLOSED in letter** — see **N-3** | two callers, `:8657` (texture) and `:8723` (renderbuffer). `StateBackendObjectRegistry::ReleaseByHandle` removed, the decision written where the method was (`Managers.h:470-481`); `SlotTable::ReleaseByHandle` untouched and still driven by `SanityTest.cpp` |
| **ID-12** `MGPipeSubDataUploadTargetOf` at find + consume, no bare comparison | **CLOSED** | see M-1's row. `MGPResourceDesc::Target` still has zero readers; `MGPFramebufferState::Target` is no longer compared anywhere |
| **ID-12** DV-2 / DV-4 | **CLOSED** | `MGB_TEXPARAM_DS_MODE` reads `kMGPipeDepthStencilModeStencil` (`:7835-7839`); the read-surface emptiness test keeps `MGPipeHandleIsNull(Res)` as the authority and **cross-checks** `Kind` against it, refusing when they disagree (`:8877-8886`) |
| **ID-15** bit 10 requires bit 11, in the precedent's voice | **CLOSED** | `ResolveTextureResourceSubsystemArm` `:3651-3664`, through the shared `PipeSubsystemDependencyMissing` (`:3521`), guarded `if (!refused)` so the bit-7 row cannot pre-empt it. `ResolveSamplerSubsystemArm`'s "the mirror pair … is FINE" sentence is corrected in place (`:3692-3699`); the mirror-pair table in `esprytobj-v2.md` §2(7) matches the source |
| … the exact `MGLOG_E` sentence F's matcher expects | **MATCHES** | one `MGLOG_E` line, `"MGPipe: %s - REFUSING the dependent bit and running the legacy arm. Set both bits, or clear both"` with `what` naming both bits in set-then-needed order. `gates-review-v2.md` §2.6 fed D's four real `what` strings through F's matcher: the two dependency rows match in both the 0x9ff and 0x5ff columns and all four near-misses are rejected. Nothing changed in that sentence after `b6169dba`, so F's measurement still applies at `ef6beb28` |
| **E's item (4)** `InvalidateFramebufferHandleArmMemos` inside `InvalidateFramebufferBindingCache` | **CLOSED as a declared two-line handshake (DV-13)** | the call is `Managers.cpp:8270-8278`, inside the function, beside the pre-handle trio it clears; the declaration and the whole argument are at `Managers.h:1996-2015`, gated by `#define MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED 0` (`:2011`). Can E's memo miss an invalidation once it is on? **No** — the three `SanityTest.cpp` callers (`:2530`, `:2778`, `:2799`) all go through `FramebufferImpl::InvalidateFramebufferBindingCache`, as do the six in `DirectGLES.cpp`, so being inside it is exactly the "forgetting is impossible" property E asked for. Half-done handshake does not link: the constant has no other reader and the declaration no other definition |
| **G9 white-box probe (R1)** | **CLOSED**, green on four arms, red under mutation — see **N-9** for the residual | `SanityTest.cpp:3184`. Covers deferred-to-first-view as R1 demanded: the reading is taken while the texture is attachment-only, and the probe asserts *before* the sync that no sampler-view twin exists — the assertion the public scenario cannot make |
| ten minors m-1..m-10 | **all closed or declared**, individually re-checked (§3 of `esprytobj-v2.md` matches the source at every line I spot-checked) | m-1 fixed + DV-14 declared (`PendingUpload` really is `{UploadTarget, Level, UnionBox, Regions}` — `PipeApply.h`; `SourceIsVerbatimLevelShadow` is genuinely unreachable); m-2 `:6975-7001`; m-3 `:8063`; m-4 `:8888-8912`; m-5 reworded; m-6 run (I re-ran it, §4); m-7 the nested `#if` is gone and the dispatcher's own guard at `:182` really does open the whole function; m-8 `:4194-4208`; m-9 `:4337-4352`; m-10 restated — **but the restatement is still short by two, N-2** |
| DV-9 wording | **CORRECTED** | the false sentence is withdrawn and the claim is narrowed to the property D-P actually requires. I verified that property directly and independently (§4): pull build at `712c9467` vs pull build at D's rebased head, `+0` bytes of `.text`, `0 added / 0 removed / 0 resized / 0 renamed`, identical file size |
| G1 0/0/0/0 mechanism (macros, not locals/lambdas) intact | **YES** | 20 push-arm `#define MGB_*` and 20 matching `#undef`s; every one takes the receiver it would have been spelled on, so the pull expansion is the pre-P4a expression in one pair of parentheses. `MGB_RECT_SRC_PTR` is a macro for the stated `$_N`-renumbering reason and the pull arm is literally `(rectShadowPtr(rect))` |
| macro leak into headers | **NONE** | `grep -rn 'MGB_LEVEL_\|MGB_TEXPARAM\|MGB_RBO_\|MGB_STORAGE_\|MGB_RECT_' MobileGL --include=*.h` → no hits |
| purity greps (G13) | **CLEAN** | `pGLContext` → `MG_Backend/MGPipe/PipeInputs.h:1` only; `MGPipeResourceOps` ∌ `MG_State` (0) |
| exit-order rule (ID-8) | **HELD** | the diff adds **no** namespace-scope object in `Managers.cpp` — the seven new file-scope symbols are all `static` *functions*. `SanityTest.cpp` adds `g_texParamCalls` (a raw pointer) and `g_texParamBoundTexture` (a `GLuint`): trivially destructible, no guard variable, no `std::atexit`, and neither holds a frontend `SharedPtr` |
| logging discipline | **CLEAN** | the diff adds **17** `MGLOG_E_ONCE`, **0** bare `MGLOG_E`, **0** `MGLOG_I`. No `printf`, no commented-out code |
| DirectVulkan lane 475/475 — what runs there | **REPRODUCED**, and the attribution is right | 475 `DirectVulkan.*` integration-gpu cases, run under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. D changes only `MG_Backend/DirectGLES`, so what this lane actually exercises of D's work is the **shared** half: `MGPipeSlotAllocator`, the death-notice dispatcher (`Managers.cpp:182-250`, which the SamplerViewCso arm's `#if` removal touched) and process-teardown ordering, on the backend that installs **no** death ops. That is precisely ID-8's leak/idempotence half, and 0 failures is the right reading |
| the default arm 189 = 183 + 6 | **REPRODUCED EXACTLY**, classification confirmed by mechanism | §4 |

---

## 2. Findings

Nine, none blocking. Each has a file:line, a failure scenario and the refutation I tried.

### N-1 (minor, but it ships in a log line) — "left unbound rather than bound-and-unconfigured" is false at every caller

`Managers.cpp:9083-9087` (the refusal text), and `esprytobj-v2.md` §2(1)'s claim that on a missing
record "the driver's binding is where the caller left it rather than pointing at a freshly minted,
attachment-less driver FBO".

`SyncToBackend` returning without binding does not decide the driver's binding, because **every**
caller binds immediately afterwards:

* `DirectGLES.cpp:3292-3293` — `SyncAndBindFramebufferObject` is `backendObj->SyncToBackend(...)`
  then `backendObj->Bind(target)`, unconditional, `void` return. That is the path all five DSA entry
  points take (`:6474`, `:6475`, `:7960`, `:7980`, `:7997`, `:8017`).
* `DirectGLES.cpp:3206-3256` — `BindCurrentFBO` binds the current FBO's twin independently of
  whether `SyncCurrentFBO` (`:2310`) synced it.

**Failure scenario.** On the integrated tree B fails to emit a `Named` record for one DSA-cleared
framebuffer. The twin refuses and returns; `SyncAndBindFramebufferObject` then calls `Bind(Draw)`,
which mints and binds the driver FBO exactly as before, and `glClearBufferfv` lands on a framebuffer
with no attachments — the identical outcome to v1. The integrator reads the log line, believes
nothing was bound, and looks for the wrong symptom.

**Refutation attempted, and it fails three ways.** (a) Does `Bind` early-out when nothing was
synced? No — it binds `m_backendFBOId`, minting it if needed. (b) Does any caller check a return
value? `SyncToBackend` returns `void`. (c) Does the reorder buy anything at the *bound*-FBO path?
No: `SyncCurrentFBO` never binds, and `BindCurrentFBO` binds regardless.

**What the reorder does buy** is real but smaller and worth saying instead: the twin no longer
*mints and binds* a driver FBO as a side effect of a refusal, so the refusal itself has no driver
effect. **Fix**: reword the message to "…so it is not configured; the caller's own bind still
targets it" (or similar), and correct §2(1). One string, and it is the string F's residual grep and
the integrator's eye will both land on.

### N-2 (minor, owed before the verification round) — the refusal census names six of **eight** shapes, and I observed seven in a single run

`esprytobj-v2.md` §4 ("six distinct shapes and no seventh") and §5 item 2 ("the integrated tree must
show **zero** of all six shapes").

`grep -n 'applier record' Managers.cpp` at HEAD gives **eight** distinct `MGLOG_E_ONCE` refusals:

```
:6310  texture %u ... its storage and uploads cannot be driven from the pushed descriptor
:7564  texture %u ... its built-in sampler cannot be pushed
:7778  texture %u ... its parameters cannot be pushed
:8866  framebuffer %u ... its read buffer cannot be pushed
:9083  framebuffer %u ... it cannot be synced from the pushed record and is left unbound ...
:11925 program %u has no shader-CSO applier record ... its synced serial cannot be stamped
:12174 sampler %u ... its parameters cannot be pushed                      <- NOT IN THE CENSUS
:12418 renderbuffer %u ... its storage cannot be allocated from the pushed descriptor  <- NOT IN THE CENSUS
```

**Demonstrated, not argued.** One standalone run of `*IntegerBorderColor*` on the default arm with
`MOBILEGL_LOG_FILE_PATH` set produced exactly **seven** `ERROR]: MGPipe` lines — the six the report
names **plus** `MGPipe: sampler N has no applier record on the handle arm, so its parameters cannot
be pushed`. (The renderbuffer one needs a scenario with a renderbuffer attachment; it is the eighth
by inspection.) v1's m-10 corrected "three" to "six"; six is still short.

**Failure scenario.** ID-12's rule is that a residual of *any* named refusal on the integrated tree
is a seam defect. The verification round greps the six, the sampler-CSO and renderbuffer families
still refuse because C's or B's records never arrive for them, and the round reports "zero
residuals" over a tree where two families are running the legacy arm.

**Refutation attempted.** Could the sampler line be another package's? No — `Managers.cpp:12174` is
in `BackendSamplerObject::SyncToBackend`, D's file, added by D's v1 commit `107658f7` ("take the
sampler parameters … from the applier"). Could it be unreachable on an integrated tree? No: it is
the sampler family's only decline, and it fires the moment a sampler object is synced with no
`create_sampler_state` applied — which is exactly what a C-side seam looks like.

**Fix**: list all eight in §4 and §5-item-2. The common stem `no applier record on the handle arm`
matches seven of the eight; only `:11925` ("no shader-CSO applier record") needs its own pattern.

### N-3 (minor) — `AdoptTwinByHandle` now has callers, but neither can reach either of its refusals

`Managers.cpp:8657` and `:8723`, against the helper at `:3550-3574`.

Both call sites are preceded by a cross-check that the record's `Res` equals
`registry.HandleOf(frontendObject)` (`:8642-8655`, `:8709-8722`) and return `false` when it does
not. `HandleOf` (`Managers.h:420`) resolves through `MGPipeSlots().FindByLifetimeId`, i.e. the
**live** handle. So by the time `AdoptTwinByHandle` runs, `handle.Slot` is a slot the registry itself
minted (below `kMaxHandleSlot`) and `handle.Gen` equals `LiveGenAt(handle.Slot)` — neither the
slot-bound refusal nor the backward-generation refusal can fire, and `GetOrCreateByHandle` can never
take its `entry.backend.reset()` path either.

**Failure scenario.** M-4's actual argument was "dead code cannot be wrong-and-noticed; its bound and
its wording will first be exercised at P5". That argument is unchanged: the helper is now *called*
but its distinguishing behaviour is still never *exercised* outside `SanityTest.cpp`. A typo in
either bound would still ship.

**Refutation attempted, and it half succeeds.** `HandleOf` memoises (`SlotTables.h`,
`m_memoLifetimeId`/`m_memoHandle`), so a stale memo could in principle return a generation behind the
live entry and make the refusal reachable. I could not construct a sequence that does it — a live
object's handle does not change generation — so I record this as "essentially unreachable" rather
than "provably unreachable". Either way the letter of M-4 is closed and its substance is not; say so
in the report rather than leaving the integrator to infer it.

### N-4 (minor) — the metadata respecify routes the *prevention* case through the remint-pull emulation marker

`Managers.cpp:6335-6339` calling `RequireImageBindableStorage` (`:5203`), whose first statement is
`MG_Pipe::MGPipeUnmigratedEmulation("texture-remint-pull")` (`:5235`).

`MGPResourceDesc::ImageBindableHint` exists as *mitigation 1, the prevention half* — the comment at
`:5223-5231` says so in as many words: a texture that has ever been image-bound "is allocated in the
carrier from the start and never reaches this path". D's rework makes the hint's arrival the trigger
for entering that path.

**Failure scenario.** `glBindImageTexture` on a texture whose storage is not yet defined: B's sticky
mask gains `kMGPipeBindShaderImage`, emits the mask-only respecify (ID-18 M4), D calls
`RequireImageBindableStorage`, the marker fires, and the level loop then finds nothing to re-arm.
Under split (P9) that marker is the arm that aborts/counts, so the case the hint was invented to
prevent is counted as an unmigrated emulation. In monolith `MGPipeUnmigratedEmulation` is
`{ (void)name; }` (`PipeApply.cpp:2717`), so nothing is observable today — this is a P9-facing
accuracy defect, which is why it is a minor and not a major.

**Refutation attempted.** (a) Is the marker about something wider than the replay? No — its own
comment scopes it to "the server is reaching BACK into the client's own dirty model to ask for texels
it does not hold". (b) Is the no-levels case unreachable? No: B fills
`desc.ImageBindableHint = (bindMask & kMGPipeBindShaderImage) != 0`
(`p4a/clientfb:TextureEmit.h:230`, `:520`, `:982`) from the sticky mask on every respecify, including
one that precedes any level definition.

**Fix**: move the marker to where a level is actually re-armed (inside the loop, or behind a
"replayed at least one level" flag). Two lines, and it keeps the marker honest for P9.

### N-5 (minor, consistency) — `SrcOffset` is consumed raw while its sibling stride got a loud refusal in the same commit

`Managers.cpp:7091-7096` (`MGB_RECT_SRC_PTR`), used at `:7123`, `:7218`, `:7262`.

The macro adds `Regions[idx].SrcOffset` to `uploadData` and hands the result to `glTexSubImage*` as
the client pointer for `(w·bpp × h × d)` bytes. The same commit added a loud refusal when the
regions' *strides* disagree (`:6975-7001`), on the argument that "a record whose regions disagree is
corrupt". `SrcOffset` gets no equivalent — and the block two lines above computes the same number
independently (`rectShadowPtr`, `:7074-7079`), so the cross-check is free.

**Refutation attempted, and it is strong enough to keep this a minor.** (a) Wire validates
region-inside-box and box-encodability (`PipeApply.cpp:743-770`) but explicitly does *not* validate
the box against the level extent, and says so. (b) B derives `SrcOffset` from the same box origin and
level pitch the shadow path uses (`clientfb:TextureEmit.h:394-396`), so in monolith the two agree by
construction. (c) The pre-existing `rectShadowPtr` derivation is exactly as untrusted, so this is not
a new exposure. It is an asymmetry in the package's own discipline, not a live defect.

**Also**: the first of the three use sites is unreachable. `dirtyRectCount` is forced to `0` whenever
`UnpackRingAvailable()` (`:7069-7071`), and the staging-block loop at `:7119` requires
`ringUsable && dirtyRectCount >= 2`. Pre-existing shape, not D's — but `esprytobj-v2.md` §3-m-1 cites
three live sites where there are two.

### N-6 (minor) — an empty surface is accepted silently while every other record-vs-frontend disagreement is loud

`Managers.cpp:8634-8640` (the `Kind == kMGPipeSurfaceKindNone || MGPipeHandleIsNull(Res)` arm of
`SyncAttachmentSurface`), against the detach at `:9289-9291`.

The detach is decided by the **frontend** (`attachmentObject.IsEmpty()`); the attach is decided by
the **record**. When the record says a point is empty and the frontend still holds an attachment,
neither fires: no detach (frontend not empty), no attach (surface empty), `return true` — and the
caller then stamps `m_syncedFrontendAttachmentVersions[i]`, so the point is marked synced with the
*previous* owner's image still attached. The comment claims "the caller has already detached it where
that is what an empty point means", which is only true when the two sides agree.

**Refutation attempted, and it explains why this is a minor.** (a) Would the point even be visited?
Yes — a moved `ContentHash` re-arms every version to `~0` (`:9256-9260`), so a record-only change
forces the whole walk. (b) Can the two disagree in monolith? Only through a frontend that keeps a
detached attachment object non-empty, which I could not find. (c) Is the *converse* loud? Yes:
`Kind == Texture` with a null frontend texture, and a `Res`/`HandleOf` mismatch, are both
`MGLOG_E_ONCE`. So the hole is exactly one direction wide. One `else if (!attachmentObject.IsEmpty())`
with the same voice closes it.

### N-7 (minor) — D keys on the high byte, wire keys on the whole packed field

`Managers.cpp:3799` / `:3821` / `:3872` versus `PipeApply.cpp:814` (`AccumulatePendingUpload`:
`if (entry.UploadTarget != record.Target || entry.Level != record.Level) continue;`).

Wire treats two entries with the same upload half and different resource-target bytes as **distinct**;
D treats them as the **same**. They agree today only because a texture's
`MGPipeResourceTargetForTextureTarget(GetTarget())` is constant for the object's life.

**Failure scenario.** If that ever stopped holding, wire would store two entries and D's consume
would erase the first match and leave the second in the set for ever — a full re-upload of that level
on every sync, silently, for the life of the texture.

**Refutation attempted, and it holds today.** A texture emits sub-data only through
`MGPipeBuildSubRegion`/the texture emitter, whose resource-target byte comes from the object's own
target; texture views own their own resource. So this is a latent divergence, not a live one. **Fix**:
either compare the packed field (which is what `RearmPipeTextureLevelUpload` already writes, so
nothing else changes) or assert the low byte matches, at the two comparison sites.

### N-8 (minor) — `Desc.HasDefinedContent` is neither read nor listed among what the descriptor does not answer

`Managers.cpp:6230-6237` enumerates what the descriptor answers and what it does not; c0e made
`HasDefinedContent` storage-defining (`MGPipeTypes.h:301`), the buffer family reads it three times,
and the texture arm reads it nowhere.

**Refutation attempted, and it succeeds on behaviour.** For a texture the per-level pending set is
the authority on content (D-D5), and a level the shadow has not defined is skipped by the per-level
loops; `HasDefinedContent` would add nothing. So this is a **documentation** gap in an enumeration
whose whole purpose is to be exhaustive — one line at `:6237`.

### N-9 (minor) — the G9 probe covers deferral *inside* the function, not *above* it

`SanityTest.cpp:3184`, and `gates-review-v1.md` R1's second half ("distinguish 'not applied yet' from
'applied late'").

The probe calls `twin->SyncTextureParamsToBackend(texture)` directly. The negative control the report
describes — requiring `FindSamplerViewForHandle(...) != nullptr` inside the swizzle block — is
therefore the only deferral shape it can see. A regression that gates the *call* on a sampler view
existing (in `SyncNeccessaryTextures`, or in E's per-unit walk) leaves this case green.

**Refutation attempted, and it is why this is a minor.** R1 asked for a *backend-side probe* taking
its reading while the texture is attachment-only, and that is exactly what this is; the caller-level
shape is a scenario's job and the scenario is F's. Worth one sentence in the case's comment and in
F v3's G9 note, so the next reader does not over-trust it.

---

## 3. What I tried to break and could not

* **Can a `Named` record reach a target-gated apply?** No. `glDrawBuffers` and the four masks are
  under `asTarget == Draw` (`:9151`, `:9159`), `glReadBuffer` under `asTarget == Read` (`:9220`), and
  `asTarget` is the caller's binding intent, not the record's `Target`. The D-O row survives.
* **Can the metadata respecify recurse?** No. `RequireImageBindableStorage` early-returns on
  `m_imageBindableStorageRequired` and calls nothing that re-enters `SyncMipmapsToBackend`.
* **Can `pushedStorage` dangle across the re-arm?** No. The re-arm `push_back`s into
  `record.PendingUploads`, which does not move `TextureResources`; only a create/respecify for a new
  slot grows the outer vector, and nothing on this path emits one.
* **Can the re-arm break wire's own invariants?** No. It either replaces `UnionBox` and clears
  `Regions` (box-only, which is legal and is what `AccumulatePendingUpload` itself falls back to) or
  inserts a region-less entry. The one nit is that it *replaces* rather than unions the box; the
  replacement is a whole-level box, which contains any box built from the same level's extent, and
  `UnionOfBoxes` is `static` in `PipeApply.cpp` so D could not call it anyway.
* **Can `dirtyRects[r]` and `Regions[r]` fall out of step?** No. On the handle arm `dirtyRects` is
  *built from* `Regions` (`:7043-7060`), index for index, so `MGB_RECT_SRC_PTR(r, rect)` is
  consistent by construction.
* **Does the sampler-view / death-notice `#if` removal (m-7) change any arm?** No. The dispatcher's
  own `#if MOBILEGL_PIPE_PUSH` at `:182` opens the whole `OnFrontendStateObjectDestroyed` body;
  the removed inner pair was strictly nested inside it.

---

## 4. What I measured — all of it on the rebased tree (base `712c9467`, D head `d86b32e9`)

```
build-push  (PUSH=ON)                                          rc 0
build-linux (pull, PUSH=OFF)                                   rc 0

ctest -L unit  build-push    100% passed, 0 failed out of 1686
ctest -L unit  build-linux   100% passed, 0 failed out of 1686

G1, ISOLATING D (pull build at 712c9467  ->  pull build at D's head):
    symbol_report.py --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0   rc 0
    file bytes 19114360 -> 19114360 ; .text 10806323 -> 10806323 (+0, +0.000%)
    .data/.bss/.rodata all +0 ; Total 17226471 -> 17226471 (+0)
    27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
    27072 normalised names, 27072 unchanged
    ### Removed (0)  ### Added (0)  ### Resized (0)  ### Renamed only (0)

p4a_untouched_regions.sh 37da3c3a HEAD (F's script, from ~/w7/p4a-gates)   rc 0
    17 pool / ring / unpack-staging / attachment-permutation /
    depth-stencil-sampling / format-caveat regions byte-identical
    791b54a3.. StageBlocksIntoUnpackRing     1d6f15c9.. UnpackRingAvailable
    19e614af.. UnpackRingAllocate            3ae7e347.. RecomputeBackendColorSlots
    822ca3b0.. DepthStencilSamplingReadImpl  266710e0.. ShouldUseCaveatTextureFormat
    == esprytobj-v2.md §4, sha for sha

ctest -L integration-gpu -j 8 -R DirectGLES, build-push:
    default (0x1fff)         62% passed, 189 failed out of 491   (6 aborts, 183 named)
    MOBILEGL_PIPE_PUSH=0x1ff 99% passed,   6 failed out of 491   (6 aborts)
    MOBILEGL_PIPE_PUSH=0     99% passed,   6 failed out of 491   (6 aborts)
    comm -3 (0 vs 0x1ff)                 EMPTY  -> the two failing SETS are identical
    comm -13 (0x1ff not in default)      EMPTY  -> the default arm is a strict superset

ctest -L integration-gpu -j 8 -R DirectVulkan   100% passed, 0 failed out of 475

G9 probe DirectGLESTextureSync.AnAttachmentOnlyTexturesParametersReachTheDriverWithNoSamplerView
    push @ 0, push @ 0x1ff, push @ 0x1fff, and the pull build:  PASSED (4/4)
DirectGLESSlotTable.*                                          20/20 passed

m-6, re-run as a SINGLE TU rather than a whole configure: the build-push compile command for
    Managers.cpp with -DMOBILEGL_PIPE_LEGACY_MEMOS=1 rewritten to =0, -fsyntax-only:
        rc 0, 0 errors, 0 warnings
    (six `#if !MOBILEGL_PIPE_LEGACY_MEMOS` bodies now live in the file: :333 :4281 :7642 :7902
     :12187 :12431 — the four P4a added plus two inherited)

greps:  MGB_* in any header                 no hits
        pGLContext in MG_Backend            PipeInputs.h:1 only
        MGPipeResourceOps ∌ MG_State        0
        diff adds MGLOG_E_ONCE 17 / MGLOG_E 0 / MGLOG_I 0
```

**The 189 = 183 + 6 classification.** Confirmed by mechanism rather than by a log I no longer have:
the six are the ctest `ENVIRONMENT`-pinned `.Handles` aborts, they are present in all three arms'
failing sets and are the *only* overlap (`comm`), and `comm -13` shows the default arm adds nothing
that fails for a pre-existing reason. The census run's log carried **zero** `FATAL]: MGPipe` lines —
the only two `FATAL` lines are ambient EGL entry-point loads under llvmpipe
(`eglSwapBuffersWithDamageEXT`, `eglUnlockSurfaceKHR`), which the integrator should expect and not
mistake for a pipe fault.

**Not re-derived**: DV-9's 38-line preprocessed-text count (the report withdraws the claim it
supported; the property that matters is measured above), and the retrace corpus (meaningless with no
records, as §5-item-13 says).

---

## 5. The rebase onto `712c9467` — **ID-32's command does not work**

ID-32 and the brief both say:

```
git rebase --onto refs/heads/feat/disaggregated cce81d19      # "replays only the seven rework commits"
```

Ran it. It **conflicts on the first commit**:

```
Rebasing (1/7)
CONFLICT (content): Merge conflict in MobileGL/MG_Backend/DirectGLES/Managers.cpp   (4 hunks)
CONFLICT (content): Merge conflict in MobileGL/MG_Backend/DirectGLES/Managers.h     (2 hunks)
error: could not apply ac6757e6...
```

**Why**, and it is not a resolvable conflict — it is the wrong range. `cce81d19..HEAD` is the seven
rework commits *only*, and those seven are built on D's **v1 six** (`b1983e2c..8338c18f`), which sit
*below* the two merges and are **not on pipe**: `git diff cce81d19 712c9467 -- Managers.{cpp,h}` is
`-1208 / -360` lines, exactly D's v1 additions. Replaying the seven onto a tree that has none of v1
asks git to re-apply `PushedFramebufferRecord`'s *edit* to a file that has no
`PushedFramebufferRecord`. Resolving the conflicts by hand would silently drop the whole v1
deliverable.

`git rebase --onto refs/heads/feat/disaggregated 9ea44389` is also wrong: it linearises the merged
branch, so after replaying D's six v1 commits cleanly it tries to replay **wire's twelve** on top of
pipe's already-rebased copies of them and conflicts in `PipeApply.{h,cpp}` (wire's rebase onto
`e8502a61` changed their patch-ids, so `git` cannot skip them).

### The recipe that works — verified, zero conflicts, builds, and reproduces every number

```
git checkout --detach 712c9467
git cherry-pick 9ea44389..8338c18f      # D's six v1 commits   -> clean, 6/6
git cherry-pick cce81d19..ef6beb28      # D's seven rework     -> clean, 7/7
```

Thirteen commits, **no conflicts at any step**, and

```
git diff HEAD ef6beb28 -- MobileGL/MG_Backend/DirectGLES/ MobileGL/MG_Test/SanityTest.cpp
```

is **empty** — D's three files are byte-identical to the pre-rebase tree. `cce81d19` is excluded by
the range (its retirement is already `712c9467`); its patch-id and `712c9467`'s differ, so `git`
would *not* have skipped it automatically — leaving it out of the range is what drops it, and that is
the point of starting the second range at `cce81d19` rather than at `815dd6d3`.

Resulting head `d86b32e9`. Both builds rc 0, unit 1686 × 2, G1 0/0/0/0 against `712c9467`'s own pull
build, D-N rc 0, three integration arms at 189/6/6, DirectVulkan 475/475 — §4.

**The pipe head moved during this review** and the recipe survives it. `refs/heads/feat/disaggregated`
is now **`92dffb81`** (C v3 / `p4a/clientsp` fast-forwarded in: eleven commits, `1534cf37..92dffb81`),
with `712c9467` still an ancestor. `git diff 712c9467 92dffb81 -- Managers.cpp Managers.h
SanityTest.cpp` is **empty** — nothing C landed touches any file D owns — so both cherry-pick ranges
apply unchanged onto `92dffb81` and every number in §4 stands. Substitute whatever
`refs/heads/feat/disaggregated` is at integration time for `712c9467` in the recipe; the two ranges
(`9ea44389..8338c18f` and `cce81d19..ef6beb28`) are what matters and they are fixed.

---

## 6. D's verification-round checklist, corrected

`esprytobj-v2.md` §5 is sound; these are the amendments this review adds. Order is still ID-24's
(C v3 → B v2 → **D v2** → E verification → **D verification** → F v3).

1. **Item 2 is short by two shapes (N-2).** The residual-refusal check must cover **eight**:
   the six §4 lists, plus `MGPipe: sampler %u has no applier record …its parameters cannot be pushed`
   (`Managers.cpp:12174`) and `MGPipe: renderbuffer %u has no applier record …its storage cannot be
   allocated from the pushed descriptor` (`:12418`). Grep the stem
   `no applier record on the handle arm` (catches seven) **and** `no shader-CSO applier record`
   (catches `:11925`). A residual sampler shape points at C's cache seam, a residual renderbuffer
   shape at B's renderbuffer descriptor — neither is what the framebuffer shapes would tell you.
2. **Item 3 stays, but read it with N-1 in mind**: if a DSA case is red *and* the framebuffer refusal
   is in the log, the driver still had the unconfigured FBO bound. That is B's missing `Named` record,
   not a residue of the CRITICAL fix.
3. **Item 7 (the storage-shape seam) is already closed** — I compared `TextureEmit.h:183-241` against
   `Managers.cpp:6244-6270` this round: B fills `Width/Height/Depth` from `GetBaseSize()` verbatim and
   derives `ArrayLayers` *in addition*, so `MGB_STORAGE_BASE_SIZE` and the frontend agree exactly and
   `needsRegeneration` cannot churn on it. Ten minutes saved; spend them on item 6 instead.
4. **Add: assert `MGPipeApplier().StaleFramebufferRecordLookups == 0`** — already item 3's tail, but
   also worth asserting after the *image-bindable remint* set, because the re-arm and the framebuffer
   table are the two places D writes/reads applier state outside a record apply.
5. **Add (N-4)**: when B's `ImageBindableHint` lands, confirm that a texture whose mask-only respecify
   arrives *before* any level is defined does not trip `MGPipeUnmigratedEmulation`. Today it does, and
   it is inert; P9 will not be.
6. **Add (N-9)**: when F v3 writes G9's scenario half, note in the white-box case's comment that it
   covers in-function deferral only.
7. `MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED` → 1 (`Managers.h:2011`) after E drops the `static`
   — unchanged, and the gate is that the flip does not link without E's half.
8. The **0x5ff** arm is expected **GREEN**, not red — `gates-review-v2.md` F-v2-m2 already corrected
   the gates side, and D's row is in and matches F's matcher. A red there is a real finding about D's
   resolver.

---

## 7. What I could not check

* **The 183's per-case classification.** `~/w7/p4a-esprytobj-lanes*.log` are gone (ID-5-compliant). I
  reproduced the count, the six-abort overlap and the superset property mechanically, and sampled the
  shapes from a fresh standalone run — which is how N-2 surfaced. The same recommendation as v1
  stands: **keep the lane logs until the review of the round closes**.
* **Anything behind a record.** With no client on this tree the handle arms mostly decline; the
  CRITICAL's fix, M-1's replay, M-2's descriptor allocation, M-3's attachment walk and the four masks
  are all first *executed* on the integrated tree. §6 is where they get their first real reading.
* **G4's retain-mode comparison** — still has not run anywhere in this phase, and DV-15 makes it the
  one lane that can tell a legitimate server-side re-arm from a lost emission.

---

Private worktree `~/w7/p4a-rereview-esprytobj` removed and `git worktree prune` run; the
`~/w7/rr-obj-*.log` files and `/tmp/rr-obj-*` scratch removed with it.
