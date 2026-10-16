# P4a package D — `p4a/esprytobj` adversarial review, v1

Reviewed: `refs/heads/p4a/esprytobj` `7aa598fa..9bb95335` (six commits) against the tag `p4a/contract`
(`08192d72`). Worktree read-only; experiments in `~/w7/p4a-review-esprytobj` (detached, removed at the
end of this round). Sources read in full: the whole `git diff 08192d72 HEAD` (Managers.cpp +1208 /
Managers.h +360 / SanityTest.cpp +196) plus the surrounding bodies of every function it touches, plus
`SlotTables.h`'s handle overloads, `p4a/wire@3c07c8c3`'s `PipeApply.{h,cpp}`, `p4a/clientfb@8946061e`'s
`TextureEmit.h` / `FramebufferEmit.h` / `TextureObject.cpp` and `p4a/esprytdraw@0b00f7f0`'s
`DirectGLES.cpp` (only to refute findings, never to grade them).

## Verdict: **REWORK**

One critical, four majors, ten minors. The critical and two of the majors are behavioural and are
invisible on this tree by construction (no records exist yet), so none of them could have been caught
by the package's own verification — which is exactly why they have to be fixed before the real-path
round rather than found by it.

What the package got right is worth stating first, because it is most of it: the refusal discipline is
genuine (every handle arm declines with a named `MGLOG_E_ONCE` and returns; I could not find one
silent fall-back that would make a lane pass), the four arm resolvers and the three dependency
refusals are D-K2/D-K3 verbatim, the death-notice arm is idempotent by construction, the G5 set is
byte-identical (recomputed independently), the macro hygiene is complete, and the purity greps are
clean.

---

## 1. Critical

### C-1 — the handle arm silently stops configuring every framebuffer that is not the currently bound one, which is all five DSA / blit entry points

`MobileGL/MG_Backend/DirectGLES/Managers.cpp:8216-8228` (`FramebufferImpl::PushedFramebufferRecord`)
and `:8450-8473` (the decline inside `BackendFramebufferObject::SyncToBackend`).

`PushedFramebufferRecord(asTarget, fbo)` answers only `MGPipeApplier().DrawFramebuffer` /
`.ReadFramebuffer` — the applier's **working state for the two bound targets** — and returns null when
`record.Fbo != fbo`. `SyncToBackend` then does:

```
GLenum glFBOTarget = ...;
Bind(asTarget);                                   // :8432 - the driver FBO is bound FIRST
...
pushedRecord = PushedFramebufferRecord(asTarget, fbo);        // :8450
if (pushedRecord == nullptr) { MGLOG_E_ONCE(...); return; }   // :8451-8457
```

**Failure scenario.** `glClearNamedFramebufferfv(fbo, ...)` with bit 9 set:
`ClearNamedFramebufferfv` (`DirectGLES.cpp:7960`) →
`SyncAndBindFramebufferObject(framebuffer, Draw, forceSync=true)` (`DirectGLES.cpp:3262-3294`) →
`registry.GetOrCreate(framebuffer)` mints a **fresh** twin with no attachments →
`InvalidateSyncedState()` → `SyncToBackend(framebuffer, Draw)` → `Bind(Draw)` → the record names the
*application's* bound FBO, not this one → refusal → `return` → back in the caller,
`backendObj->Bind(target)` → `glClearBufferfv` is issued against a driver framebuffer with **no
attachments at all**. Result: `GL_INVALID_FRAMEBUFFER_OPERATION` and nothing cleared. On the legacy
arm the same call configures the FBO from the frontend and clears correctly. The same holds for
`BlitNamedFramebuffer` (`DirectGLES.cpp:6474-6475`, both targets) and the other three
`ClearNamedFramebuffer*` (`:7980`, `:7997`, `:8017`).

This is not the benign case the report describes. §2-d4 says a mismatch "is answered with null rather
than with the other framebuffer's attachments" — true, and the alternative it rejects would indeed be
worse — but the caller's response to null is to do *nothing at all*, and doing nothing to a
framebuffer that is about to be cleared or blitted into is a third outcome the report does not
consider.

**Refutations attempted, all four failed.**

