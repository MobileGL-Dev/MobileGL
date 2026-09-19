# Scout: the strangler scaffold that exists (P0/P0.5) and what P1 must add

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated` @ `5635e33f` ("[Docs] (Disaggregated): record the P0.5 landing"). Working tree clean except untracked `.trace-work/`.

Every claim below cites a file and a line I opened. Paths are repo-relative to the worktree root.

---

## 0. One-paragraph orientation

`MG_Pipe` is a leaf: it is a set of headers plus a `generated/` directory of seven `.inc` files, and **nothing in the shipped library compiles any of it today**. `grep` for `MG_Pipe/` outside `MobileGL/MG_Pipe/` returns exactly five hits: four `MG_State` headers include `<MG_Pipe/MGPipeValueTypes.h>` (`MG_State/GLState/FramebufferState/FramebufferObject.h:14`, `MG_State/GLState/RenderState/RenderState.h:11`, `MG_State/GLState/SamplerState/SamplerObject.h:11`, `MG_State/GLState/VertexArrayState/VertexArrayObject.h:13`) and one test includes `<MG_Pipe/MGPipe.h>` (`MG_Test/Pipe/PipeCatalogueTest.cpp:18`). `MobileGL/MG_Pipe` appears in `CMakeLists.txt` only as an include directory (`CMakeLists.txt:535`), never in `SOURCE_FILES` (which starts at `CMakeLists.txt:234` and is an explicit list, not a glob). **P1 is therefore the first change that makes the generated headers part of the library build**, and the first that gives `MG_Pipe`/`MG_Backend/MGPipe` a `.cpp`.

---

## 1. `scripts/gen_pipe.py` — the seven generators, read end to end (676 lines)

### 1.1 Inputs, outputs and paths

| what | where set |
|---|---|
| `REPO_ROOT` = parent of `scripts/` | `scripts/gen_pipe.py:37` |
| `PIPE_DIR` = `MobileGL/MG_Pipe` | `:38` |
| `GENERATED_DIR` = `MobileGL/MG_Pipe/generated` | `:39` (created if missing, `:641-642`) |
| `INVENTORY` = `scripts/data/backend_read_inventory.md` | `:40`; missing → `sys.exit` telling you to copy it from MobileGL-CS (`:219-220`) |

Sources of truth read: `PipeCalls.def`, `PipeFields.def`, `Coverage.def` (all under `MobileGL/MG_Pipe/`), plus the vendored inventory markdown. Docstring at `:9-30`.

Banner emitted at the top of every output: `:42-55`. It hard-codes the sentence **"This file is included from MG_Pipe/MGPipe.h inside namespace MobileGL::MG_Pipe."** — any new generated file P1 adds inherits that claim, so it must in fact be included from inside that namespace or the banner text has to be parameterised.

### 1.2 Parsers and the validation they perform in BOTH modes

- `parse_calls()` `:131-160`. Finds `#define MGP_CALL_LIST_DOCUMENTED_COUNT (\d+)` (`:133-135`), then scans from the line `#define MGP_CALL_LIST(X)` and stops at **the first line that does not end in a backslash** (`:149-150`). Row regex `CALL_RE` at `:128` = `X(Name, Payload, Class, Flags)` with `Flags` allowed to contain `|`. Index is 1-based position = wire opcode (`:146`). Exits if the parsed count ≠ the documented count (`:151-154`) or on a duplicate call name (`:155-159`).
- `parse_verify_payloads()` `:175-184`. Regex `#define MGP_VERIFY_PAYLOAD_LIST\(P\)(.*?)\n\n` — **terminated by a blank line**, so the list macro in `PipeFields.def` must be followed by an empty line. Exits if a listed payload has no `#define MGP_FIELDS_<name>(F)` (`:181-183`).
- `check_call_payloads_have_field_lists()` `:187-195`. Every payload named in `PipeCalls.def` must either have a field list or be in `MEMCMP_FALLBACK_TYPES` (`:167-172` = `RenderStateParameters`, `PixelStoreParameters`, `DynamicBackendParameters`, `MGHostSpan`). The comment at `:165-166` says explicitly: **"They … get field lists of their own in P1"**.
- `parse_coverage()` `:198-212`. Two macros, both blank-line-terminated: `MGP_COVERAGE_ACCESSOR_LIST(X)` → `X(Accessor, Call)` pairs (`:201-205`) and `MGP_COVERAGE_DELTA_LIST(X)` → `X(free text delta kind, Call)` (`:207-211`).
- `parse_inventory()` `:218-237`. Reads `### \`<file>\`` headings and table rows `| line | kind | member | delta |` (`INVENTORY_ROW_RE` at `:215`).

### 1.3 What each generator emits

