# P4a package A — `c0`, the contract. Result, v1

**Commit `08192d72`** on `refs/heads/p4a/contract`, branched from `feat/disaggregated@37da3c3a`.
**Tag `refs/tags/p4a/contract` → `08192d72`** (both refs exist and are the same object; ID-2's
"spell branch refs `refs/heads/...`" applies from here on). **Not pushed.**

Message, exactly as C.0 specifies, single line, no attribution:

> `[Feat] (Pipe): land the P4a contract - the resource target enum, the framebuffer target byte, the texture params' builtin sampler, the sampler-parameter field table, four subsystem bits, seven dirty arms and the program archive codec`

43 files, +3802 / −145. Working tree clean. `p4a/wire` continues from here; B/C/D/E/F branch from
the tag.

---

## 1. Verdict on the hard rules

| rule | result |
|---|---|
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 renamed / 0 resized.** The pull `.so` is byte-for-byte the same size as `$BASE`'s (19114360 file, 10806323 `.text`). P4a's admitted-resize set stays EMPTY. |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (7 controls tripped, positive control OK) |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` after regeneration | rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0 — **27 negative controls** (was 21), all tripped |
| `check_include_closure.py --mode both --self-test --require-all` | rc 0, 4 probes, 0 problems, 6 controls tripped |
| `p3a_untouched_regions.sh 37da3c3a HEAD` | rc 0, the eleven byte-identical |
| three builds compile (pull / push / verify) | all three |
| `ctest -L unit` in all three | **1635/1635 in each**, 0 failed |
| ctest names pull == push | `diff` empty |
| no name removed vs `~/w7/p4a-before-ctest-names.txt` | empty; 13 names added |
| only C.0's files | **no** — 6 deviations, §3 |
| one commit, exact message, no attribution, tagged, not pushed | yes |

---

## 2. Per-file summary

### `MG_Pipe/MGPipeTypes.h` (+247)
- **`enum class MGPipeResourceTarget : Uint8`** with `kMGPipeResourceTargetBuffer` moved in from
  `ResourceTracker.h`, the drift `static_assert`, `kMGPipeResourceTargetUnmapped = 0x100`, the
  `default:`-less `MGPipeResourceTargetForTextureTarget(TextureTarget)` table, and
  `MGPipeEveryTextureTargetIsMapped()`'s `static_assert` (plus a second one pinning that
  rectangle ≠ 2D).
- **`enum MGPipeBindBit : Uint16`** moved here verbatim (twelve enumerators + `kMGPipeBindNone`).
- **`MGPResourceDesc`**'s `Target`/`BindMask` comments re-pointed at the two new spellings.
- **`MGPTextureParams` 32 → 40**: `BuiltinSampler` at +8, `SamplerResync` at +26, `Pad0` at +27.
- **`MGPFramebufferState::Pad0` → `Uint8 Target`**, size unchanged at 304, plus
  `enum class MGPipeFramebufferTarget : Uint8 {Draw, Read, Both, Count}` and the amended
  `Complete` comment (it is `CheckCompleteness()`, never `glCheckFramebufferStatus`'s answer).
- **`kMGPipeMaxColorAttachments = 8`** + its `static_assert` against `kMGMaxDrawBuffers`.
- **`kMGPipeMaxTextureUnits = kMGPipeMaxImageUnits = 192`**.
- **`MGPipeResourceRespecifyNeedsAck` narrowed** to `Immutable != 0 && Target == Buffer`.

### `MG_Pipe/PipeFields.def` (+27)
`F(Target)` on `MGP_FIELDS_MGPFramebufferState`; `F(BuiltinSampler)` + `F(SamplerResync)` on
`MGP_FIELDS_MGPTextureParams`; new `MGP_FIELDS_SamplerParameters` (sixteen members) and
`P(SamplerParameters)` in `MGP_VERIFY_PAYLOAD_LIST` → **72 verified payloads**.

### `MG_Pipe/MGPipe.h` (+33)
Bits 9/10/11/12, `kMGPipeSubsystemsMigratedAtP4a = 0x1fff` with a `static_assert` tying it to the
four bits. `0x7f` and `0x1ff` untouched.

### `MG_Pipe/MGPipeHandles.h` (+11)
Comment only: `AllocateComposite` named as the one door into the composite band, with the
two-release-path note.

### `MG_Pipe/PipeApply.{h,cpp}` (+326 / +123)
Three bounds, three record types, `MGPipeResourceRecord`'s four new members, five new object
tables + the composite table, P4a's working state, `RefusedObjectCalls`, `Reset` /
`ReleaseObjectRecords` extended, **fifteen apply entry points with complete signatures and stub
bodies**, `MGPipeUnmigratedEmulation`. `MGPipeResourceOps` untouched (0 `MG_State` hits inside the
block).

### `MG_Pipe/Coverage.def` (+46)
Six P4a rows in `MGP_COVERAGE_EMITTED_LIST` and the paragraph explaining why those six and not
the other four (§4.7).

### `MG_Pipe/PipeMutation.h` (+60)
Six death-helper declarations with the three-step order written out.

### `MG_Pipe/DirtySurface.def` (+91)
Four new rows, `MarkProgramForDeletion → kExplicitDestroy`, updated reasons on the other two
destroy rows, the `Create*`/`Pop*` exclusion paragraph, and **two `MGP_DIRTY_SURFACE_UNDECIDED_LIST`
entries** (§3, D6).

### `scripts/gen_pipe_dirty_surface.py` (+56)
`MUTATOR_PREFIXES` gains `Use` and `Bind`; six new self-test controls (19, 20a–20d, 21).

### `MG_Impl/Pipe/Tracker.h` (+120)
`kMGPipeDirtyEmittedAtP4a`, seven `MGPipeSubsystemForDirty` arms, bit 11's shutter widened with a
second widened counter `m_readFramebufferBind`, the `SetDrawBuffer` trap recorded, header and bit
comments corrected.

### `MG_Impl/Pipe/SetHashSuppressor.h` (+25)
`SetFramebufferState` appended before `Count`; the three unit slots relabelled `P4a - wired` with
P3b/P4b named as the owner of the backend-debounce deletion.

### `MG_Impl/Pipe/SlotAllocator.{h,cpp}` (+42 / +129)
`AllocateComposite(lifetimeId)`, the band's own free list and slot table, the `EntryOf` resolver,
and `Free`/`IsLive`/`GenOfSlot`/`LifetimeIdOfSlot`/`FindByLifetimeId`/`HighWater`/`FreeCount`/`Reset`
routed through it (§3, D13).

### `MG_Impl/Pipe/PipeFill.{h,cpp}` (+365, `.cpp` only)
Five emit-header includes; six `SubsystemForEmitter` arms; seven pairing `static_assert`s (all
against `SubsystemForEmitter`, never a constant) + the "no dirty bit owns the texture-resource
subsystem" assert + the two texture-unit capacity pins; `kMGPipeWiredSubsystems` as an OR of four
per-family constants with four `0 || own-bit` asserts; six `EmittedCallSuppliesTheWholeField`
false arms; seven stub emitters and the validate-point ladder; the five emitters' `Reset()` on
`FreshlyPrimed`; **six death-helper bodies**.

### `MG_State/GLState/ProgramState/ProgramArtifactsCodec.{h,cpp}` (new, 76 + 303)
The archive serializer. `MG_State/GLState/ProgramState/ProgramArtifacts.h` (+21) gains the
"the serializer now exists" paragraph and its five stale "when one exists" notes updated.

### `MG_Util/Metrics/PipeStats.{h,cpp}` (+32 / +26)
Five `CallClass` members in the existing push block; a **new** push block at the tail of
`ByteClass` carrying `CsoBlobBytes`; both name tables, the short-name table and
`FormatWindowLine`'s new `emit[...]` bracket.

### Tests
`MG_Test/Util/PipeStatsTest.cpp` (+23, six names pinned), `MG_Test/Pipe/PipeCatalogueTest.cpp`
(+200), `MG_Test/Pipe/TrackerTest.cpp` (+42, §3 D11), six new `MG_Test/Pipe/*Test.cpp`
stubs (each with one real shape-pinning case), `MG_Test/Program/ProgramArtifactsCodecTest.cpp`
(+375), and both `CMakeLists.txt`s.

### Build/config
Root `CMakeLists.txt` (+6, `ProgramArtifactsCodec.cpp` inside `if (MOBILEGL_PIPE_PUSH)`),
`MobileGL/Config.h` (+19, the four new bits and their dependencies documented),
`MobileGL/ConfigLoader.cpp` (+12, the push default `kMGPipeSubsystemsMigratedAtP3a` →
`…AtP4a`).

---

## 3. Deviations from the brief, each with its reason

**D1 — five client emit headers are CREATED by `c0`.**
`MG_Impl/Pipe/{FramebufferEmit,TextureEmit,SamplerEmit,ImageEmit,ProgramEmit}.h` are not in C.0's
file table, and C.7 gives the first two to B and the rest to C. They have to exist at `c0`
anyway, because C.0 asks for two things that are only jointly satisfiable if they do: the
validate-point ladder lives in `PipeFill.cpp`, and `kMGPipeWiredSubsystems` is "an OR of four
per-family constants each **defined in its family's emit header**… **with no file touched
twice**". If the emitter bodies stayed in `PipeFill.cpp`, B and C would have to edit that file to
give them bodies — which is exactly what C.7/E forbid. So each header lands with (a) its family's
`kMGPipeWired*Subsystem = 0`, (b) a stub emitter class whose methods return 0, and (c) a
never-destroyed singleton accessor. **B and C replace the class bodies and the constant in their
own headers and never touch `PipeFill.cpp`.** This is the same shape as C.0's six stub test files.
`CompositeResolver.h` is *not* created: nothing at `c0` needs it, and it stays wholly C's.

**D2 — `MGPipeResourceTarget` has THIRTEEN enumerators, not twelve.**
`TexRect` is appended **after** `TexBuffer`, so every value D-A3 names keeps its number.
`MobileGL::TextureTarget::TextureRectangle` exists at `$BASE`, the mapping table may not carry a
`default:` arm, and folding rectangle onto `Tex2D` would erase a distinction the frontend keeps
and both backends switch on (`Managers.h:1246`, `UniformManager.cpp:132/170/1408`,
`VkTextureManager.cpp:2596`, `VkRenderPassManager.cpp:1041`) — while `Tex1D`, which Espryt lowers
to 2D in exactly the same place, still has an enumerator of its own. A second `static_assert` pins
`Tex2D != TexRect` so a later "simplification" is a build break.

**D3 — `kMGPipeResourceTargetBuffer` MOVED to `MGPipeTypes.h` (and narrowed `Uint16` → `Uint8`).**
D-A3(a) implies it stays in `ResourceTracker.h` with a cross-file `static_assert`, but D-A2's
narrowed predicate lives in `MGPipeTypes.h` and spells the constant, and `MG_Pipe` may not reach
into `MG_Impl`. The drift `static_assert` moved with it; `ResourceTracker.h` carries a comment
pointing at the new home. All three existing uses compile unchanged.

**D4 — no `using` alias for `MGPipeBindBit` in `ResourceTracker.h`.**
D-A4 asks for one. It is neither possible nor needed: both files are `namespace
MobileGL::MG_Pipe` and `ResourceTracker.h` includes `MGPipeTypes.h`, so `using MGPipeBindBit =
MGPipeBindBit;` is ill-formed and every existing spelling already resolves. A comment records the
move instead. Package B's code is unchanged either way.

**D5 — six death helpers, one per KIND, and the texture helper does not release the built-in
sampler CSO.**
D-I1's table has six rows but five distinct helper names; the phase mints six *kinds*
(`PipeSlotKind` grows six, G8 has six ABA arms), so there are six helpers, one per kind, all
taking `Uint64 lifetimeId`. The texture helper emits `ResourceDestroy` and then calls
`MGPipeEmitSamplerViewCsoDestroyAndFree(lifetimeId)` — legal because the view is minted off the
texture's own lifetime id (D-F2). It does **not** release the built-in sampler CSO from that same
id: `ITextureObject::m_sampler` is a real `SamplerObject` with its own lifetime id and its own
`#if MOBILEGL_PIPE_PUSH` destructor (`SamplerObject.cpp:30-40`), so freeing it from the texture's
id would resolve the wrong slot — possibly a live one belonging to another object. `~SamplerObject`
runs immediately after `~TextureObjectBase`'s body and takes
`MGPipeEmitSamplerCsoDestroyAndFree`, which is the same helper, the same three-step order, and
idempotent. The three wire calls D-I1's table attributes to a texture's death therefore all still
go out, from two destructors instead of one.

**D6 — `MGP_DIRTY_SURFACE_UNDECIDED_LIST` is NOT empty.**
D-K5 says it stays empty. With the widened prefix set it cannot: `--check` reports both new
bit-answer rows as UNDECIDED, and prints exactly why —
`UseProgram` reaches `DestroyProgramSlot()`, which writes `attachedShaders`, and `BindVertexArray`
reaches a call spelled `Bind(`, which resolves by name to `ImageTextureBinding::Bind` and writes
`Access`; neither is an `m_`-prefixed member, so the write analysis taints both bodies and refuses
to decide. The tool itself offers two options ("widen the analysis or list the row … with this
reason"). Widening the taint rule would weaken the one mechanism that catches a genuine under-fire
and has its own negative control (#9), so the two rows are **marked**, with the tool's own reason
written into the file. Control 18 fails if a mark outlives its reason; new control 21 fails if a
mark stops being needed. Both bit answers themselves are unchanged and correct.

**D7 — `X(BindProgramPipelineObject, kPulledEveryVerb)`, not `NEW_SHADER`.**
D-K5 lists it as `NEW_SHADER`. The derivation refutes that outright (not merely "undecided"): the
mutator writes `m_boundProgramPipeline`, `m_everBound`, `m_programPipelineNames`,
`m_programPipelines`, and bit 6's shutter reads `m_currentProgram` / `m_lifetimeId` /
`m_linkVersion` — disjoint on every path. Nor is that a shutter defect: bit 6 reads
`GetCurrentProgram()` and deliberately not `GetProgramForDraw()`, because the tracker must not
force a compile, and flattening a pipeline into its composite is that compile. What a bind moves
is `GetProgramForDraw`, which is pulled at every verb of every class that reads it, so
`kPulledEveryVerb` is the answer that holds on every path. Recorded verbatim in the row's comment.

**D8 — six `Coverage.def` emitted rows, and four deliberate absences.**
Added: `GetFramebufferBindingSlot → SetFramebufferState`, `GetImageTextureBinding →
SetShaderImages`, `GetTextureUnitObject → SetSamplerViews`, `GetMaxTouchedTextureUnit →
SetSamplerViews`, `GetProgramForDraw → SetDrawProgram`, `GetProgramForDispatch →
SetDispatchProgram`. Deliberately absent, for `GetPixelStoreParameters`' reason (a row is a claim
that the field is supplied, and for these it would be a half-truth): `GetActiveTextureUnit` (no
call carries the active-unit selector), `GetTextureContextId` (a backend's own keying question),
and `GetTextureBindGeneration` / `GetSamplingResolutionGeneration` (frontend *shutters*; what
replaces them server-side is the applier's `Serial`, a different value with a different owner).
`GetTextureObject` and `GetProgramObject` are **sticky** — forwarded, keyed by GL name, no storage
for an emitted call to supply.

**D9 — seven validate-point emitters, not five.**
`EmitFramebufferState`, `EmitShaderState`, `DrainTextureSubData`, `EmitSamplerViews`,
`EmitSamplerStates`, `EmitShaderImages`, `EmitGlobalConstants`. Seven dirty bits plus the texture
sub-data drain need call sites, and adding one after the tag would mean A touching `PipeFill.cpp`
again and telling five packages. The drain is the only one not gated by a dirty bit: its family's
calls dispatch at the GL call that causes them, so it is gated on the subsystem bit alone (and by
`kMGPipeWiredSubsystems`), and an empty drain list costs one test. The P4a block stands **before**
the P2/P3a block, which is ARCHITECTURE.md 5.4's recommended order; that document says in as many
words that the order is code organisation and not a contract, and every P4a emitter is a stub at
`c0`, so the move is inert here.

**D10 — the `0x1ff → 0x1fff` default is `ConfigLoader.cpp` only.**
C.0 and D-K1 say "the push default in `CMakeLists.txt` / `ConfigLoader.cpp:257`". The root
`CMakeLists.txt` carries no numeric push default — only `option(MOBILEGL_PIPE_PUSH … OFF)` — so
there is nothing there to move. The itest phase-mask pins (`0x1ff` → `0x1fff`, and
`0x80000000000001ff` → `0x8000000000001fff`) live in `MobileGL/MG_IntegrationTest/CMakeLists.txt`
at `:949, 1065, 1070, 1075, 1080, 1149, 1183, 1188, 1219`, which C.7 gives to **package F**; they
are untouched here and F owns them.

**D11 — `MG_Test/Pipe/TrackerTest.cpp` is edited.**
C.7 gives it to nobody. `TrackerWalk.OnlyTheFiveEmittedBitsNameASubsystem` compares
`MGPipeSubsystemForDirty` against `kMGPipeDirtyEmittedAtP3a` and goes red the instant `Tracker.h`'s
map grows — which is the gate working. `Tracker.h` is A's for the whole phase, so the case that
pins it is A's too. The test **name is unchanged** (G14). It now compares against
`kMGPipeDirtyEmittedAtP4a`, pins the three phase constants as a chain, pins each of the seven new
arms, and pins that no bit names `kMGPipeSubsystemTextureResources`.

**D12 — `PipeCatalogue.SixValueStructsHaveFieldLists` count 71 → 72.**
Inside A's own file, listed because the case name is now doubly stale and stays: it gained the
`SamplerParameters` verifier plus the padding control that writes garbage into the three trailing
bytes through a byte pointer.

**D13 — the ShaderCso composite band gets its own dense table, in the allocator AND the applier.**
`kMGPipeShaderCsoCompositeSlotBase` is 983040. A single composite in a slot-indexed vector
allocates ~983k `SlotState`s (~23 MB) in the allocator, and ~983k `MGPipeShaderCsoRecord`s
(~236 MB) in the applier — a 236 MB spike on the first program-pipeline draw, which would have
been a defect this commit introduced. Both sides therefore keep the band in a second table indexed
by `slot − base`: `KindState::{BandSlots, BandFreeList}` with a shared `EntryOf(kind, slot)`
resolver, and `MGPipeApplierState::CompositeShaderCsos` beside `ShaderCsos`. Both spaces stay dense
against their own high-water mark, which is the property the allocator exists to give the server.
`HighWater(MGPipeKind::ShaderCso)` still reports the band's top once a composite has been minted,
so a **leaked composite slot moves it** — otherwise G8b's dedicated composite leak case would be
green for ever and mean nothing. `kMGPipeMaxShaderCsoSlots` is still `kMGPipeShaderCsoSlotLimit`,
because a composite slot is a legal slot below that bound. **The server never learns a handle is a
composite**: the split is an indexing detail on the client side of the wire.

**D14 — the "visited field count" guard is a test assertion, not a `static_assert`.**
D-H2 asks for a `static_assert` on the visited field count. `VisitFields` needs an *instance*, and
`LinkArtifacts` carries strings, vectors, maps and a `std::set`, so no constant expression can walk
it. `ProgramArtifactsCodec.h` exposes `ProgramArtifactsVisitedFieldCount<T>()` and
`ProgramArtifactsCodec.TheTablesVisitEveryMemberExceptTheLiveProgram` pins 57 / 8 / 14 / 11 / 20.
`link.program` is additionally forced null on every successful decode.

**D15 — the codec has one hand-written arm.**
`glslang::TIntermediate::TUniformInitializer` (`LinkArtifacts::uniformInitialValues`) has no
`VisitFields` table in `ProgramArtifacts.h`, so the codec cannot reach it through the tables. It is
a plain aggregate (`std::string`, a `TBasicType`, four `int`s, `vector<long long>`,
`vector<double>`) — which is exactly what `ProgramTranslationCache.h`'s audit concluded — and the
codec carries an explicit arm for it, flagged in the source so that a field added there moves the
arm and the format version together. Everything else goes through the tables; a type with no table
is a `static_assert` failure, not a `memcpy`.

**D16 — `check_include_closure.py --compiler clang++-20` does not exist on this box.**
The binary is `clang++` (`/usr/sbin/clang++`); there is no `clang++-20`, and the script prints
`::error::compiler not found` and then still exits 0, which is a false green worth knowing about.
Run with `--compiler clang++`: 4 probes, 0 skipped, 0 problems, 6 controls tripped. A tooling
note for the integrator's `wsl_p4a_gate.sh`, not a tree defect.

---

## 4. The contract itself — what B, C, D, E and F compile against

### 4.1 `MG_Pipe/MGPipeTypes.h`

```cpp
enum class MGPipeResourceTarget : Uint8 {
    Buffer = 0, Tex1D, Tex2D, Tex3D, Tex1DArray, Tex2DArray, TexCube, TexCubeArray,
    Tex2DMS, Tex2DMSArray, Renderbuffer, TexBuffer, TexRect, Count,
};
inline constexpr Uint8  kMGPipeResourceTargetBuffer = 0;      // moved from ResourceTracker.h
inline constexpr Uint32 kMGPipeResourceTargetUnmapped = 0x100u;
constexpr Uint32 MGPipeResourceTargetForTextureTarget(MobileGL::TextureTarget target); // no default: arm
constexpr Bool   MGPipeEveryTextureTargetIsMapped();

enum MGPipeBindBit : Uint16 {   // moved verbatim from ResourceTracker.h
    kMGPipeBindNone = 0,        kMGPipeBindVertex = 1u << 0,   kMGPipeBindIndex = 1u << 1,
    kMGPipeBindConstant = 1u << 2, kMGPipeBindShaderBuffer = 1u << 3, kMGPipeBindIndirect = 1u << 4,
    kMGPipeBindSampler = 1u << 5,  kMGPipeBindShaderImage = 1u << 6, kMGPipeBindRenderTarget = 1u << 7,
    kMGPipeBindDepthStencil = 1u << 8, kMGPipeBindStreamOutput = 1u << 9, kMGPipeBindAtomic = 1u << 10,
    kMGPipeBindElementArray = 1u << 11,
};

enum class MGPipeFramebufferTarget : Uint8 { Draw = 0, Read = 1, Both = 2, Count };
inline constexpr Uint32 kMGPipeMaxColorAttachments = 8;   // == MobileGL::kMGMaxDrawBuffers
inline constexpr Uint32 kMGPipeMaxTextureUnits = 192;     // == TextureState::MAX_TEXTURE_IMAGE_UNITS
inline constexpr Uint32 kMGPipeMaxImageUnits   = 192;

struct MGPTextureParams {          // 40 bytes (was 32)
    MGPipeHandle Res;              //  0
    MGPipeHandle BuiltinSampler;   //  8   kind SamplerCso; kMGPipeNullHandle is ILLEGAL
    Uint16 BaseLevel, MaxLevel;    // 16
    Uint8 Swizzle[4];              // 20
    Uint8 DepthStencilMode;        // 24
    Uint8 ForceResync;             // 25
    Uint8 SamplerResync;           // 26   NEW
    Uint8 Pad0;                    // 27
    Float MinLod, MaxLod, LodBias; // 28
};

// MGPFramebufferState: ...Uint8 FixedSampleLocations, IsDefault, Complete; Uint8 Target; Uint32 Pad1;
//   Target is MGPipeFramebufferTarget. Size unchanged at 304. Complete is
//   FramebufferObject::CheckCompleteness(), NEVER glCheckFramebufferStatus's answer.

inline Bool MGPipeResourceRespecifyNeedsAck(const MGPResourceDesc& desc) {
    return desc.Immutable != 0 && desc.Target == kMGPipeResourceTargetBuffer;
}
```

### 4.2 `MG_Pipe/MGPipe.h`

```cpp
inline constexpr Uint64 kMGPipeSubsystemFramebuffer      = 1ull << 9;
inline constexpr Uint64 kMGPipeSubsystemTextureResources = 1ull << 10;
inline constexpr Uint64 kMGPipeSubsystemSamplers         = 1ull << 11;
inline constexpr Uint64 kMGPipeSubsystemPrograms         = 1ull << 12;
inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP4a   = 0x1fffull;  // 0x7f and 0x1ff untouched
```
Dependencies (diagnosed at first use, never half-run, backend-side): **11 requires 10**,
**9 requires 10**, **10 requires 7**; bit 12 depends on nothing. The mirror pairs are all fine.
Push default is now `kMGPipeSubsystemsMigratedAtP4a` (`ConfigLoader.cpp:257`).

### 4.3 `MG_Pipe/PipeApply.h` — bounds, records, state

```cpp
inline constexpr Uint32 kMGPipeMaxSamplerCsoSlots  = 1u << 16;
inline constexpr Uint32 kMGPipeMaxSamplerViewSlots = 1u << 20;
inline constexpr Uint32 kMGPipeMaxShaderCsoSlots   = kMGPipeShaderCsoSlotLimit; // 1<<20

struct MGPipeSamplerCsoRecord  { Uint32 Gen = 0; Bool Live = false; SamplerParameters Params{}; Uint64 Serial = 0; };
struct MGPipeSamplerViewRecord { Uint32 Gen = 0; Bool Live = false; MGPSamplerView View{};      Uint64 Serial = 0; };
struct MGPipeShaderCsoRecord {
    Uint32 Gen = 0; Bool Live = false;
    MGPProgramDesc Desc{};
    Uint32 GlobalConstantsVersion = ~Uint32{0};   // the "never uploaded" sentinel
    Vector<Uint8> GlobalConstants;
    Uint64 GlobalConstantsSerial = 0;
    Uint64 Serial = 0;
};

// MGPipeResourceRecord gains:
MGPTextureParams Params{}; Uint64 ParamsSerial = 0; MGPipeHandle ViewCso = kMGPipeNullHandle;
struct PendingUpload { Uint16 UploadTarget = 0; Uint16 Level = 0; MGPBox UnionBox{}; Vector<MGPSubRegion> Regions; };
Vector<PendingUpload> PendingUploads;

// MGPipeApplierState gains — OBJECT RECORDS (survive a make-current):
Vector<MGPipeResourceRecord>  TextureResources, RenderbufferResources;
Vector<MGPipeSamplerCsoRecord>  SamplerCsos;
Vector<MGPipeSamplerViewRecord> SamplerViewCsos;
Vector<MGPipeShaderCsoRecord>   ShaderCsos;
Vector<MGPipeShaderCsoRecord>   CompositeShaderCsos;   // indexed by slot - kMGPipeShaderCsoCompositeSlotBase
Uint64 RefusedObjectCalls = 0;
// WORKING STATE (cleared by MGPipeApplierReset; serials ADVANCE, never zero):
MGPFramebufferState DrawFramebuffer{}, ReadFramebuffer{}; Uint64 FramebufferSerial = 0;
Array<MGPBoundView,  kMGPipeMaxTextureUnits> BoundSamplerViews{};  Uint32 SamplerViewStart, SamplerViewCount;   Uint64 SamplerViewsSerial;
Array<MGPipeHandle,  kMGPipeMaxTextureUnits> BoundSamplerStates{}; Uint32 SamplerStateStart, SamplerStateCount; Uint64 SamplerStatesSerial;
Array<MGPImageView,  kMGPipeMaxImageUnits>   BoundShaderImages{};  Uint32 ShaderImageStart,  ShaderImageCount;  Uint64 ShaderImagesSerial;
MGPipeHandle DrawProgram, DispatchProgram, BoundShaderCso;         Uint64 ProgramBindingSerial = 0;
```

### 4.4 `MG_Pipe/PipeApply.h` — the fifteen entry points (**stub bodies at `c0`**)

```cpp
void  MGPipeApplySetFramebufferState(const MGPFramebufferState& state);
void  MGPipeApplyCreateSamplerState(const MGPSamplerDesc& desc, const SamplerParameters* parameters);
void  MGPipeApplyDeleteSamplerState(const MGPHandleOnly& handle);
void  MGPipeApplyCreateSamplerView(const MGPSamplerView& view);
void  MGPipeApplyDeleteSamplerView(const MGPHandleOnly& handle);
void  MGPipeApplySetTextureParams(const MGPTextureParams& params);
void  MGPipeApplySetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail);
void  MGPipeApplyBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail);
void  MGPipeApplySetShaderImages(const MGPShaderImages& hdr, const MGPImageView* tail);
void  MGPipeApplyCreateShaderState(const MGPProgramDesc& desc,
                                   const MG_State::GLState::LinkArtifacts* link,
                                   const MG_State::GLState::SpirvArtifacts* spirv);
void  MGPipeApplyBindShaderState(const MGPHandleOnly& handle);
void  MGPipeApplyDeleteShaderState(const MGPHandleOnly& handle);
void  MGPipeApplySetDrawProgram(const MGPHandleOnly& handle);
void  MGPipeApplySetDispatchProgram(const MGPHandleOnly& handle);
void  MGPipeApplySetGlobalConstants(const MGPGlobalConstants& record, const void* bytes);

void  MGPipeUnmigratedEmulation(const char* name);   // monolith body: (void)name;
```
`LinkArtifacts` / `SpirvArtifacts` are **forward-declared** in `PipeApply.h`; the header never
reaches a frontend one. `MGPipeResourceOps` is unchanged (nine members).

### 4.5 `MG_Pipe/PipeMutation.h` — the six death helpers

```cpp
Bool MGPipeEmitTextureDestroyAndFree(Uint64 lifetimeId);        // ResourceDestroy, then the view helper
Bool MGPipeEmitRenderbufferDestroyAndFree(Uint64 lifetimeId);   // ResourceDestroy
Bool MGPipeEmitFramebufferDestroyAndFree(Uint64 lifetimeId);    // no wire call at all (D-I2)
Bool MGPipeEmitSamplerCsoDestroyAndFree(Uint64 lifetimeId);     // DeleteSamplerState
Bool MGPipeEmitSamplerViewCsoDestroyAndFree(Uint64 lifetimeId); // DeleteSamplerView
Bool MGPipeEmitShaderCsoDestroyAndFree(Uint64 lifetimeId);      // DeleteShaderState (composites too)
```
Order inside each, fixed: **wire delete → `NotifyStateObjectDestroyed(kind, lifetimeId)` → slot
free**. Each returns "did the delete go out". At `c0` all six answer `false` (the emitters publish
nothing yet), so the legacy path still runs and the tree is behaviourally unchanged. B and C
replace the bare `NotifyStateObjectDestroyed(...)` in the five existing `#if MOBILEGL_PIPE_PUSH`
destructor bodies with the matching helper call — **edit, never add a destructor** (G1).

### 4.6 `MG_Impl/Pipe/*`

```cpp
// SlotAllocator.h
MGPipeHandle MGPipeSlotAllocator::AllocateComposite(Uint64 lifetimeId);  // the ONE door into the band
// SetHashSuppressor.h — the enum is now:
//   SetVertexBuffers, SetSamplerViews, BindSamplerStates, SetShaderImages,
//   SetShaderBuffers, SetStreamOutputTargets, SetVertexAttribDefaults, SetFramebufferState, Count
// Tracker.h
inline constexpr Uint32 kMGPipeDirtyEmittedAtP4a = kMGPipeDirtyEmittedAtP3a
    | NewShader | NewShaderBindings | NewGlobalConstants
    | NewFramebuffer | NewSamplerViews | NewSamplers | NewShaderImages;   // (bit macros elided)
// bit 11's shutter is now
//   Mix(Mix(anyFramebufferAttachmentGeneration, drawBindVersion), readBindVersion)

// the five emit headers, each header-only and inside #if MOBILEGL_PIPE_PUSH:
inline constexpr Uint64 kMGPipeWiredFramebufferSubsystem = 0;   // FramebufferEmit.h
inline constexpr Uint64 kMGPipeWiredTextureSubsystem     = 0;   // TextureEmit.h
inline constexpr Uint64 kMGPipeWiredSamplerSubsystem     = 0;   // SamplerEmit.h (covers ImageEmit.h)
inline constexpr Uint64 kMGPipeWiredProgramSubsystem     = 0;   // ProgramEmit.h

class MGPipeFramebufferEmitter { Uint64 EmitFramebufferState(GLContext&); void Reset(); };
class MGPipeTextureEmitter     { Uint64 DrainTextureSubData(GLContext&);  void Reset(); };
class MGPipeSamplerEmitter     { Uint64 EmitSamplerViews(GLContext&); Uint64 EmitSamplerStates(GLContext&); void Reset(); };
class MGPipeImageEmitter       { Uint64 EmitShaderImages(GLContext&);     void Reset(); };
class MGPipeProgramEmitter     { Uint64 EmitShaderState(GLContext&); Uint64 EmitGlobalConstants(GLContext&); void Reset(); };
MGPipe{Framebuffer,Texture,Sampler,Image,Program}EmitterInstance();  // heap-constructed, leaked at exit
```
**B and C own these five headers from the tag on.** Each returns the bytes that went on the wire
(for the per-draw payload histogram) and each family flips its own constant from `0` to its
subsystem bit in the commit that gives its emitters bodies. `PipeFill.cpp` needs no edit for any
of that, and four `static_assert`s refuse a constant that is neither `0` nor its own bit.

### 4.7 `MG_Pipe/Coverage.def`, `PipeFill.cpp`
Six new `MGP_COVERAGE_EMITTED_LIST` rows (§3 D8) → new `MGPipeFieldEmitter` enumerators
`SetFramebufferState`, `SetShaderImages`, `SetSamplerViews`, `SetDrawProgram`,
`SetDispatchProgram`. All six rows are **shape-only**: they land in
`EmittedCallSuppliesTheWholeField`'s **false** arm, so nothing stops being pulled.

### 4.8 `MG_Util/Metrics/PipeStats`
`CallClass` (push-only): `FramebufferEmissions` `framebuffer-emissions` `fbe`,
`SamplerViewEmissions` `sampler-view-emissions` `sve`, `SamplerStateEmissions`
`sampler-state-emissions` `sse`, `ShaderImageEmissions` `shader-image-emissions` `sie`,
`ClientTextureUploadEmissions` `client-tex-upload-emissions` `ctu`.
`ByteClass` (new push-only block): `CsoBlobBytes` `cso-blob-bytes`, short **`csob-blob`** — not
`csob`, which the `cso[...]` bracket already uses for the CSO bind count.
The summary line gains `emit[fbe= sve= sse= sie= ctu=]`.

### 4.9 `ProgramArtifactsCodec`
```cpp
inline constexpr Uint32 kProgramArtifactsCodecVersion = 1;
void EncodeProgramArtifacts(const LinkArtifacts&, const SpirvArtifacts&, Vector<Uint8>& out);
Bool DecodeProgramArtifacts(const Uint8* bytes, SizeT size, LinkArtifacts&, SpirvArtifacts&);
template <class T> SizeT ProgramArtifactsVisitedFieldCount();
```
Header: version word, then a `MGL_LINKARTIFACTS_SIZE` echo (0 where the toolchain is not pinned).
Everything length-prefixed; every count checked against the bytes that remain **before** any
allocation; little-endian asserted. Refuses a truncated stream, a version mismatch, a size-echo
mismatch, a null pointer and **trailing bytes**, leaving both outputs default. `link.program` is
never visited and is null on return.

---

## 5. Verification transcript (on `08192d72`, working tree clean)

```
$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0
| before | ~/w7/p4a-before-libMobileGL.so | 19114360 | 10806323 |
| after  | build-linux/libMobileGL.so     | 19114360 | 10806323 |
### Removed (0) _none_    ### Added (0) _none_
### Resized (0) _none_    ### Renamed only (same size) (0) _none_

$ python3 scripts/gen_pipe.py --check
gen_pipe: 71 calls (11 screen, 60 context), 72 verify payloads, 63 PipeInputs fields
          (7 sticky, 40 emitted by a P2 call), 69 verbs, 9 classes
gen_pipe: inventory 477 rows: ... 0 UNMAPPED
gen_pipe: generated files are up to date                                    rc=0
$ python3 scripts/gen_pipe.py --self-test
gen_pipe: self-test: 7 negative-control trip(s), positive control OK        rc=0
$ git diff --exit-code -- MobileGL/MG_Pipe/generated                        rc=0

$ python3 scripts/gen_pipe_dirty_surface.py --check
dirty-surface: 79 mutators, all mapped, no stale rows; 45 render-state answers derived
dirty-surface: 10 other (mutator, bit) answers derived ...; 0 COARSE; 2 UNDECIDED
               (listed in MGP_DIRTY_SURFACE_UNDECIDED_LIST); 26 prose answers        rc=0
$ python3 scripts/gen_pipe_dirty_surface.py --self-test
dirty-surface self-test: 27 negative controls, all tripped; positive controls OK      rc=0

$ python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all
include-closure: self-test: 6 negative-control trip(s), parser checks OK
include-closure: 4 probes, 0 skipped, 0 problem(s)                                    rc=0

$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
[p3a-untouched] the 11 ... functions are byte-identical between 37da3c3a and HEAD      rc=0

$ cmake --build build-linux  -j 16 && ctest --test-dir build-linux  -L unit -j 12   100% 1635/1635
$ cmake --build build-push   -j 16 && ctest --test-dir build-push   -L unit -j 12   100% 1635/1635
$ cmake --build build-verify -j 16 && ctest --test-dir build-verify -L unit -j 12   100% 1635/1635

$ diff <(ctest --test-dir build-linux -N | ID-4 extractor) \
       <(ctest --test-dir build-push  -N | ID-4 extractor)                          (empty)
$ comm -23 ~/w7/p4a-before-ctest-names.txt <(build-linux names)                     (empty)
$ comm -13 ~/w7/p4a-before-ctest-names.txt <(build-linux names)                     13 added:
  CompositeResolver.TheCompositeBandHasExactlyOneDoor
  {Framebuffer,Image,Program,Sampler,Texture}Emit.TheEmitterIsOneNeverDestroyedProcessSingleton
  PipeCatalogue.EveryTextureTargetMapsToItsOwnResourceTarget
  PipeCatalogue.EveryUnmigratedEmulationIsNamedOnce
  PipeCatalogue.TextureParamsNameTheirBuiltinSamplerAndFramebufferStateNamesItsTarget
  ProgramArtifactsCodec.{RoundTripsAFullyPopulatedArchive, ATruncatedStreamIsRefusedNotGuessed,
                         AVersionMismatchIsRefused, TheTablesVisitEveryMemberExceptTheLiveProgram}

$ grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'
MobileGL/MG_Backend/MGPipe/PipeInputs.h:1     # pre-existing at 37da3c3a, unchanged
$ awk '/struct MGPipeResourceOps \{/,/^    \};/' MobileGL/MG_Pipe/PipeApply.h | grep -c MG_State
0
```

Two failures were found and fixed during the round, both of them gates doing their job:
`PipeCatalogue.SixValueStructsHaveFieldLists` (verified-payload count 71 → 72) and
`TrackerWalk.OnlyTheFiveEmittedBitsNameASubsystem` (the phase constant, §3 D11).

---

## 6. Left undone, and who owns it

- **Every apply body.** All fifteen entry points are stubs; `w1`/`w2`/`w3` fill them, on this
  branch, **before** any client package runs against them.
- **Every emitter body**, and the four `kMGPipeWired*Subsystem` constants (all `0`). B and C, in
  their own headers.
- **`CompositeResolver.h`** — not created. Wholly C's.
- **`Resolve<Family>SubsystemArm()`** and D-C3's `MaxColorAttachments > 8` refusal — backend-side
  (`Managers.cpp`, beside `ResolveResourceSubsystemArm`), package D.
- **The five `MGPipeUnmigratedEmulation` call sites** in `MG_Backend/DirectGLES/` — D and E. The
  five names are pinned here (`PipeCatalogue.EveryUnmigratedEmulationIsNamedOnce`):
  `copy-image-shadow-mirror`, `generate-mipmap-storage`, `generate-mipmap-cpu-fallback`,
  `get-tex-image-shadow`, `texture-remint-pull`.
- **`PipeSlotPeek`'s six new members**, `p4a_untouched_regions.sh`,
  `p4a_descriptor_negative_control.sh`, the itest phase-mask moves (`0x1ff → 0x1fff`,
  `0x80000000000001ff → 0x8000000000001fff`) and `MagmaPipeArms.h` — package F.
- **The libc++/NDK size pin** (`ProgramArtifacts.h:563-565`) — the integrator's, from an NDK
  build's ctest properties. The codec's size echo is `0` under that branch until it lands, which
  round-trips correctly within one build and starts biting the moment the pin exists.
- **`ROADMAP.md:100`'s citation of `:7`**, `ARCHITECTURE.md:134`'s texture-params row,
  `ARCHITECTURE.md:63`'s sampler-view annotation and `SetHashSuppressor`'s doc corrections in
  `docs/` — the integrator's (D.5). The in-tree comments are already correct.
- **`clang++-20`** does not exist in this WSL image; `wsl_p4a_gate.sh` should spell `clang++`
  (§3 D16), and note that the script exits 0 when the compiler is missing.