1. *Does package B emit a record for a DSA-named framebuffer?* No.
   `p4a/clientfb:MG_Impl/Pipe/FramebufferEmit.h:219 EmitFramebufferState(GLContext& ctx)` builds only
   from the two bound slots (`:237-244`, `drawFbo`/`readFbo`), which is D-C2 as written ("once per
   bound target that moved"). A named-but-unbound framebuffer never gets a record, and it must not:
   writing one into `DrawFramebuffer` would make the applier claim it is bound.
2. *Does package E fix the call site?* No. `p4a/esprytdraw@0b00f7f0`'s `SyncAndBindFramebufferObject`
   (`DirectGLES.cpp:3982-4013`) is unchanged in this respect and still calls
   `backendObj->SyncToBackend(framebuffer, target)`. E's e1 rework is confined to `SyncCurrentFBO` /
   `BindCurrentFBO` / `ForceBindCurrentFBO`.
3. *Does `ForceBindCurrentFBO` repair it afterwards?* It re-syncs the **application's** framebuffer,
   not the named one, and the clear/blit has already been issued by then.
4. *Is the path dead?* No — `ClearThenReadPixelsScenario`, `OrientationScenario` and
   `FramebufferTest.NamedFramebufferDrawBuffersDoNotModifyDefaultFramebuffer` all drive it, and the
   Minecraft/Iris traces drive it every frame.

**What the fix has to decide** (this is a contract question, not a one-line patch): either (a) the
applier grows a per-object framebuffer record beside the two working-state ones so a named FBO can be
addressed by handle — which is a real change to D-B2/D-I2 and A's file; or (b) Espryt keeps a named
arm for the not-currently-bound case and says so out loud (it is still a decline on the handle arm,
just not a silent one); or (c) B emits a third, explicitly-not-bound record kind. What is not
acceptable is the present state, where the only visible symptom is one `MGLOG_E_ONCE` line and a
framebuffer that quietly never gets attachments. **This is the finding the integrator most needs
before the real-path round**, because on the integrated tree it will present as "the 183 went to
zero, but the DSA scenarios are red" and look like a seam defect in B.

---

## 2. Major

### M-1 — the server's own re-dirty of the frontend model is invisible to the handle arm's upload loop

`Managers.cpp:6046-6049` (`MGB_LEVEL_NEEDS_UPLOAD`) **replaces** `IsStorageDirty` on the handle arm
rather than OR-ing with it:

```
(pushedStorage != nullptr
     ? FindPipeTextureUpload(*pushedStorage, (Uint16)tgt, (Uint16)lvl) != nullptr
     : (obj)->IsStorageDirty(tgt, lvl))
```

D-D5's inversion covers the **client's** own emission-time clear. It does not cover the fact that the
**server** still writes into the frontend dirty model from three places, two of which are live:
`RequireImageBindableStorage` (`Managers.cpp:5127`,
`mipmapObject->MarkStorageDirty(uploadTarget, level, true)` — the very re-dirty this package planted
`MGPipeUnmigratedEmulation("texture-remint-pull")` next to) and
`GenerateThreeChannelFloatMipmapOnCpu` (`DirectGLES.cpp:7406`).

**Failure scenario.** First `glBindImageTexture` on a texture that was already uploaded:
`SyncTextureObjectToBackend` (`DirectGLES.cpp:1529`) → `RequireImageBindableStorage` sets
`m_imageBindableStorageRequired`, clears `m_isInitialized`, re-dirties every defined level in the
**frontend** and sets `m_forceTextureParamsResync`; execution falls straight into
`SyncMipmapsToBackend` in the same verb. On the handle arm the applier's pending set is empty (the
client cleared and emitted those levels long ago), so `levelDirty == false` for every level,
`pData == nullptr`, and the widened image-carrier storage is allocated **empty**. The dispatch/draw
that triggered the bind reads zeroes. This is precisely the bug `Managers.cpp:5091-5101`'s comment
says "survived so long", re-introduced on the new arm.

**Refutation attempted (partially successful, and it is what keeps this out of §1).**
`TextureObjectWithOneMipmap::MarkStorageDirty` in `p4a/clientfb` (`TextureState/TextureObject.cpp:493-503`)
calls `PipeNoteLevelDirty(uploadTarget, mipmapLevel, dirty)` →
`MGPipeNoteTextureLevelDirty` (`TextureEmit.h:1012-1021`) → `emitter.NoteLevelDirty(...)`, so the
server's re-dirty **does** reach the client's drain list and the levels are emitted at the **next**
validate point. So the damage is bounded to the verb that triggers it and self-heals one verb later
(the applier bumps `Serial`, the gate at `Managers.cpp:6120-6123` misses, and the levels upload). It
is still deterministic wrong content for that verb, and for a one-dispatch-then-readback scenario that
is the whole result. `GenerateThreeChannelFloatMipmapOnCpu` has the same shape with a milder outcome.

