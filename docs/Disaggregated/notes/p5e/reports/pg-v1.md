# pg — programs (P5e wave 2), v1

Branch `p5e/pg`, head **4d1499ef**, base **f6cfcbd3**. 3 commits, 18 files, +2071/-316.
Gate: build 0 · unit **2210/2210** · integration-split **111/111** · gens all rc=0 ·
`one` on the six named scenarios **212/212**. Strict lane run before AND after (§5).

## 1 What changed, per file

| file | region | what |
|---|---|---|
| `ProgramArtifactsCodec.{h,cpp}` | header note, `:242` note, +55 tail | names the skipped member + reads the libc++ caveat (§2); `ProgramArchive`, `kProgramArchiveMaxStages`, `Encode/DecodeProgramArchive` — the FRAME `[stageCount][stages][codec stream]`, bounded before it multiplies, refusing a stage count that disagrees with the module count |
| `MG_Pipe/PipeApply.h` | `:41-52`, `:384-420`, `:1327-1345` | `ProgramArchive` fwd-decl; `MGPipeShaderCsoRecord::Archive`; `MGPipeApplyCreateShaderState`'s 4th argument |
| `MG_Pipe/PipeApply.cpp` | `:2854-2918`, `:3000-3060` | `MGPipeApplySetProgramBindings`'s body; the create ADOPTS the archive and clears the three tails + `Signature`, advancing `BindingsSerial` |
| `MG_Pipe/PipeRoute.{h,cpp}` | escape table + 2 sites | the FIFTH escape (§7.3); `CreateShaderState` grows the stage list; `MGPipeRouteSetProgramBindings` |
| `MG_Remote/Client/WireTables.cpp` | `:488-600` | the create frames the archive and counts its bytes into PipeStats; new `Wire_Escape_SetProgramBindings` (three tails, per-name SEG_STAGE staging) |
| `MG_Remote/Wire/PipeWireCodec.cpp` | the two arms | decodes onto the heap into the record's own archive; op 80 resolves and NUL-checks every name span and calls the applier |
| `MG_Impl/Pipe/ProgramEmit.h` | `:90-110`, `:106-180`, `:215-330` | `MGPipeStorageOverrideSignatureEntry`; `EmitProgramBindings` + latch; `Desc.LinkStatus`; the stage list; the bindings latch cleared on re-issue |
| `Managers.h` | `:2341-2480` new + 6 signatures | `ProgramArchiveSource`, `ProgramBuildSource`, `ProgramHandleArm()`, `m_syncedBindingsSerial`, `ProgramBlockBindingFromRecord` / `ProgramSamplerUnitFromRecord` |
| `Managers.cpp` | `:745`, `:10913-11060`, `:11123-11370`, `:11907-13400` | the source's two constructors and its accessors; every helper of the build re-typed; `SyncToBackendFromSource` + the two public heads; the seam stub deleted |
| `DirectGLES.cpp` | `:4097`, `:4230-4620`, `:4800-4830`, `:5460-5800`, `:5900-6030`, `:6400-6430` | the handle stash; `ResolveGlobalConstantsRecordForHandle` + the MapUBO refusal; `ResolveFragColorBroadcastCount`'s policy; `SyncCurrentProgramByHandle`; both Prepare lines; `BindCurrentProgramWithResources`; `GetCurrentBackendProgram`; `CurrentProgramMayNeedPerSubDrawBuiltins` |
| tests | `ProgramEmitTest` +4 cases; 5 files mechanical | §7.5 |

**The one-body decision (G1).** `SyncToBackend`'s ~1070-line body is written ONCE against a
reference `src` whose TYPE is `PrgramImpl::ProgramBuildSource` — `ProgramArchiveSource` in a push
build, `ProgramObject` in a pull build. The view's accessors carry ProgramObject's names one for
one, so the pull build compiles the same text through the same accessors. The alternative was an
`#if`-duplicated 1100-line body. **Exact pull deltas: §6.**

## 2 Codec completeness (CONTRACT-P5E §5.5)