| gen | function | output | content |
|---|---|---|---|
| G1 | `gen_tables` `:244-270` | `PipeTables.inc` (108 lines) | `struct MGPipeScreen` / `struct MGPipeContext` of function pointers, split by `Class == kScreen`; `kMGPipeScreenCallCount=11`, `kMGPipeContextCallCount=60`, `kMGPipeCallCount=71`; four `static_assert`s including "table size == count × sizeof(fnptr)" and "count == `MGP_CALL_LIST_DOCUMENTED_COUNT`". Signatures come from `Call.Signature` (`:113-125`): `const <Payload>* payload`, plus `const void* varTail, Uint32 varTailCount` if `kVarTail`, plus `MGPReplySlot* reply` if `kReplySlot`. |
| G2 | `gen_thunks` `:273-286` | `PipeThunks.inc` (302 lines) | one `inline void MGP_<Name>(...)` per call forwarding to `gMGPipeScreen.<Name>` / `gMGPipeContext.<Name>`. Example `PipeThunks.inc:20-22`. |
| G3 | `gen_wire` `:289-371` | `PipeWire.inc` (931 lines) | `MGPWireRecHeader` (8 bytes), `enum class MGPWireOp : Uint16` with `kInvalid=0` … `kOpCount=72`, one `struct alignas(8) MGPWireRec_<Name>` per call with a composition size assertion, `MGPipeWireProtocolFatal`, the `MGP_WIRE_CHECK_BOUNDS` macro and `MGPipeApplyWireRecord()` — a skeleton switch where **every arm validates bounds then `return false`** (`:350-357` comment). Oversize-record rule (chunking at `RingProducer::MaxRecordBytes() == Capacity()/2`) documented at `:305-311`. |
| G4 | `gen_verify` `:374-444` | `PipeVerify.inc` (573 lines) | `#include "../PipeFields.def"` (`:387`), a `MGPipeHasFieldVerifier<T>` trait, forward decls + specializations + one `MGPipeVerify(const P&, const P&, const char** outField)` per payload, the `MGPipeFieldEqual` dispatcher (`:399-418`) with the **memcmp fallback branch** and its "P1 gives them field lists … at which point this branch stops being reachable" comment (`:411-415`), the array overload (`:420-426`), the `MGP_VERIFY_FIELD` macro, and `kMGPipeVerifiedPayloadCount = 63` (tail of the file). Floats compare by bits (`:404-405`). |
| G5 | `gen_filled` `:447-510` | `PipeFilled.inc` (308 lines) | `enum class MGPipeInputField : Uint16` of the 61 Coverage.def accessors + `kFieldCount` (`PipeFilled.inc:28`), `kMGPipeInputFieldCount` + `static_assert(== 61)` (`:93-94`), `kMGPipeInputFieldNames[]` (`:96`), `kMGPipeInputFieldSticky[]` — **every entry `false`** with the note that each `true` must be argued for in P1 (`gen_pipe.py:478-484`, output at `PipeFilled.inc:163`), `kMGPipeInputFieldFilledBy[]` (`:229`, pseudo-calls marked with a trailing comment), `struct MGPipeFilledState { Uint64 CurrentVerbSerial; Uint64 FilledGen[61]; }` (`:293`), `MGPipeInputPoisonFatal()` (`:298`) and `MGPipeInputFieldIsFresh()` (`:304`). Header comment states plainly "P0 is the skeleton … **PipeInputs itself lands in P1**" (`gen_pipe.py:461-462`). |
| G6 | `gen_coverage` `:513-579` | `PipeCoverage.inc` (108 lines) | joins inventory rows against the accessor map (by `member`) then the delta map (by `delta`); pseudo-calls `{kClientResolved, kReverseChannel, kStructuralHandle}` at `:515`; exits if a Coverage.def call is neither a catalogue call nor a pseudo-call (`:518-520`). Emits `kMGPipeCoverage[]` (63 entries, sorted by key), `kMGPipeInventoryReadPoints=477`, `MappedToCall=299`, `ClientResolved=5`, `ReverseChannel=6`, `StructuralHandle=167`, `Unmapped=0` (`PipeCoverage.inc:96-102`) and the "every row lands in exactly one bucket" assertion (`:104-108`). |
| G7 | `gen_span_table` `:582-616` | `PipeSpanTable.inc` (67 lines) | `kMGPipePipelineStateMembers[]` — the 24 names hard-coded in **python** at `gen_pipe.py:67-92` (taken from `VulkanRenderer::ComputePipelineStateHash`), the count, and two `extern const MGPStateChunk kMGPipePipelineChunks[]/kMGPipeDynamicChunks[]` declarations whose definitions land in `MG_Pipe/MGPipeRenderStateSpans.cpp` **in P2** (`PipeSpanTable.inc:64-67`). Three deliberate absences are named as P2 questions (`gen_pipe.py:592-601`): `FramebufferSrgb`/`DepthClamp` (no storage at all), `ProvokingVertexModeSetting`, `FrontFaceModeSetting`/`ClipOrigin`/`ClipDepthMode`. |

### 1.4 `write()` and `--check`

`write(path, text, check, changed)` `:619-626`: reads the existing file, returns if byte-identical, otherwise appends the repo-relative path to `changed` and — **only when not in check mode** — writes with `encoding="utf-8", newline="\n"` (LF, deliberate; the repo is developed on Windows).

`main()` `:629-671`: parses everything first (so **all validation above runs in `--check` too** — docstring `:28-29` says so explicitly), makes `GENERATED_DIR`, then writes the seven files in the fixed order `Tables, Thunks, Wire, Verify, Filled, Coverage, SpanTable` (`:646-652`), prints two summary lines and one line per UNMAPPED row (`:654-662`), and in `--check` returns **1** with `gen_pipe: OUT OF DATE: <files>` on stderr when anything would change (`:664-667`).

Consequence for P1: `--check` coverage of a NEW output is automatic *if and only if* the new file is produced through `write(...)` inside `main()`. A generator that writes with `open()` directly, or that emits into a path outside `GENERATED_DIR`, is invisible to both `--check` and the CI `git diff` (which only diffs `MobileGL/MG_Pipe/generated`, see §6).

---

## 2. The generated outputs as they stand

All seven live in `MobileGL/MG_Pipe/generated/` and are **committed**. Line counts: `PipeCoverage.inc` 108, `PipeFilled.inc` 308, `PipeSpanTable.inc` 67, `PipeTables.inc` 108, `PipeThunks.inc` 302, `PipeVerify.inc` 573, `PipeWire.inc` 931 (2397 total).

They are consumed in exactly one place, in this order, all **inside `namespace MobileGL::MG_Pipe`**:

```
MG_Pipe/MGPipe.h:65   #include "PipeCalls.def"
                :68   #include "generated/PipeTables.inc"
                :72-73 inline MGPipeScreen gMGPipeScreen{}; inline MGPipeContext gMGPipeContext{};
                :77   #include "generated/PipeThunks.inc"
                :80   #include "generated/PipeWire.inc"
                :83   #include "generated/PipeVerify.inc"
                :86   #include "generated/PipeFilled.inc"
                :89   #include "generated/PipeCoverage.inc"
                :92   #include "generated/PipeSpanTable.inc"
```

`MGPipe.h:10` includes `<Includes.h>` and `:12-15` the four sibling headers. `PipeVerify.inc` additionally does `#include "../PipeFields.def"` from inside the namespace.

**What compiles them today:** only `MG_Test/Pipe/PipeCatalogueTest.cpp` (target `PipeCatalogueTest`, `MG_Test/Pipe/CMakeLists.txt:3-28`; include dirs include `${MGL_ROOT}/MobileGL/MG_Pipe` at `:11`; `/Zc:preprocessor` on MSVC at `:23-25`; registered `gtest_discover_tests(... LABELS unit)` at `:28`). Registered from `MG_Test/CMakeLists.txt:88-90`.

Notable shapes P1 will build on:

- `PipeTables.inc:17-29` — the screen table ends with `FenceWaitServer`; the context table ends with `QueryTimestamp` / `QueryCounter` (tail of the file). Table member order is **catalogue order within a class**, which is *not* opcode order for the three appended calls.
- `PipeWire.inc` gives `MGPWireOp::SetSwapInterval == 68`, `QueryTimestamp == 69`, `QueryCounter == 70`, `FenceWaitServer == 71`, `kOpCount == 72` (pinned by `PipeCatalogueTest.cpp:164-170`).
- `PipeVerify.inc` — the `MGPipeFieldEqual` memcmp fallback is **reachable today**: `MGP_FIELDS_ResidualValueBlock` (`PipeFields.def:145-146`) names `F(RenderState)` (type `RenderStateParameters`) and `F(Pack)` (`PixelStoreParameters`); `MGP_FIELDS_MGPCaps` (`:39-40`) names `F(Dynamic)` (`DynamicBackendParameters`); `MGP_FIELDS_MGPPixelPackState` (`:139-140`) names `F(Pack)`. None of those three types defines `operator==` — `grep -n "operator==" MobileGL/MG_Pipe/MGPipeValueTypes.h` returns nothing — so all four fields fall to `std::memcmp`, which is exactly the padding-false-positive case the harness must not have.