**Fix**: on the handle arm the question is "does this level owe an upload", and the honest answer is
the union of the two sources for as long as the server keeps writing the client's flag —
`pending != nullptr || (obj)->IsStorageDirty(tgt, lvl)` — with `MGB_LEVEL_UPLOAD_DONE` clearing the
frontend flag too **only** for levels it did not consume from the pending set (so the D-D5 rule "the
server never clears the client's flag" keeps holding for records the client owns). Whatever the
shape, the three server-side re-dirty sites must be enumerated in the code beside it.

### M-2 — `MGPipeResourceRecord::Desc` is never read by the texture twin: the storage is still allocated entirely from the frontend

Grep across `Managers.cpp`: `pushedStorage->Desc` has **zero** readers (`pushedStorage` is used at
`:6047`, `:6055`, `:6097-6122`, `:6140`, `:6200`, `:6702`, `:7183-7193`, and never for `Desc`). Inside
`SyncMipmapsToBackend`, `currentTextureInfo` (`Managers.cpp:6236-6244`), `needsRegeneration`
(`:6251`, and again at `:7044`), `canAppendMipmaps` (`:6266-6277`) and every `glTexImage*` argument still
come from `stateTextureObject->GetFormat()/GetBaseSize()/GetSamples()/HasFixedSampleLocations()` and
`textureMipmapObject->GetMipmapLevelCount()`.

C.3's `d2` is explicit — "`m_prevTextureInfo` probe re-keyed onto the resource record's `Serial` **and
`Desc`**" — and the commit message says "drive texture storage, parameters and uploads from the pushed
descriptors". Only the parameters and the uploads moved. The renderbuffer twin one function over does
read its `Desc` (`MGB_RBO_FORMAT/WIDTH/HEIGHT/SAMPLES`, `:11694-11701`), so the asymmetry is not a
capability gap.

**Why it matters even though no lane can see it.** `SyncTextureParamsToBackend` already answers
`MGB_TEXPARAM_FORMAT` from `Desc.InternalFormat` (`:7479-7481`) while `SyncMipmapsToBackend` allocates
from `GetFormat()`. That is two authorities for one value inside one twin — the exact coupling P4a
exists to remove — and in monolith they agree, so G4/G12 cannot expose it. It is also what
`BackendTextureFormatAddsAlpha(MGB_TEXPARAM_FORMAT, ...)` at `:7651` silently depends on.

**Refutation attempted.** Is `Serial` enough? No: `Serial` gates *whether* to run, not *what* to
allocate. Is `Desc` missing the fields? No: `MGPResourceDesc` carries InternalFormat / Width / Height
/ Depth / Levels / Samples / FixedSampleLocations / Immutable, and B fills them (`TextureEmit.h`'s
respecify path).

**Not in §5's twelve deviations**, which is the part that makes it a major rather than a scope note:
D-N/D-P force a lot of shapes on this package and every one of them is documented; this one is not.

### M-3 — `SyncAttachmentObject` is in **D's** file, so DV-7's hand-off to E is a mis-attribution, and §7-13's claim about G9 is wrong

`SyncAttachmentObject` is `Managers.cpp:7947` — `MG_Backend/DirectGLES/Managers.cpp`, which C.7 gives
to **D** exclusively. DV-7 says "resolving them from `MGPSurface::Res` instead is package E's
`SyncAttachmentObject` work", and §7 item 13 says G9's red-before case
`AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` "is closed on the **E** side
(`SyncAttachmentObject` gains the parameter sync for READ-only attachments)". E cannot touch that
function without violating C.7, and it did not: `p4a/esprytdraw@0b00f7f0` changes `DirectGLES.cpp`
only (`git diff --stat`: 1 file, +893/-38).

**Refutation attempted, and it saves the deliverable but not the attribution.** E closed D-E3 by a
different route — a new `SyncReadFramebufferTextureAttachments` in `DirectGLES.cpp` (E's diff
`:208-282`, `:356-358`) that walks `MGPipeApplier().ReadFramebuffer` and calls
`SyncTextureParamsToBackend` / `SyncBuiltinSamplerToBackend` for the read attachments. So G9's case
does get closed. But the consequence for the rework stands: **with ID-12 minting
`MGPSurface::TextureTarget`, the attachment-walk re-key from `MGPSurface::Res` is D's work, not E's**,
and so are the four cross-object masks (below). The integrator should not schedule it on E.

### M-4 — the adoption helper d1 is named for has no call site anywhere in the phase

`AdoptTwinByHandle` (`Managers.cpp:3544-3568`), the anonymous-namespace template the report calls "the
release-build VOICE for the two silent refusals, shared by the five kinds", is never instantiated.
`StateBackendObjectRegistry::GetOrCreateByHandle` (`Managers.h:453`) and `ReleaseByHandle`
(`Managers.h:476`) likewise have no caller for any of the five re-keyed kinds. Grep over the whole
tree returns only the definitions, P3a's pre-existing buffer-family copies
(`Managers.cpp:2157`, `:2472`) and `SanityTest.cpp`. Package E adds none either: grep for
`AdoptTwinByHandle|GetOrCreateByHandle|ReleaseByHandle|LiveGenAt` over
`p4a/esprytdraw:DirectGLES.cpp` returns nothing.

So in this package the five twins are still minted through `GetOrCreate(StatePtr)` off the frontend
lifetime id, and every d2-d5 site resolves with `HandleOf(stateObject.get())` — a *lookup*, not an
adoption. DV-1 records half of this honestly (the resolution sites are E's) but not that the helper
itself is dead on arrival on both branches.

**Consequence, and it is not cosmetic**: that helper holds the only release-build diagnostic for the
two refusals `SlotTables.h` performs under a compiled-out `MOBILEGL_ASSERT`. Dead code cannot be
wrong-and-noticed; its bound (`Registry::SlotTable::kMaxHandleSlot`) and its wording will first be
exercised at P5. Either give it its first caller in D's rework (the five `HandleOf` sites are where an
adoption would go once the handle arrives in a payload) or move it, with its comment, to the package
that does — and say which in the report.

---

## 3. Minor

| # | site | what |
|---|---|---|
| m-1 | `Managers.cpp:6761-6765`, `:6724` | `MGPSubRegion::SrcOffset` and `MGPSubData::SourceIsVerbatimLevelShadow` are **never read** (zero grep hits). `regionPtr` re-derives the byte offset from `dirtyRegion.lo` and the strides, and `subRectEligible` is still the pre-P4a `uploadData == mipData` pointer comparison rather than D-D6's `SourceIsVerbatimLevelShadow` test. Correct in monolith, both are carried contract fields the server is supposed to consume, and D-D3's "carried, never inferred" applies to `SrcOffset` in the same sentence as the strides. |
| m-2 | `Managers.cpp:6742-6755` | only `Regions.front()`'s `SrcRowStride`/`SrcSliceStride` are consulted; a record whose regions disagree silently uses the first. D-D3 says the pitch is the level's, so add the assert rather than the loop. |
| m-3 | `Managers.cpp:7718` | `const auto borderColorForm = MGB_TEXPARAM_BORDER_FORM;` is evaluated unconditionally and, when `pushedBorder == nullptr`, reads `stateTextureObject->GetBorderColorForm()` **on the handle arm**. The value is unused (`borderColorReadable`, declared `:7541`, gates the block at `:7721`), so this is not a silent fall-back — but it is a live frontend read on a switched-over path and a future edit will use it. |
| m-4 | `Managers.cpp:8277-8299` | the read-buffer decode takes the FIRST `Color[i]` whose `{Res, Level, Layer}` equals `ReadSurface`. The same image legally attached to two colour points resolves to the lower index; `Kind` and `Layered` are not compared. The refusal branch says "refusing to guess", which the match itself partly does. |
| m-5 | report §5 DV-9 | the claim "the pull build's preprocessed text — and therefore its object code — is unchanged" is **false as worded**. Measured (clang++ `-E -P` with the pull `compile_commands.json` flags at both revs, diff categorised): **38 differing lines — 23 `line = N` constants** inside `ErrorLopper::Loop` lambda captures shifted by the insertions, **13 pure added-parenthesis** differences from the macro expansion, **2 whitespace re-flows** (the border-colour `if`). Zero semantic differences, and `symbol_report --fail-on-symbol-set-change --fail-on-added-bytes 0` is the real gate and passes — so the *conclusion* holds. Reword DV-9 to claim what is true. |
| m-6 | `Managers.cpp:7303`, `:7563`, `:11499`, `:11743` | the four `#if !MOBILEGL_PIPE_LEGACY_MEMOS` "unreachable but loud" bodies were compiled in **no build this round**: `build-linux` and `build-push` both pass `-DMOBILEGL_PIPE_LEGACY_MEMOS=1` (verified in both `compile_commands.json`), and `build-verify` does not exist in this tree. A syntax error in them would not have been caught. P3a's twin at `:4164` has the same property, so this is inherited — but P4a quadruples it. One `cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` compile-only configure closes it. |
| m-7 | `Managers.cpp:217` / `:236` | the `#if MOBILEGL_PIPE_PUSH` around the `SamplerViewCso` case (`:218`) sits **inside** the guard at `:182` that already opens the whole dispatcher. Redundant; delete the inner pair. |
| m-8 | `SanityTest.cpp` (the `auto& stale = table.GetOrCreate(first); EXPECT_EQ(stale, nullptr)` pair) | the first assertion cannot go red for its stated reason: an **adopted** backward generation also yields a null `BackendPtr&`, because `SlotTables.h`'s `GetOrCreate(handle)` does `entry.backend.reset()` before stamping. Only the following `EXPECT_NE(table.FindByHandle(second), nullptr)` distinguishes refusal from adoption. Keep both; fix the message so a reader does not trust the wrong one. |
| m-9 | `SanityTest.cpp` `ADeathNoticeForEveryP4aKindIsIdempotent` | the successor-acquire proof (slot really came back, generation moved) exists only for `SamplerViewCso`. The five object kinds get "second and third delivery is a no-op" but no ABA leg. ID-8's third case ("notice for a slot re-minted at a new generation") is therefore covered for one of six kinds. |
| m-10 | report §6.4 | "Three refusals, one per switched-over texture path" under-quotes: re-running the same standalone scenario, the default arm emits **six** distinct named shapes (3 texture, 2 framebuffer, 1 program). The classification claim ("no case fails for any other reason") is what matters and it holds — but `grep -c 'no applier record'` counts only the texture wording, so the number 3 is an artefact of the grep, not of the run. |

---

## 4. What I verified mechanically (and it all passes)

| check | result |
|---|---|
| **G5** — the four D-N functions in this package's files | Recomputed with my own mask-first extractor (comments and string/char literals blanked, definition = the one `name (` whose closing paren is followed by `{`, brace-matched in the masked text, **hashed from the original text**, exactly one definition per name). `08192d72` and `9bb95335` give byte-identical shas and they equal §4's four values: `d75537bc…` `StageBlocksIntoUnpackRing`, `a0c9994d…` `UnpackRingAvailable`, `f56d432f…` `UnpackRingAllocate`, `39a37249…` `RecomputeBackendColorSlots`. |
| `ShouldUseCaveatTextureFormat` / `BackendTextureFormatAddsAlpha` untouched | Both are in `Utils.cpp:245` / `:270`; `git diff --stat 08192d72 HEAD` touches three files and `Utils.cpp` is not one of them. |
| **DV-9 macro hygiene** | All fourteen macros are `#undef`'d immediately after their function (`:7212-7213`, `:7776-7783`, `:11830-11833`). `grep -rn 'MGB_LEVEL_\|MGB_TEXPARAM\|MGB_RBO_' MobileGL --include=*.h` → **no hits**: nothing leaks into `Managers.h` or any other header. |
| **DV-9 pull-text identity** | See m-5: 38 differing lines, all provably non-semantic. |
| **Purity (G13)** | `grep -rc pGLContext MobileGL/MG_Backend` → only `MG_Backend/MGPipe/PipeInputs.h:1`. `awk '/struct MGPipeResourceOps \{/,/^    \};/' MG_Pipe/PipeApply.h \| grep -c MG_State` → `0`. |
| **Logging discipline** | Diff adds 22 `MGLOG_E_ONCE`, 2 `MGLOG_E` (both arm-resolver refusals, once per process by construction), 12 `MGLOG_D`, and **zero** `MGLOG_I`. No leftover debug, no `printf`, no commented-out code. |
| **Exit order (ID-8)** | The only new namespace-scope object is `SamplerViewImpl::g_backendSamplerViews`, a `BackendSlotTable` whose `Entry::stateRef` is a `weak_ptr` and is never written on the handle overload (nothing instantiates `GetOrCreate(StatePtr)` for it), so it is not a holder of frontend `SharedPtr`s. No new `std::atexit`; `EnsureProcessTeardownSentinel()` is still armed by the first insertion of a driver-id-owning table. The four arm latches are function-local `static const Bool` in `inline` functions — trivially destructible, one guard variable, no exit registration. |
| **Death-notice idempotence** | Argument checked against the mechanism, not the prose: `MGPipeSlotAllocator::Free` erases the lifetimeId→slot map, `BackendSlotTable::OnFrontendObjectDestroyed` returns false on the null handle, and `ReleaseTwinAt` refuses a `Gen` mismatch. The `SamplerViewCso` arm (`Managers.cpp:228`) is a plain `OnFrontendObjectDestroyed` and cannot free another kind's slot because the allocator resolves `(kind, lifetimeId)`. `ADeathNoticeForEveryP4aKindIsIdempotent` drives the real `GetStateObjectDeathOps()->OnDestroyed`, not a stub, which is the right call. |
| **The legacy arm is unchanged** | Ran the full `MOBILEGL_PIPE_PUSH=0 ctest -L integration-gpu -j 8 -R DirectGLES`: **99% passed, 6 failed out of 491**, and the six are exactly the `DirectGLES.HandleRecycle.Handles.*` aborts — §6.3's central safety claim reproduced independently. |
| **The default arm** | Ran the full default-arm lane: **189 failed out of 491, 62% passed** — the report's number reproduced exactly. `-R HandleRecycle` on the default arm gives exactly the same six aborts and nothing else. Standalone repro of the two shapes confirmed verbatim: at `0x1ff`+`LEGACY_MEMOS=0`, `exit=134` and `Fatal{PipeLegacyMemosDisabled, "MOBILEGL_PIPE_PUSH leaves kMGPipeSubsystemTextureResources (bit 10) clear (or refuses it) and MOBILEGL_PIPE_LEGACY_MEMOS=0 disables the pre-handle texture cheap-gate trio, so there is no arm to run"}`; at `0x1fff` the abort is gone and only named refusals remain. **The `~/w7/p4a-esprytobj-lanes*.log` files no longer exist** (deleted per ID-5), so §6.3/§6.4's failure *sets* could not be diffed against their logs — see §6. |
| **No silent fall-back** | Walked every `#if MOBILEGL_PIPE_PUSH` arm's decline path: `SyncMipmapsToBackend` `:6102-6108`, `ResolvePushedBuiltinSampler` `:7225`/`:7237`/`:7249`, `SyncTextureParamsToBackend` `:7439`, `SyncToBackend`(fbo) `:8452`/`:8466`, `SyncReadBufferToBackend` `:8271`/`:8290`, `SamplerObject::SyncToBackend` `:11486`, `RBO::SyncToBackend` `:11730`, program `:11237`. Every one names the handle and returns (the program one deliberately leaves its memo rather than stamping 0, which is the right direction). The only frontend read left under a handle arm that I could not justify is m-3's dead one — and M-2's, which is a design gap rather than a fall-back. |

---

## 5. The twelve deviations, judged against ID-12

| DV | verdict |
|---|---|
| **DV-1** | Accepted as the C.7 boundary note ID-12 rules, **with M-4's correction**: the note explains why the resolution sites are E's, but not that the adoption helper and the two new registry methods end up with no caller on either branch. |
| **DV-2** | As ruled. c0c mints `kMGPipeDepthStencilModeDepth/Stencil`; D's rework replaces the bare `!= 0` in `MGB_TEXPARAM_DS_MODE` (`Managers.cpp:7498-7500`) with the named constant. The chosen direction (zero decodes to `GL_DEPTH_COMPONENT`) is right for the reason given and I could not fault it. |
| **DV-3** | KNOWN rework per ID-12; not re-found. **I verified the scope**: the packed field is compared in exactly five places (listed in §7) and **nothing else in D compares `MGPSubData::Target`** (`Desc.Target` is never read anywhere in `Managers.cpp`; `MGPFramebufferState::Target` at `:8463-8473` is a different field). Wire confirms the hazard: `PipeApply.cpp:756` stores `entry.UploadTarget = record.Target` — the **whole packed field** — while D matches `static_cast<Uint16>(TextureUploadTarget)`, so today every texture upload would be dropped, silently, on the handle arm. |
| **DV-4** | As ruled and the cheapest of the four: D never reads `Kind`, and `SyncReadBufferToBackend`'s emptiness test is `MGPipeHandleIsNull(record->ReadSurface.Res)` (`:8277`), which D-C1 fixes independently of the enum. c0c's constants make the code clearer but no rework is *required* here. |
| **DV-5** | As ruled, **but the ownership in the report is wrong** (M-3). All four masks are in **D's** file — `IsSnormFallbackAttachment` `Managers.cpp:8051`, `IsUnormFallbackAttachment` `:8067`, `IsAlphaWidenedColorAttachment` `:8098`, `IsIntegerColorAttachment` `:8149` — so moving them onto `MGPSurface::TextureTarget` is D's rework, not "contract + integrator, until then D leaves them alone". The analysis behind the deviation (no `TextureUploadTarget → TextureTarget` inverse exists; inventing one would feed a D-N-pinned function a guessed input) is correct and is why the field is the right answer. |
| **DV-6** | Accepted as argued, and I confirmed both halves: `DecodePushedDrawBuffers` (`:8237-8247`) is total for `Color0..7` and `-1 → None`, and the D-O row survives — the apply is still gated on `asTarget == FramebufferTarget::Draw` (`:8499`), as is the snorm/unorm/widened/integer mask block (`:8535`), so a `Target = Both` record synced as READ still cannot land `glDrawBuffers` on the wrong framebuffer. |
| **DV-7** | Accepted as a boundary note, **with M-3's correction to who owns it**. |
| **DV-8** | Accepted; D-H3 is unambiguous and the code matches it (`:11582` stamps identity + serial only). The step message does read the other way and the deviation says so. |
| **DV-9** | Accepted as G1-forced, **with m-5's rewording** and m-6's gap. The measurement behind it (lambda `$_N` renumbering; +9 / -2 bytes) is the right kind of evidence and I did not try to re-derive it. |
| **DV-10** | Fine. |
| **DV-11** | Fine structurally — every `#if !MOBILEGL_PIPE_LEGACY_MEMOS` body is inside a push arm — with m-6 (never compiled) and one asymmetry worth a line in the rework: `SyncMipmapsToBackend` and the framebuffer pair express the arm choice as `if (pushedStorage == nullptr)` / `if (FramebufferSubsystemEnabled())` rather than `#if/#else`, so **their** pre-handle text is not removed under `LEGACY_MEMOS=0` and gets no "loud unreachable" body. Unreachability still holds (all four resolvers pass `legacyArmSurvivesLegacyMemos=false`), so this is a consistency note, not a defect. |
| **DV-12** | Fine; §4's one-off equivalent copies the parent script's shape faithfully and I reproduced its four shas with an independently written extractor. |

---

## 6. What I could not check, and why

- **§6.3/§6.4's failure *sets*.** `~/w7/p4a-esprytobj-lanes*.log` are gone (ID-5 says the owner deletes
  them at the end of the round, so this is compliant, not a lapse). I reproduced the default arm's
  **count** (189/491) and the six aborts exactly, and I confirmed on a representative scenario that
  the only `[ERROR]: MGPipe` lines are named refusals — but the per-case classification of all 183
  rests on the report's own log, which no longer exists. **Keep the logs until the review closes** in
  future rounds; §6.4's whole argument is a log-line attribution.