The skipped member is **`LinkArtifacts::program`**, the live `SharedPtr<glslang::TProgram>` that
`VisitFields` omits (57 of 58). **Proven unread by either twin, by closed grep:** every read in
the tree is inside `ProgramLinkTask.cpp` (`DoReflection`) plus the three `reset()` sites;
`MG_Backend/` contains none, and `ProgramTranslationCache` asserts it null at insert. It could not
be carried in any case — it points into a glslang arena no archived instance owns.
`ProgramArtifactsCodec.cpp:242`'s "libc++ caveat" is about the **struct-size ECHO**, not a member:
under the NDK's unpinned branch the word is 0, which is exact for an in-process split (one build
writes and reads) and becomes a real check at P6's process boundary. Both verdicts are written
where they were asked for.

## 3 Kimi audit rows 91-110

**Retired** (record/handle answers them now): 93 (`Desc.LinkStatus`/`SpirvStatus`), 94
(`ResolveProgramTwin`), 96 (`record.Serial`/`BindingsSerial`), 97 (`record.Signature`), 98
(`SyncToBackendByHandle`), 99 (`ResolveGlobalConstantsRecordForHandle`), 101, 102
(`Desc.GlobalUboSize`/`GlobalConstantsVersion`), 103 (**`MapUBO` is Fatal now**, gap G-C), 104
(`ProgramBlockBindingFromRecord`), 105 (`BindingsSerial` + `ProgramSamplerUnitFromRecord`), 107,
108, 110 (handle arm; kept as the monolith stamp).