---

## 3. `MobileGL/MG_Pipe/*.h` and the two `.def` sources

- **`MGPipe.h`** (93 lines). `enum MGPipeCallClass : Uint8 { kScreen, kCtxCso, kCtxState, kCtxObject, kCtxVerb, kCtxQuery, kCallClassCount }` at `:29-37` (unscoped on purpose so `PipeCalls.def` can be read by both cpp and python, `:27-28`). `enum MGPipeCallFlags : Uint32` at `:39-55` (`kNone/kNeedsAck/kHasBlob/kVarTail/kHostSpan/kReplySlot/kOptional`). Forward declaration `struct MGPipeRenderStateSpans;` at `:61` (defined in P2).
- **`MGPipeTypes.h`** (33 KB, ~800 lines). `MGP_ASSERT_POD(T, Size)` at `:44-46`. Includes `<MG_Backend/BackendObject.h>` at `:33` — this is the reason purity gate A asserts `MGPipeValueTypes.h` and not this header (`:25-32`). `using MG_Backend::DynamicBackendParameters; using MobileGL::PixelStoreParameters; using MobileGL::RenderStateParameters;` at `:37-41`. `MGPCaps` `:126-146` (composition-based size assert at `:145`). `MGPProgramDesc` `:300-311` — `MGPBlobRef Spirv[6]` + `MGPBlobRef Reflection`, the reflection blob being "the whole LinkArtifacts + SpirvArtifacts archive" (`:297-299`). `ResidualValueBlock` `:516-527`; `#define MGL_RESIDUAL_BLOCK_SIZE 1248` at `:536` with the ratchet assertion `:537-539`. Buffer-range-in-the-box helpers `MGPipeSetSubDataBufferRange` `:593`, `MGPipeSubDataBufferOffset` `:602`, `MGPipeSubDataBufferSize` `:607`.
- **`MGPipeValueTypes.h`** (23 KB, 548 lines). `#pragma once` **and** an include guard `MOBILEGL_MG_PIPE_VALUE_TYPES_H` at `:10-11`, with the comment "belt and braces: this file is reachable both as `<MG_Pipe/...>` and `<...>` (CMakeLists.txt:531,535)". Purity contract stated at `:23-25`. Contents: the render-state/pixel-store/sampler enums and `PixelStoreParameters` (`:208`), `PerBufferBlendState` (`:219`), `StencilFaceState` (`:229`), `RenderStateParameters` (`:239`), `SamplerParameters` (`:447`), and `namespace MG_State::GLState { VertexAttribute (:476), VertexBufferBindingPoint (:517), VertexAttributeVersion (:525) }` — i.e. it re-opens `MG_State::GLState` from inside `MG_Pipe` so old spellings still compile.
- **`MGPipeHandles.h`** (98 lines). `enum class MGPipeKind : Uint8` `:24-40`; `struct MGPipeHandle {Uint32 Slot; Uint32 Gen;}` `:54-61` with size/align asserts `:63-65`; `kMGPipeNullHandle{0,0}` / `kMGPipeDefaultFramebuffer{0,1}` `:72-73`; `kMGPipeFirstAllocatableSlot=1` `:81`; composite ShaderCso band `:88-94`.
- **`MGPipeHostSpan.h`** (57 lines). `kMGHostSpanSegNone=0` `:119`, `kMGHostSpanSegFromServerIndexMirror=0xFFFFFFFF` `:124`, `struct MGHostSpan` (32 bytes) `:126-137`, `MGPipeSegmentResolver` + `gMGPipeSegmentResolver = nullptr` `:144-145`, `MGPipeHostBytes()` `:148-154`.
- **`MGPipeCallbacks.h`** (61 lines). `struct MGPipeCallbacks` with the ten reverse-channel function pointers `:182-206` (`OnGlError`, `OnGpuWritten`, `OnBufferWriteback`, `OnTextureWriteback`, `OnTexturePullRequest`, `OnMipLevelsGenerated`, `OnSurfaceChanged`, `OnCapsInvalidated`, `OnLog`, `OnXfbScatterReady`), `kMGPipeCallbackCount = 10` + size assert `:210-212`, `inline MGPipeCallbacks gMGPipeCallbacks{}` `:215`.
- **`PipeCalls.def`** (177 lines). `#define MGP_CALL_LIST_DOCUMENTED_COUNT 71` at `:69`; the list `:72-160`; per-class counts documented `:29-37`; explicit non-migrations `:163-177`.
- **`PipeFields.def`** (238 lines). 63 `MGP_FIELDS_*` macros `:21-219`; `MGP_VERIFY_PAYLOAD_LIST(P)` `:223-236`. Header note `:15-17`: "Adding a field to a payload without adding it here makes the comparator blind to it; **that gap closes in P1**, when the verify harness goes live and the comparator's coverage is itself asserted."
- **`Coverage.def`** (106 lines). `MGP_COVERAGE_ACCESSOR_LIST(X)` `:29-97` (61 `X(Accessor, Call)` rows, including the three pseudo-call rows `InvalidateCompileEnv → kClientResolved` `:95`, `ValidateProgramName → kClientResolved` `:96`, `RecordError → kReverseChannel` `:97`); `MGP_COVERAGE_DELTA_LIST(X)` `:102-104` (`handle-ify (wire handle) → kStructuralHandle`, `Buffer ops delta → ResourceRespecify`). Comment at `:36-39` flags `GetBufferBindingSlot` as polymorphic over `BufferTarget` and says its rows split into four calls "**once the inventory carries the target argument (P1)**".

---

## 4. `scripts/gen_pipe_dirty_surface.py` (202 lines) — report today, gate in P1

Scans `MobileGL/MG_Impl/GLImpl` (`SCAN_ROOT` `:34`) for functions that BOTH mutate through `pGLContext->` with one of the prefixes `Add|Set|Mark|Bump|Allocate|Truncate|Record|Notify|Begin|End` (`MUTATOR_PREFIXES` `:37-38`, `MUTATOR_RE` `:40`) AND reach the backend via `gBackendFunctionsTable.GL.*` or `pActiveBackendObject->` (`BACKEND_RE` `:41`) in the same braced body (`FUNCTION_RE` `:42-43`, `function_bodies` `:88-103`). Comments/strings are masked first (`mask_comments_and_strings` `:46-85`). Output is a human report or `--summary` counts (`:138-141`, `:180-198`); every mutator prints `UNMAPPED` (`:178`) and the closing line says "the aggregate-generation mapping file **lands in P1**, and this report is what it has to cover" (`:193-195`). Known limits are printed by the script itself (`:196-198`): lambda bodies attributed to the enclosing function, helper-published mutations read as deferred.