- **The `0x1ff` lane.** Only the `0` lane was re-run in full (6/491, the six aborts and nothing else).
  §6.3 says both outer lanes see the identical set *because* the ctest `ENVIRONMENT` property replaces
  the job env, which the `0` run confirms as far as it goes; the `0x1ff` run would prove the same
  property from the other side and costs four minutes.
- **`build-verify`** does not exist in this tree, so G4's retain-mode comparison — the *only* gate that
  can check a consume-and-clear set (D-D5) — has not been run anywhere in the phase yet.

---

## 7. Rework list for D's verification round

**Blocking (must be done before the real-path round):**

1. **C-1** — decide and implement the not-currently-bound framebuffer case. Sites:
   `Managers.cpp:8213-8225` and `:8459-8479`; the five callers are `DirectGLES.cpp:6474`, `:6475`,
   `:7960`, `:7980`, `:7997`, `:8017` via `SyncAndBindFramebufferObject`. Coordinate with A/B if the
   answer is a new record; raise it as a contract deviation if it is not.
2. **M-1** — make the handle arm's per-level dirty question cover the server's own re-dirty
   (`Managers.cpp:6046-6049` and `:6053-6060`), and enumerate the three server-side `MarkStorageDirty`
   sites in the comment beside it (`Managers.cpp:5127`, `DirectGLES.cpp:7406`, `DirectGLES.cpp:6735`).