**Left pulled, and why:** 91/92 (`GetProgramForDraw`/`ForDispatch` — my sync no longer reads them,
but the accessor CALL stays for vi's and tx2's consumers, §4.3); 100 (`BindCurrentTextures`' memo —
tx2's line range, §4.1); 106 (vi's); 95 (`MOBILEGL_PIPE_LEGACY_MEMOS`, unreachable under a
transport); **109 left barriered BY CONTRACT §5.5** — `GetBackendProgramId` is a GL-thread entry
point (P9) and `ShaderStorageBlockBinding` is reached from the barriered
`set_storage_block_binding` (ID-85, pg's trailing item).

**Not found: none** — all twenty rows exist at this head.

**The five scope sites → three retired, two kept.** `ResolveGlobalConstantsRecord`,
`SyncCurrentProgram` and `BindCurrentProgramWithResources` are unreachable under a transport (the
scopes stay as the monolith glue's, ruling 1); the other two are row 109's contract exceptions.

## 4 Red-once evidence (executed, quoted verbatim, reverted)

**(1) Empty the applier's block-binding tail.** The two named scenarios are NOT in
`integration-split` (they are `DirectGLES.*`, the monolith lane), and with the tail emptied the
split lane stays **111/111 green** — no split scenario binds a named uniform block to a non-zero
point. The statement moved to the unit case the brief also names:
```
../MobileGL/MG_Test/Pipe/ProgramEmitTest.cpp:894: Failure
    Which is: 0
    Which is: 3
[  FAILED  ] ProgramEmit.TheThreeBindingTailsAreWholeSetsAndAReIssuedCreateDropsThem
```
**(2) Blob refs at Size 0, companion pointers removed** (decoder passes `nullptr,nullptr,nullptr`);
`Split.TriangleScenario.AVboBackedTriangleReachesReadPixels` aborts:
```
[Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{ProtocolCorruption} create_shader_state {slot=1, gen=0}: the record declares no blobs and carries no artefacts (stages=0x11, globalUboSize=0, bound 16777216)
```
**(3) The frontend-keyed resolution back, id's hook forcing "unbarriered".** `PrepareForDraw`
routed to `SyncCurrentProgram` + `MGPipeApplierSetCurrentRecordBarriered(false)` at its top —
ID-102 kept: the probed action IS the registry lookup, not an object built on the apply thread.
```
[Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{RoleViolation, "MGPipeSlots"} - the apply thread called MGPipeSlots().HandleOf. With an active transport the client slot allocator is client-only memory (CONTRACT-P5C §3.1, rule E; CONTRACT-P5E §4.4): a handle arrives already minted in a record, and a server that resolves or mints one off a frontend object's lifetime id is reading memory that will not exist on its side of a real split. A named exemption scope admits it only while the record being applied is BARRIERED (barriered=0) and, for Magma's P7 debt, only on a DirectVulkan server
```
**(4) `strict` names `GetProgramForDraw@draw_vbo` — NOT DEMONSTRABLE AT THIS HEAD**, and the
reason is structural rather than a miss: see §5. The marker is in NEITHER table.
**(5) The global-constants record declines under run-ahead** (what forcing bit 12 off does to the
record arm); 8 split entries abort:
```
[Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedVerb, "set_global_constants"} - the ShaderCso record {1, 0} does not answer the default uniform block for a draw of a program that declares 16 bytes of it, and MapUBO() is the client's live scratch array
```

### ID-87's measurement — numbers, not an argument

`EncodeProgramArchive` instrumented at the one place the bytes exist; whole split lane, **80
links**, then reverted:
```
us:    min=3.2  p50=4.5  p90=5.4  max=6.4  mean=4.6
bytes: mean=2516, max observed 6070
```
PipeStats over `TriangleScenario.*` independently: `cso-blob-bytes = 4538` for 2 links.
**Links per steady frame = 0** — the re-issue latch is `GetLinkVersion()`, which only a real
`glLinkProgram` moves, so the cost lands on loading frames. Slope ≈ **1 µs per KB**, so ruling 8's
0.2 ms/frame trigger needs ~200 KB of archive linked in ONE frame (~33 of the largest measured).

**The half ID-87 actually asked for — MC + a shader pack on the Redmi — IS NOT DONE:** no device
here and adb is forbidden by the package instructions. These are lavapipe with the lane's small
programs, and I will not extrapolate a ruling trigger from a 6 KB sample. **Blocker for declaring
ruling 8 settled**: run the device arm at the phase gate, or land lazy-encode as a precaution.

## 5 The strict lane, before and after

Run on this tree at **f6cfcbd3** and at **4d1499ef**. Both: **108/111 failed, 3 passed**; 88 of
111 entries keep a private log (the ID-53 baseline saw 22 — the per-entry log path now covers the
whole lane, so this is a wider slice than `STRICT-BASELINE-2fde7034.md`'s).

| marker | before | after | owner |
|---|---|---|---|
| `GetFramebufferBindingSlot@Clear` | 39 | 39 | fb |
| `GetBoundVertexArray@DrawArrays` | 14 | 14 | vi |
| `GetImageTextureBinding@BindImageTexture` | 13 | 13 | fb |
| `GetTextureUnitObject@Clear` | 9 | 9 | tx2 |
| `GetTextureObject@CopyImageSubData` | 6 | 6 | tx2/fb (trailing) |
| `GetProgramForDispatch@DispatchCompute` | 3 | 3 | **pg**, blocked on tx2 (§4.3/§7) |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | 2 | **pg**, allowlisted trailing (ID-85) |

**IDENTICAL, and that is the honest result.** Each entry aborts on its FIRST pulled row, and in
every entry of this lane a framebuffer, VAO, texture or image row precedes any program read — so
no program field of mine was ever first, none could disappear, and none surfaced behind one.
`GetProgramForDraw@draw_vbo` appears in neither. This lane will only measure this family once fb,
vi and tx2 have landed; what pg can show is the three executed Fatals above.

## 6 G1 / monolith impact — the deltas, named

Every new symbol is under `MOBILEGL_PIPE_PUSH` / `MOBILEGL_BUILD_DISAGGREGATED`. **I could not run
the comparator** (`symbol_report.py` needs pull builds of base and head; the gate script has no G1
step). The deltas the pull build WILL see, from the diff:

1. `SyncToBackend(const SharedPtr<ProgramObject>&)` keeps name and signature; its body gains
   `const ProgramBuildSource& src = *stateProgramObject;` and every read becomes `src.X()`. Same
   loads, same inlining **expected — an expectation, not a measurement**.
2. `CacheResourceLocations`: `(const SharedPtr<ProgramObject>&)` → `(const ProgramObject&)` —
   mangled-name change + a dropped `SharedPtr` deref.
3. `RebindImageUniformsToFrontendUnits`: likewise, and it loses one null test the caller already made.
4. The other six helpers keep their signatures exactly (`ProgramBuildSource` IS `ProgramObject` there).

Expected pull report: **2 renamed + 2 resized, 0 added, 0 removed**, all in one function family.
**Ruling wanted:** accept those four, or ask for the `#if`-duplicated body. I recommend accepting —
the duplicate is 1100 lines two arms would have to be kept in step by hand for ever.
Push + `Transport=monolith` keeps its frontend arms through
`ProgramHandleArm() = Transport != Monolith && ProgramSubsystemEnabled()`.

## 7 Seams, assumptions and rulings needed

1. **tx2 — `BindCurrentTextures`' memo key (ruling 6 / ID-86).** Its range, so I left a comment on
   `ResolvedTextureBindingMemo` instead of editing. **What I expect:** the four program fields
   replaced on the handle arm by `{DrawProgram.Slot, .Gen, record Serial, BindingsSerial}`, AND-ed
   with tx2's `SamplerViewsSerial`. Both serials are needed — a relink with unchanged texture
   resolution still hands the twin new unit assignments.
2. **tx2 — `ResolveAndBindUnitTextures`/`sampledTargetForUnit` (`:4834-4948`).** BRIEF gives the
   range to tx2, §5.5 gives the function to pg; I left it to tx2.
   `PrgramImpl::ProgramSamplerUnitFromRecord(record, location)` and `ProgramArchiveSource`'s
   uniform-type accessors are public for it.
3. **vi + tx2 — `GetProgramForDraw`/`ForDispatch`.** `:4781` and `:6126` are my lines but their
   consumers are vi's `SyncCurrentVertexAttributeValues` and tx2's `BindCurrentTextures`; the call
   can only become conditional once both stop taking a `SharedPtr<ProgramObject>`.
4. **sb — the UBO loop.** I own `:5463` (the block binding, from the record's first tail) and left
   `:5464-5497`; `binding` is still a `Uint` in the same index space.
5. **Files touched outside my listed scope, all mergeable:** `PipeWireCodec.cpp` (op 80's own
   `case` — `PipeApplier.cpp` has no `OnSetProgramBindings`; every non-verb row's dispatch lives in
   `ApplyChecked`, so the task file's pointer is an estimate); `PipeCatalogueTest.cpp` and
   `RemoteClientTest.cpp` (escapes 4→5, total 38→39 — op 80's `gMGPipeContext` slot STAYS null, so
   those pins are unchanged); `PipeWireCodecTest.cpp` (the create stages the FRAMED archive);
   `ResourceEmitTest.cpp`/`CompositeResolverTest.cpp` (the create's 4th argument, `nullptr`).
6. **RULING: the emission ORDER is the opposite of the draft's note.** `MGPipeTypes.h`'s
   `MGPProgramBindings` comment says "Emitted BEFORE create_shader_state". That is wrong against
   the applier the same contract specifies: a re-issued create CLEARS all three tails, so bindings
   published ahead of it are wiped. I emit them AFTER the create at the same validate point and
   said so in both files. **That c0e comment still says the wrong thing and needs a one-line fix.**
7. **RULING: one signature authority per arm** — the handle arm stamps and compares the CLIENT's
   `record.Signature` verbatim, so the two commutative hashes never have to stay bit-identical.
   Confirm, or the equality needs a test that can reach both (nothing in `MG_Test` can today:
   MG_Backend includes nothing from MG_Impl).
8. **`MGPipeApplyCreateShaderState` grew a fourth argument, undefaulted**, on PipeRoute.h's stated
   reasoning; five call sites moved, three in other packages' test files.
9. **ID-87 is not settled** (§4).

## 8 What I did not do

* `set_storage_block_binding` by `ShaderCso` — the TRAILING item, explicitly not this package
  (ID-85); `ShaderStorageBlockBinding`'s monolith body is kept verbatim.
* `BindCurrentTextures`' memo and `ResolveAndBindUnitTextures` — tx2's ranges (§7.1-2).
* The two `GetProgramFor*` accessor calls — blocked on vi and tx2 (§7.3).
* The composite uniform-mirror fire rate (BRIEF §5) — still unmeasured. The mechanism is the latch
  on `(backendStateVersion, blockBindingVersion)` and both setters' equality bail-outs, so a mirror
  re-setting unchanged values emits nothing; that is the mechanism, not a measurement.
* The G1 comparator run (§6) and ID-87's device arm (§4).