Docstring `:21-22`: "P1 adds the mapping file and CI regenerates it with `git diff --exit-code` and zero unmapped mutators, **the same shape as gen_pipe.py's G6**." There is no mapping file and no `--check` flag today; `argparse` accepts only `--summary`.

---

## 5. `MG_State/GLState/ProgramState/ProgramArtifacts.h` (576 lines) — the P0.5 archive

Types: `TypeFacts` `:31`, `ResourceReflection` `:63` with the four aliases `UniformReflection`/`BlockReflection`/`PipeInputReflection`/`PipeOutputReflection` `:88-91`, `XfbVarying` `:95`, `LinkArtifacts` `:160`, `SpirvArtifacts` `:359`. Purity contract in the header banner `:16-19` (no `ShaderObject.h`, no `SpvcSession.h`, no `ShaderTranspiler/`, no `Config.h`, no `MG_Backend/`, no `BufferState/`; probe name `artifacts-header`; the token `glslang::` may appear exactly twice). Generic archive via `VisitFields(Self&, V&&)` overloads — `LinkArtifacts` visits **57 fields** (closing comment at `:534`), `SpirvArtifacts` visits **8** (`:539-547`). Size trip wires `:550-575`: `MGL_RESOURCEREFLECTION_SIZE 128`, `MGL_XFBVARYING_SIZE 128`, `MGL_LINKARTIFACTS_SIZE 1056`, `MGL_SPIRVARTIFACTS_SIZE 88`, pinned per-STL (libstdc++ only today; the libc++ branch at `:558-560` is inert until the integrator pins it). This is what `MGPProgramDesc::Reflection` (`MGPipeTypes.h:310`) will carry; nothing serializes it yet.

---

## 6. Config knobs — exact semantics, defaults and every read site

Declared in `MobileGL/Config.h` under the comment banner at `:319`:

| field | line | default | env | parsed at |
|---|---|---|---|---|
| `Uint64 PipePush` | `Config.h:326` | `0` | `MOBILEGL_PIPE_PUSH` (decimal or `0x`) | `ConfigLoader.cpp:245` (`QueryEnvUint64`) |
| `Bool PipeVerify` | `:332` | `false` | `MOBILEGL_PIPE_VERIFY` | `ConfigLoader.cpp:246` (`QueryEnvFlag`) |
| `Bool PipeStats` | `:335` | `false` | `MOBILEGL_PIPE_STATS` | `ConfigLoader.cpp:247` |
| `Bool PipeLegacyMemos` | `:340` | `true` | `MOBILEGL_PIPE_LEGACY_MEMOS` | `ConfigLoader.cpp:251-252` — **tri-state**: `QueryEnvQuirkOverride(...) != QuirkOverride::ForceOff`, i.e. only an explicitly falsy value turns it off |
| `Uint32 PipeTexelRetainMb` | `:344` | `0` (0–4096) | `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | `ConfigLoader.cpp:253` |
| `Uint32 PipeIndexMirrorMb` | `:349` | `64` (0–4096) | `MOBILEGL_PIPE_INDEX_MIRROR_MB` | `ConfigLoader.cpp:254` |
| `Uint32 PipeStatsPeriod` | `:354` | `120` (1–1000000) | `MOBILEGL_PIPE_STATS_PERIOD` | `ConfigLoader.cpp:255` |
| `String PipeStatsFile` | `:358` | `""` (no dump) | `MOBILEGL_PIPE_STATS_FILE` | `ConfigLoader.cpp:256` (`QueryEnvVariable`) |

`ConfigLoader.cpp:242-244` notes that no allow-list edit is needed: every `MOBILEGL_`/`LIBGL_` prefixed variable is accepted by construction. The hex/decimal parser's rules (a leading `0` is not octal; a `-` anywhere is rejected) are documented at `ConfigLoader.cpp:165`.

**Read sites outside Config/ConfigLoader — measured, whole tree:**

- `PipeStats` → `MG_Util/Metrics/PipeStats.cpp:257` (`Init()` latches `g_pipeStatsEnabled`).
- `PipeStatsPeriod` → `PipeStats.cpp:258-260` (0 is coerced to `kDefaultSummaryFramePeriod`).
- `PipeStatsFile` → `PipeStats.cpp:235-265` (`WriteJsonDump()` + the Init log line).
- **`PipePush`, `PipeVerify`, `PipeLegacyMemos`, `PipeTexelRetainMb`, `PipeIndexMirrorMb`: read NOWHERE.** They are parsed and then unused — pure P0 placeholders. P1 is the first consumer of `PipeVerify` and `PipePush`.

There is **no CMake option and no compile-time macro** named `MOBILEGL_PIPE_VERIFY` (`grep` over `CMakeLists.txt` and all `*.txt/*.cmake` returns nothing; `option(` list is `CMakeLists.txt:5-24,108,219`). ARCHITECTURE.md:579 lists it as "计划（P1；构建期开关…）", so P1 must add the CMake option itself and decide how it interacts with the already-parsed runtime `Features.PipeVerify`.

Likewise **`MOBILEGL_DEBUG` does not exist anywhere in the tree** — the only hit is the plan snippet `docs/Disaggregated/ARCHITECTURE.md:330` (`#if MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED`). The repo's real debug gate is `#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG` (`MobileGL/Defines.h:105`, and `MOBILEGL_ASSERT` compiles to nothing otherwise at `:113-115`). **P1 must use that spelling, not `MOBILEGL_DEBUG`.** Consequence worth stating: because `MOBILEGL_ASSERT` is already a no-op in INFO builds, deleting the ~43 `MOBILEGL_ASSERT(...pGLContext...)` lines in `MG_Backend` cannot move `.text` in an INFO build.

`MOBILEGL_BUILD_DISAGGREGATED` (`CMakeLists.txt:23`) appends `-DMOBILEGL_BUILD_DISAGGREGATED=1` (`CMakeLists.txt:524-526`) and the `MG_Remote` sources (`:455-490`), and gates `MG_Test/Wire` (`MG_Test/CMakeLists.txt:103-105`).

---

## 7. `MG_Util/Metrics/PipeStats.{h,cpp}` — counters P1 must keep alive

Header (192 lines). Enum `ByteClass` `:46-77`: `StageBuffer, StageTexture, StageUboGlobal, StageUboNamed, StageVertexClient, StageIndexClient, StageIndirectCmd, PersistentMapPush, ResidualValueBlock(placeholder, 0 until P2 — :73-75), Count`. Enum `CallClass` `:81-102`: `Draws, AccessorCalls, TextureUploadEmissions, TextureUploadBoxEmissions, TextureUploadRectEmissions, TextureUploadJobs, Count`. Enum `Gate` `:107-122`: `EsprytRenderState, EsprytTextureSyncList, EsprytUnitBindingsEpoch, MagmaDrawFastPath, MagmaPipelineMemo, MagmaDynamicTail, Count`. `kPayloadHistogramBuckets = 24` `:129` — the histogram is a **placeholder**: "MGPipe emits no records yet, so nothing in the backends calls `RecordDrawPayloadBytes`. The bucketing and the reporting are implemented and unit-tested so that the first generator to emit records only has to add the one call" (`:124-128`). API: `Enabled()` `:141`, `Init()` `:145`, `Shutdown()` `:149`, `AddBytes/AddCalls/CountGate/RecordDrawPayloadBytes` `:151-154`, `OnPresent()` `:159`, introspection `:162-173`, `FormatWindowLine()` (pure) `:179`, `AdvanceSummaryWindow()` `:183`, `FormatJson()` `:185`, test hooks `:189-190`.

Implementation (488 lines). The **site inventory** — "this list is the contract" — is `PipeStats.cpp:15-100`; it names what is NOT wired (compressed-texture upload, readback direction, persistent-map pushes, Magma's absent indirect array) and explains that `accessor-calls` is a **lower bound**: static tallies at ~10 hot entry points, "NOT a wrapper around all 293 `pGLContext->` sites" (`:76-88`). Counter storage `:112-124` (relaxed atomics + plain window bases). `PayloadBucketOf` `:142-152`. Name tables `:168-180`. `Init()` `:255-267`, `SummaryFramePeriod()` `:269`, `RecordDrawPayloadBytes` `:303`, `OnPresent` `:305`, `NameOf` `:362-364`, `FormatWindowLine` `:366`, `AdvanceSummaryWindow` `:434`, `FormatJson` `:448`. Called from `MobileGL/Init.cpp:114` (`Init()`, right after config load) and `Init.cpp:50` (`Shutdown()`).

Live counting sites (all guarded by `if (MG_Util::PipeStats::Enabled())`): DirectGLES `DirectGLES.cpp:1416,1423,1428,1520,1553,1570,2039,2045,2969,3423,3446,4368,10593`; `Managers.cpp:783,896,970,1541,2573,2607,2705,4433`; `MultiDraw.cpp:176,218,432,548,754`; DirectVulkan `DirectVulkan.cpp:1337`; `UniformManager.cpp:2080,2297`; `VkBufferManager.cpp:237,374,391,465,492,532,605,660,743`; `VkTextureManager.cpp:3154`; `VulkanRenderer.cpp:5004,5013,5257,5924,5930,6408,6442,6451,6456`.

**P1 obligations here:** every site above sits in a function whose `pGLContext->` reads P1 rewrites, so the tallies (`AddCalls(AccessorCalls, N)`) must be re-audited when reads move behind `PipeInputs` — the literal N at e.g. `VulkanRenderer.cpp:5273` (15) and `:6461` (3) counts accessor calls in that body. The counter NAMES are pinned by a test (§8) and are TracyPlot series names + JSON keys, so P1 must extend rather than rename; the natural P1 extension is finally calling `RecordDrawPayloadBytes` and lighting up `ResidualValueBlock`/`PersistentMapPush`.

---

## 8. The two unit suites

**`MG_Test/Pipe/PipeCatalogueTest.cpp` (322 lines).** Counting macros `:26-34`; per-payload POD asserts over both `MGP_CALL_LIST` and `MGP_VERIFY_PAYLOAD_LIST` `:38-44`. Tests: `HandleIsEightBytes` `:48`; `EntryCountMatchesTheDocumentedCount` `:65`; `GeneratedTablesHoldTheWholeCatalogue` `:72` (per-class 11/8/13/17/9/13 at `:86-91`); `UninstalledTablesAreAllNull` `:96`; `ResidualBlockSizeIsPinned` `:109`; `ValueTypeLayoutsArePinned` `:120` (`PixelStoreParameters` 28, `PerBufferBlendState` 28, `StencilFaceState` 28, `RenderStateParameters` **1168**, `SamplerParameters` 100, `VertexAttributeVersion` 6, `kMGMaxDrawBuffers` 8); `ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail` `:142` (offsets 0 / 1168 / 1200 / 1208); `WireOpcodesAreThePositionsInTheCatalogue` `:150`; `LateArrivalsAreAppendedWithoutRenumbering` `:164`; `ApplierAcceptsAWellFormedRecord` `:174`; `VerifyComparatorNamesTheDifferingField` `:184`; `CoverageAccountsForEveryInventoryRow` `:212` (477 rows, 0 unmapped); `PipeInputFieldsStartUnfilled` `:224` (**`EXPECT_EQ(kMGPipeInputFieldCount, 61u)` at `:225`** — this is the number that moves when P1 fixes the accessor set, see §9.1); `PipelineSubsetMembersArePinned` `:237`; `ReverseChannelHasTenCallbacks` `:244`; `HostSpanResolvesTheMonolithPointer` `:252`; `BufferRangeCarriesNoInlineHostSpan` `:271`; `SubDataBufferRangeRidesInTheUnionBox` `:297`.

**`MG_Test/Util/PipeStatsTest.cpp` (287 lines).** Fixture `:28`; `EnabledLatchIsTheOnlyGate` `:42`; `ByteClassesAccumulateIndependently` `:49`; `PresentClearsTheFrameButKeepsTheTotal` `:66`; `InitLatchesTheSummaryPeriodFromTheConfigAndNeverKeepsZero` `:81` (pokes `MG_Config::Features.PipeStatsPeriod` directly, `:85-92`); `GateHitsAndMissesAreSeparateCounters` `:96`; histogram tests `:112`, `:129`; `SummaryLineCarriesEveryClassAndGate` `:137`; `PerFrameFieldsKeepTwoDecimals` `:159`; `SummaryLinesReportDisjointWindows` `:174`; `FormattingTwiceDoesNotConsumeTheWindow` `:191`; zero-draw/zero-frame `:206`, `:221`; `JsonDumpNamesEveryCounter` `:234`; **`CounterNamesAreStable` `:260`** (pins all nine byte-class names and all six gate names verbatim — a rename is a breaking change); `SummaryPeriodIsOneHundredAndTwentyFrames` `:280`. Target defined at `MG_Test/Util/CMakeLists.txt:38-54`, registered `:58`.

---

## 9. CI: the `pipe-gates` job and its neighbours (`.github/workflows/test.yml`)

Jobs in the file: `build-linux:12`, `test:149`, `integration:205`, `flatc-check:304`, `include-graph-check:332`, `benchmark:351`, `build-retrace:407`, `trace-cases:552`, `trace-fixtures:572`, `retrace:637`, `retrace-summary:743`, `remove-artifact-clutter:784`, **`pipe-gates:835`**.

`pipe-gates` (`:835-887`), deliberately independent of `build-linux` (`:838-839`):

1. `:850-853` — **"Regenerate the MGPipe interface (G1-G7)"**: `python3 scripts/gen_pipe.py` then `git diff --exit-code -- MobileGL/MG_Pipe/generated`. Note it runs the *writing* mode and diffs, not `--check`; and the diff path is **scoped to `generated/`**.
2. `:862-869` — stdio-instrumentation grep gate over `MobileGL/MG_Backend` and `MobileGL/MG_State` (fprintf/printf/puts/std::cout|cerr), no exceptions.
3. `:873-874` — `python3 scripts/gen_pipe_dirty_surface.py --summary`, **informational**; the comment says "It becomes a gate in P1, when the mapping file exists to diff against."
4. `:879-887` — `scripts/check_doc_citations.py` on `docs/Disaggregated/*.md`, warning-only (`|| true`).

Other relevant lanes: `test` runs `ctest -L unit` (`:186-192`); `integration` runs `ctest -L integration-gpu` **and then a second, filtered pass with an inline env var** (`MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 ctest ... -R 'Buffer|Readback|...'`, `:281-288`) — this is the exact precedent shape for P1's third CI mode, and the comment at `:277-280` explains why an inline env works here while a ctest `ENVIRONMENT` property would not. `include-graph-check` (`:332-349`) runs `python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all`.

`scripts/check_include_closure.py` PROBES `:73-120`: `value-header` (`MG_Pipe/MGPipeValueTypes.h`, forbidding `MG_State/`, `MG_Impl/`, `MG_Backend/`, `MG_Remote/`), `artifacts-header` (`ProgramArtifacts.h`, plus `TextLimits {"glslang::": 2}`), `wire-header` (`MG_Remote/Transport/ITransport.h`, forbidding `Includes.h`). `REQUIRED_PROBE_NAMES = ("value-header", "artifacts-header")` at `:122` — deleting a probe is red. Also registered as a ctest at `MG_Test/Purity/CMakeLists.txt:9-12` (`--mode text`, label `unit`, with an explicit warning at `:11` that adding an `ENVIRONMENT` property would replace the job env).

---

## 10. P1 deliverables: what is partial, what is absent

### 10.1 `MG_Backend/MGPipe/PipeInputs.h` (accessors) — **nothing exists**; the field set is measurably stale

`MobileGL/MG_Backend/` contains only `BackendObject.cpp/.h`, `BackendObjects.h`, `Init.cpp`, `DirectGLES/`, `DirectVulkan/`. There is no `MGPipe/` directory.

Partial code that P1 builds on: the G5 tables (`PipeFilled.inc:28-306`) are the field id space; `MGPipeValueTypes.h` supplies the value types the phase-A accessors must return "with types identical to what the backend reads today".

**Measured today (whole `MobileGL/MG_Backend`, `*.cpp` + `*.h`):**

- `pGLContext->` occurrences: **277** (all spelled `MG_State::pGLContext->`). Per file: `DirectGLES/DirectGLES.cpp` 91, `DirectGLES/Managers.cpp` 15, `DirectGLES/MultiDraw.cpp` 5, `DirectGLES/Utils.cpp` 2, `DirectVulkan/BackendObject_DirectVulkan.cpp` 2, `DirectVulkan/DirectVulkan.cpp` 12, `DirectVulkan/Renderer/UniformManager.cpp` 14, `Renderer/VkClearManager.cpp` 1, `Renderer/VkRenderPassManager.cpp` 3, `Renderer/VkTextureManager.cpp` 2, `Renderer/VulkanRenderer.cpp` 127.
- Lines mentioning `pGLContext` without the arrow: **58** (matches the roadmap's "58 行非箭头清单"). Breakdown of those lines: 43 `MOBILEGL_ASSERT(...)`, 14 `!= nullptr`, 5 bare `if (MG_State::pGLContext)`, 1 `== nullptr`, 1 `.get()` (`DirectGLES/DirectGLES.cpp:147`: `MG_State::GLState::GLContext* ctx = MG_State::pGLContext.get();`).
- **ROADMAP.md:17 says "sed 293 处"; the tree has 277.** The 293 comes from the vendored inventory snapshot (`scripts/data/backend_read_inventory.md:9` "pGLContext accesses: 293"), taken before P0's D21 XFB counter-slot rekey. Implementers should work from the measured 277 + 58 and re-measure at the start of the task.
- Distinct accessors called: **62** overall — **Espryt 32** (`DirectGLES/`) and **Magma 56** (`DirectVulkan/`), against the plan's "Espryt 32 / Magma 55".

**Catalogue drift, the single most important finding for P1:** the live accessor set and `Coverage.def`/G5 disagree.

| accessor | status |
|---|---|
| `GetBoundTransformFeedbackLifetimeId` | **called** at `MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:11219`; declared `MG_State/GLState/Core.h:441`. **No row in `Coverage.def`, no field id in `PipeFilled.inc`.** |
| `HasOpenTransformFeedbackSpan` | **called** at `VulkanRenderer.cpp:11236`; declared `Core.h:450` (defined `Core.cpp:1278`). **No row, no field id.** |
| `GetBoundTransformFeedbackName` | **has** a row (`Coverage.def:34`, → `SetStreamOutputTargets`) and a field id, but is **no longer called anywhere in `MG_Backend`**; the inventory row that produced it is `backend_read_inventory.md:594` pointing at `VulkanRenderer.cpp:11137`, a line the D21 rekey replaced. `Core.h:429` still declares it (frontend use only). |

So P1's `PipeInputs` must serve **62** accessors, `kMGPipeInputFieldCount` moves from 61 to ~62–63, and `PipeCatalogueTest.cpp:225` (`EXPECT_EQ(kMGPipeInputFieldCount, 61u)`) plus `PipeFilled.inc:94`'s `static_assert(... == 61)` must be updated in the same commit. The vendored inventory itself is stale (477 rows / 293 accesses / 57 files, `backend_read_inventory.md:8-13`) and G6's `0 UNMAPPED` is therefore a statement about the snapshot, not about HEAD — re-vendoring it (or teaching G6 to also cross-check the live tree) is a real P1 decision.

Also pre-flagged by the sources themselves: `GetBufferBindingSlot` is polymorphic over `BufferTarget` and its 29 rows are supposed to split into `SetVertexBuffers`/`SetIndexBuffer`/`SetIndirectBuffers`/`SetShaderBuffers` "once the inventory carries the target argument (P1)" (`Coverage.def:36-40`).

### 10.2 `MGB_CTX` — **nothing exists**

No occurrence of `MGB_CTX` anywhere in the tree. The plan's sketch is `ARCHITECTURE.md:329-334`. Constraints P1 inherits: the gating macro must be `MOBILEGL_PIPE_PUSH` as a *compile-time* macro, which today exists only as the **runtime** `MG_Config::Features.PipePush` (`Config.h:326`) — P1 must decide whether the bitmap is compile-time (as the `#if` implies), runtime, or both, and `ConfigLoader.cpp:245` already parses the runtime one. Purity gate C is specified as `grep -c 'pGLContext' MG_Backend/ == 0` (`ARCHITECTURE.md:503`, gate 1-C) — note it greps `pGLContext`, **not** `pGLContext->`, so the 58 non-arrow lines have to go too, which is why they are an explicit deliverable.

### 10.3 Per-verb fill points — **table exists, generator does not**

`kMGPipeInputFieldFilledBy[]` (`PipeFilled.inc:229-291`) records *which call* is expected to have filled each field, derived from `Coverage.def`. What does **not** exist is the inverse table the plan asks for — "G5 从 PipeCalls.def 生成『每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些字段』的表，在 MG_Impl 的 ~93 个边界站点生成 validate/fill 调用" (`ARCHITECTURE.md:344`). Building it needs a *per-call → field set* relation, which `Coverage.def` today provides only in the many-to-one direction (accessor → one call). Concretely: 61 accessors map onto 20 distinct calls; inverting that gives, e.g., `CreateRenderState` ← 13 fields, `SetDynamicState` ← 14, `SetSamplerViews` ← 8. Whether that inversion is the right "may read" set for a *verb* (`DrawVbo`, `Clear`, `Blit`, `GenerateMipmap`, …) is a judgement P1 must encode — it is not derivable from the current `.def` files, because the calls named in `Coverage.def` are mostly `kCtxState`/`kCtxCso`, not verbs.

The "~93 边界站点" figure is the `gBackendFunctionsTable.GL.*` call-site count in `MG_Impl` that G2's thunks target (`ARCHITECTURE.md` G2 row, `:76`).

### 10.4 Poison generations — **skeleton exists, unused, and its debug gate is wrong**

Exists: `MGPipeFilledState` (`PipeFilled.inc:293-296`), `MGPipeInputPoisonFatal()` (`:298-302`, message shape `Fatal{UnmigratedPipeInput, "<field>@<verb>"}`), `MGPipeInputFieldIsFresh()` (`:304-307`), `kMGPipeInputFieldSticky[]` (`:163-227`, **all 61 entries `false`**). Missing: any instance of `MGPipeFilledState`, any bump of `CurrentVerbSerial`, any read-side assertion, and any sticky classification. The generator emits the sticky table unconditionally as `false` and its own comment (`gen_pipe.py:478-480`) demands that every `true` be argued for in P1 — meaning P1 must extend `Coverage.def` (or add a new hand-maintained source) with a sticky column, and grow `gen_filled` to consume it.

Gate spelling: use `#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG` (`Defines.h:104-115`) and/or `MOBILEGL_BUILD_DISAGGREGATED` — **`MOBILEGL_DEBUG` does not exist**.

### 10.5 The shadow comparator (G4 / `MOBILEGL_PIPE_VERIFY`) — **comparators exist; harness, snapshot and coverage assertion do not**

Exists and works: 63 `MGPipeVerify()` overloads, the trait, the array overload, bit-exact float comparison, first-differing-field reporting, all in `PipeVerify.inc`; unit coverage at `PipeCatalogueTest.cpp:184-208` and `:285-291`.

Missing / must grow:
1. `SnapshotFromGLContext()` — does not exist anywhere.
2. The harness that calls the comparator per draw with a draw serial.
3. **Field lists for the four memcmp-fallback types** (`gen_pipe.py:167-172` names them; `PipeVerify.inc`'s fallback comment says P1 closes it). Today `ResidualValueBlock.RenderState` (1168 bytes), `.Pack`, `MGPCaps.Dynamic` and `MGPPixelPackState.Pack` all compare by `memcmp`, i.e. the comparator has exactly the padding false-positive property the design forbids. Adding `MGP_FIELDS_RenderStateParameters` / `_PixelStoreParameters` / `_DynamicBackendParameters` / `_MGHostSpan` to `PipeFields.def` and removing them from `MEMCMP_FALLBACK_TYPES` is a mechanical but load-bearing P1 edit; `RenderStateParameters` is defined at `MGPipeValueTypes.h:239` and is the big one.
4. "the comparator's coverage is itself asserted" (`PipeFields.def:15-17`) — no such assertion exists. A field added to a payload in `MGPipeTypes.h` without a `PipeFields.def` entry is silently invisible today; only the *payload* (not the field) is checked (`gen_pipe.py:187-195`).
5. The `MOBILEGL_PIPE_VERIFY` **CMake option** (absent, §6) and its interaction with the already-parsed runtime `Features.PipeVerify` (`Config.h:332`, currently read by nobody).
6. The retain mode for consume-and-clear groups (texture dirty rects) described at `ARCHITECTURE.md:502`.

### 10.6 The third CI mode — **precedent exists, job does not**

Nothing in `test.yml` sets any `MOBILEGL_PIPE_*` variable. The shape to copy is `integration:281-288` (a second `ctest` invocation with an inline env var, plus the `:277-280` caution that a ctest `ENVIRONMENT` property would *replace* rather than extend the job env — the same trap is called out in `MG_Test/Purity/CMakeLists.txt:11` and `ARCHITECTURE.md:567`). The gate text (`ROADMAP.md:17`) is: 40 traces + all integration tests under `MOBILEGL_PIPE_VERIFY=1` with zero divergence, plus two negative controls (corrupt one snapshot field → verify red; drop one field from `glGenerateMipmap`'s fill table → poison Fatal **on that verb**). Trace lanes to hook: `trace-cases:552`, `trace-fixtures:572`, `retrace:637`; retrace env passes through `--env K=V` → intent extra `mobilegl_env` → `setenv` before loading the library (`ARCHITECTURE.md:445`).

### 10.7 Which generator must grow, and how `--check` must cover it

| generator | P1 growth | `--check` coverage requirement |
|---|---|---|
| **G5 `gen_filled`** | (a) absorb the 2 missing / 1 dead accessor; (b) emit a **per-verb "may read" table** (new output, e.g. `PipeFillPoints.inc`); (c) emit real sticky flags from a new column in `Coverage.def`. | Any new file must be produced by a `write(os.path.join(GENERATED_DIR, ...), ...)` call added to `main()` between `:646` and `:652` — that is what makes both `--check` and the CI `git diff -- MobileGL/MG_Pipe/generated` see it. A new *input* column must also get its own `sys.exit` validation in `parse_coverage()` so a malformed row fails in `--check` rather than silently emitting a wrong table. |
| **G4 `gen_verify`** | consume the four new field lists; shrink/empty `MEMCMP_FALLBACK_TYPES` (`:167-172`); add a per-payload **field-count** assertion so a payload that grows a member without a `PipeFields.def` entry fails. | The field-count check needs a source of truth for "how many fields does this payload have" — the only mechanical one available is parsing `MGPipeTypes.h`, which the generator does not do today. Simplest honest form: emit `static_assert(sizeof(P) == <literal>)` next to each comparator so any layout change is red, and require the literal to be regenerated. |
| **G6 `gen_coverage`** | re-vendor `scripts/data/backend_read_inventory.md` at HEAD (or the accessor drift stays invisible); split `GetBufferBindingSlot` by target (`Coverage.def:36-40`). | Both modes already `sys.exit` on an unknown call name (`:518-520`) and on missing macros. Keep `kMGPipeInventoryUnmapped == 0` and `PipeCatalogueTest.cpp:212-220` in sync with the new row count. |
| **`gen_pipe_dirty_surface.py`** | add the mapping file + a `--check` mode with `git diff --exit-code` and zero unmapped mutators (its own docstring `:21-22`); promote `test.yml:873-874` from informational to a gate. | It has no `write()` helper and no `--check` at all today; both must be added, and the mapping file has to live somewhere the CI step diffs (choose a path and add it to the `git diff` argument list — the existing step only diffs `MobileGL/MG_Pipe/generated`). |
| **G7** | untouched in P1 (chunk table + setter-consistency test are P2, `PipeSpanTable.inc:64-67`). | — |

Also: since P1 is the first time the library compiles `MGPipe.h`, whatever new `.cpp` files land (`MG_Backend/MGPipe/MGPipeImpl_DirectGLES.cpp`, `..._DirectVulkan.cpp`) must be added by hand to the explicit `SOURCE_FILES` list (`CMakeLists.txt:234` ff., backend block at `:366-393`) — there is no glob.

---

## 11. Naming / namespace / include conventions the new files must follow

1. **File header.** Every source and generated file starts with the eight-line banner: `// MobileGL - <repo-relative path>`, the two-line copyright, three licence lines, `// SPDX-License-Identifier: LGPL-3.0-only`, `// End of Source File Header`. See `MG_Pipe/MGPipe.h:1-7`, `gen_pipe.py:42-48`.
2. **Include spelling.** `MG_Pipe` headers are reachable **both** as `<MG_Pipe/X.h>` (via `${CMAKE_SOURCE_DIR}/MobileGL` on the include path) and as `<X.h>` (via `${CMAKE_SOURCE_DIR}/MobileGL/MG_Pipe`, `CMakeLists.txt:531-535`). Because of that double reachability, `MGPipeValueTypes.h:10-11` carries **both** `#pragma once` and an `MOBILEGL_MG_PIPE_VALUE_TYPES_H` guard — new headers under `MG_Pipe/` that MG_Backend will include should do the same. `MG_Backend` headers are spelled `<MG_Backend/...>` (see `MGPipeTypes.h:33`), so the documented P1 path is **`<MG_Backend/MGPipe/PipeInputs.h>`** (`ARCHITECTURE.md:328`, build-layout table `:521`). Within `MG_Pipe` the sibling includes are relative quotes (`MGPipe.h:12-15`, `PipeVerify.inc:387` uses `"../PipeFields.def"`).
3. **Namespaces.** Pipe code lives in `namespace MobileGL::MG_Pipe` (`MGPipe.h:26`); value types live directly in `namespace MobileGL` (`MGPipeValueTypes.h:27`) with a re-opened `namespace MG_State::GLState` inside it (`:473`) for the vertex-attribute types; counters live in `namespace MobileGL::MG_Util::PipeStats` (`PipeStats.h:41`). Backend code is `namespace MobileGL::MG_Backend` (`BackendObject.h`). The plan's `gPipeInputs` is spelled `::MobileGL::MG_Pipe::gPipeInputs` in the `MGB_CTX` sketch (`ARCHITECTURE.md:331`) even though the header lives under `MG_Backend/MGPipe/` — resolve that inconsistency explicitly rather than by accident.
4. **Symbol naming.** Generated constants are `kMGPipe*`; generated types `MGPipe*` / `MGP*`; wire records `MGPWireRec_<CallName>`; thunks `MGP_<CallName>`; global tables `gMGPipe*`. Payload structs are `MGP<Noun>` and each gets `MGP_ASSERT_POD(T, Size)` (`MGPipeTypes.h:44-46`). Config fields are `Pipe<Thing>` on `MG_Config::Features` with env var `MOBILEGL_PIPE_<THING>`.
5. **Generated-file discipline.** Outputs go under `MobileGL/MG_Pipe/generated/`, extension `.inc`, LF line endings (`gen_pipe.py:625`), committed to the tree, carrying the "GENERATED by scripts/gen_pipe.py from <sources> - DO NOT EDIT" banner (`:52-53`). The banner also asserts inclusion from `MGPipe.h` inside the namespace — honour it or parameterise it.
6. **Logging.** `MGLOG_D` for non-critical (compiled out in INFO builds); `MGLOG_I` only with a stated reason (`PipeStats.cpp:224-231` is the precedent for an opt-in measurement line); `MGLOG_F` + `std::abort()` for the fatal shapes (`PipeFilled.inc:298-302`, `PipeWire.inc`'s `MGPipeWireProtocolFatal`). No `printf`/`fprintf`/`std::cout` anywhere under `MG_Backend` or `MG_State` — CI greps for it (`test.yml:862-869`).
7. **Commit messages.** `[Type] (Scope): description`, single line, no `Co-Authored-By` (repo convention; see `git log`).

---

## 12. Traps worth carrying into the implementation

- `parse_verify_payloads`, and both `parse_coverage` blocks, terminate their regex at a **blank line**; appending to those macros without keeping the trailing empty line silently truncates the parse (`gen_pipe.py:177`, `:201`, `:207`).
- `parse_calls` stops at the first line in `MGP_CALL_LIST` without a trailing backslash (`:149-150`) — a wrapped comment line inside the macro that loses its backslash silently truncates the catalogue, and the documented-count check is the only thing that catches it.
- `PipeCatalogueTest` hard-codes 61 (`:225`), 477 (`:213`), 24 (`:238`), 71/72 (`:151-169`), 11/8/13/17/9/13 (`:86-91`), 1168/1248 (`:110-124`) — every P1 change to the field set or the catalogue lands in this file too.
- `PipeStatsTest.CounterNamesAreStable` (`:260-279`) pins every counter name; extend, never rename.
- The CI regeneration step diffs only `MobileGL/MG_Pipe/generated` (`test.yml:853`); a generated file placed elsewhere (e.g. a dirty-surface mapping under `scripts/data/`) needs its own diff argument.
- `ctest` `ENVIRONMENT` properties **replace** the job environment (`MG_Test/Purity/CMakeLists.txt:11`, `test.yml:253-258`); the verify lane must set its env inline on the `ctest` command line, as the integration lane's second pass does.
- `MG_Test/Wire` and everything under `MG_Remote/` only exist under `MOBILEGL_BUILD_DISAGGREGATED` (`MG_Test/CMakeLists.txt:103-105`, `CMakeLists.txt:455-490`); P1's work must stay green in the default OFF build, where `MGPipe.h` is compiled by nothing but the unit test today.