3. **ID-12 DV-3** — decode the HIGH BYTE. The cheapest correct patch is **two lines**, in the two
   helpers rather than at the three call sites, so the `static_cast<Uint16>(TextureUploadTarget)`
   spelling stays where it reads naturally:
   - `Managers.cpp:3752` (in `FindPipeTextureUpload`, `:3745`): `pending.UploadTarget == uploadTarget`
     → `MGPipeSubDataUploadTargetOf(pending.UploadTarget) == uploadTarget`;
   - `Managers.cpp:3768` (in `ConsumePipeTextureUpload`, `:3757`): `pending.UploadTarget !=
     uploadTarget` → the same decode.
   The three call sites that pass the bare enumerator are `Managers.cpp:6047-6048`
   (`MGB_LEVEL_NEEDS_UPLOAD`), `:6056` (`MGB_LEVEL_UPLOAD_DONE`) and `:6702-6705` (the staging block's
   `pendingUpload`) and need no edit under that patch — but they must be re-read against c0c's final
   helper signature (`Uint32` arguments). **Nothing else in D compares `MGPSubData::Target`** —
   verified by grep; `Desc.Target` is never read and `MGPFramebufferState::Target` is a different
   field.
4. **ID-12 DV-5** — move the four cross-object masks onto `MGPSurface::TextureTarget`:
   `Managers.cpp:8051` `IsSnormFallbackAttachment`, `:8067` `IsUnormFallbackAttachment`, `:8098`
   `IsAlphaWidenedColorAttachment`, `:8149` `IsIntegerColorAttachment`. All four are D's file (M-3);
   `ShouldUseCaveatTextureFormat` / `BackendTextureFormatAddsAlpha` stay byte-identical and only their
   *inputs* change. Consult `TextureTarget` only when `Kind == Texture`, treat `0xFFFF` as "unknown"
   and decline rather than guess.
5. **ID-12 DV-2/DV-4** — swap `MGB_TEXPARAM_DS_MODE`'s `!= 0` (`:7498-7500`) for
   `kMGPipeDepthStencilModeStencil`; optionally tighten `SyncReadBufferToBackend`'s emptiness test with
   `kMGPipeSurfaceKindNone`.
6. **M-2** — either drive `SyncMipmapsToBackend`'s shape from `pushedStorage->Desc` (the brief's d2),
   or record it as an explicit, argued deviation with an owner. Silence is the one option that is not
   available, because no monolith lane can expose it.

**Non-blocking but owed in the same round:**

7. **M-3** — correct DV-7 and §7-13 in `esprytobj-v1.md`; tell the integrator that the attachment-walk
   re-key is D's, not E's, and that E closed D-E3 through `SyncReadFramebufferTextureAttachments`.
8. **M-4** — give `AdoptTwinByHandle` / `GetOrCreateByHandle` / `ReleaseByHandle` a first caller, or
   move them to the package that will have one, and say which in the report.
9. m-1, m-2 — consume `SrcOffset`; assert the per-region stride invariant.
10. m-3 — hoist `MGB_TEXPARAM_BORDER_FORM` inside the `borderColorReadable` guard.
11. m-5 — reword DV-9 to the measured truth (23 `__LINE__` constants + 13 parenthesis-only + 2
    re-flows; symbol-set and byte-size unchanged).
12. m-6 — add one `cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF -DMOBILEGL_PIPE_PUSH=...` **compile-only**
    configure to the package's verification block so the four "loud unreachable" bodies are at least
    compiled once.
13. m-7 — drop the redundant nested `#if`.
14. m-8, m-9 — fix the over-claiming assertion message; add the successor-acquire leg to the five
    object kinds in `ADeathNoticeForEveryP4aKindIsIdempotent`.
15. m-10 — restate §6.4's refusal census with all six shapes.
16. Keep `~/w7/p4a-esprytobj-lanes*.log` until the review of the round closes.

---

## 8. What the integrator must re-run on the integrated tree

Beyond §7 of `esprytobj-v1.md`, which is otherwise sound:

1. **The DSA / named-framebuffer set, explicitly**, because C-1 is the one failure that will look like a
   B-side seam defect:
   `ctest --test-dir build-push -L integration-gpu -R 'ClearThenReadPixels|Orientation|FramebufferChurn|DepthStencilReadback'`
   plus `ctest --test-dir build-push -R 'FramebufferTest.NamedFramebuffer'`. If C-1 is fixed, these go
   green with the rest; if the fix is a decline-with-a-name, they will still be red and the integrator
   needs to know that *before* reading the number.
2. **The image-bindable remint set**, for M-1, one dispatch and one readback apiece:
   `ctest --test-dir build-push -L integration-gpu -R 'ImageLoadStoreSso|FormatlessImageBake|ImageSizeAfterRespec|ImageTargetKind|SampledSetStaleness'`,
   and confirm `TextureParamsWithoutASamplerViewScenario.AnImageUnitOnlyTexturesSwizzleSurvivesARequireImageBindableStorageRemint`
   is green **and** that a content assertion exists beside the swizzle one — today's case checks the
   swizzle, which M-1 does not break.
3. **G4's retain-mode comparison** (`build-verify`, `-L integration-verify`) — it has not run anywhere
   in this phase and it is the only mechanism that can check a consume-and-clear set. D-D5's whole
   safety argument is unverified until it does.
4. **The residual-refusal check, per shape, not per count.** With `MOBILEGL_LOG_FILE_PATH` set, the
   integrated tree must show **zero** of all six shapes, not three:
   `no applier record on the handle arm, so its {parameters,built-in sampler,storage and uploads}`,
   `has no {draw,read}-target applier record`, and `has no shader-CSO applier record`. A residual of
   any one of them is a seam defect (P3a's lesson), and the framebuffer two are the ones C-1 will
   masquerade as.
5. `MOBILEGL_PIPE_PUSH=0x9ff` once F's `ObjectSubsystemControlScenario` exists — D's half (the
   bit-11-requires-bit-10 `MGLOG_E` and the legacy fall-back, `ResolveSamplerSubsystemArm`,
   `Managers.cpp:3647-3665`) is in and correct.
6. The six itest phase pins `0x1ff → 0x1fff` (`MG_IntegrationTest/CMakeLists.txt:949, 1065, 1075, 1149,
   1183, 1188, 1219`) and `0x80000000000001ff → 0x8000000000001fff` (`:1070, 1080`) — F's, and until
   they move the six `.Handles` aborts are red on the integrated tree. I reproduced them and their
   `Fatal` verbatim; the attribution in §6.3 is correct and needs no re-argument.
7. `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` on the unit and integration runs (ID-18/21), and the
   ID-8 leak/idempotence half on the **DirectVulkan** lane — which lives in F's `HandleRecycleScenario`
   `…ReturnTheirSlots` cases (G8b), not in D's `SanityTest.cpp`. Confirm F wrote one for
   `SamplerViewCso`, the kind with no frontend object: nothing in D or E can test it there.
