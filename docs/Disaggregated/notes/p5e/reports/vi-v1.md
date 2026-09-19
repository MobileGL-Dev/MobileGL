# vi — VAO / vertex input / index, v1

Branch `p5e/vi`, head **112c1d8d**, base `f6cfcbd3`. Three commits, no trailers. Tree clean.

## 1 What changed, per file (line ranges at the BASE head)

**`DirectGLES.cpp`** — `:490` `VertexInputReadsRecords()` (arm selector: `Transport != Monolith` AND
the family bit, ruling 1 / ID-81) + `AnyClientSideVertexArrayInRecord()` (reads BOTH views: a null
`Res` is "client-sourced" only for an attribute the CSO declares ENABLED, since the window also
carries nulls for disabled slots below the high-water mark). `:786`
`SyncVaoAttributeBuffersByRecord(memo, epoch)` beside the existing `...ByHandle` — same memo, same
three-value key, walk over `st.VertexBuffers[Start..+Count).Res`, dedupe on `{slot,gen}`,
`EnsureBufferResourceForHandle(nullptr, Res)`, probe with `frontend == nullptr` — plus the two
named source decisions `ResolveDrawVertexBuffersFromRecord` / `ResolveDrawIndexBufferFromRecord`
(§3: they exist so a unit case drives the production decision, not a copy of it). `:807, :827-831`
the null-VAO early return no longer fires on the record arm and the dispatch takes it first.
`:896-941` the IBO arm reads `st.IndexBuffer.Res` + `st.IndexBufferSerial`; the legacy and
push-monolith arms unchanged inside a new brace block. `:1602-1639` `ResolveVaoTwin(SharedPtr)`
keeps its body and LOSES its `MGPipeFrontendKeyedRegistryScope` (§4.4's `:1609`). `:1655` new
`SyncCurrentVAOFromRecords(twin)`. `:1667-1694` `SyncCurrentVertexAttributeValues` reads neither
`GetBoundVertexArray` nor `GetConfigVersion`; memo keyed `{elementsHandle, ContentSerial,
activeMask}` over `rec->Attributes[i].Enabled`. `:4770-4795` `PrepareForDraw`'s VAO lines only, and
`AccessorCalls` is 1 per draw on the record arm (it was 2 and one of the two reads is gone — the
ledger has to say so). `:6446-6461` `RefuseElementArrayBufferFromTheFrontend` in both restart
helpers. `:6718-6788` the two client-array arms folded into one file-local `static`
`SyncClientSideVertexArraysForDrawArrays`; both scopes (`:6729`, `:6775`) deleted, twin by handle.

**`Managers.h`** — `ResolvedDrawBuffers::iboSerial` (push-only); `Entry::frontend`/`::attribIndex`
re-documented as monolith glue / diagnostics; `PendingAttribValueMask` gains `{elementsHandle,
elementsSerial}` (push-only); `SyncToBackendFromApplier` private → public (§4.1: "the object
parameter is deleted from the transport overload, not defaulted to null");
`BufferMutationEpochBumpsOffTheApplyThread()`; the two `Resolve*FromRecord` declarations.
**`Managers.cpp`** — `BumpBufferMutationEpoch` only (§4b); `SyncToBackendFromApplier`'s body is
UNCHANGED and is this family's existence proof.

**`EmitTables.{h,cpp}`** — `RemoteDrawBindings::ClientVertexArrays`, answered inside
`ReadDrawBindings`' one binding read; `PlanDrawInfo` sets `kDrawClientArrays` for every draw shape
(a `glDrawElements` can fetch its VERTICES from client memory); `EmitDrawRecord` and the reduced
`EmitDrawArrays` path refuse under run-ahead.

**Tests** — `MG_Test/SanityTest.cpp` (+ pull skip twins), `MG_Test/Pipe/VertexInputEmitTest.cpp`,
new `MG_IntegrationTest/Scenarios/ClientVertexArrayScenario.cpp` with its `CMakeLists.txt` block and
its `Harness/SplitLogPaths.cmake.in` entry.

## 2 The kimi-audit rows of this family, 1–34

**RETIRED** (not read under a transport): **1** (`GetBoundVertexArray@PrepareForDraw`), **2**, **3**
(twin probe + mint), **5** (`GetConfigVersion`), **6** (the VAO handoff), **9**, **10**'s
`:1683`/`:1688` halves (`:1700` was already RECORD_SUPPLIED), **12**, **13** (the `Find(vao.get())`
probe is gone unconditionally; the VAO itself is taken only when the record says an enabled
attribute is client-sourced), **16**, **17**, **19**, **20**, **21**, **23**, **24**; and **25** /
**27** *for this family's callers* (`HandleOfBuffer` inside `IsBufferDrawClean` /
`EnsureBufferResource` is no longer reached from the vertex-input arm; sb/tx2/fb keep their own).
Row **18**'s frontend argument is gone (the record arm passes `nullptr`).

**LEFT BARRIERED**: **14** / **34** — `SyncClientSideAttributesForDrawArrays` still dereferences
`attrib.Offset` as a client pointer (§4a). **15** — `MultiDraw.cpp:96`, see R2.
Rows **4**, **7**, **8**, **22**, **26** are legacy-arm-only bodies and die with the legacy arm, not
with this package.

**NOT FOUND** as a live split read: **11** (the restart helpers were already unreachable under a
transport — the P5c substitution sets `serverElementBinding >= 0` before its null test; now they
refuse by name instead of being unreachable-by-reading), **28**, **29** (monolith-only already;
passing `nullptr` changes nothing), **33** (already `Transport == Monolith`-gated).
**NOT vi's**: **30**, **31**, **32** (sb's).

## 3 Red-once evidence, verbatim

**(1) Twin key.** `PrepareForDraw`'s transport arm reverted to
`ResolveVaoTwin(MGB_CTX->GetBoundVertexArray())`; both `DirectGLES.Split.TriangleScenario` entries
`Subprocess aborted`:

```
[08:02:07] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{RoleViolation, "MGPipeSlots"} - the apply thread called MGPipeSlots().HandleOf. With an active transport the client slot allocator is client-only memory (CONTRACT-P5C §3.1, rule E; CONTRACT-P5E §4.4): a handle arrives already minted in a record, and a server that resolves or mints one off a frontend object's lifetime id is reading memory that will not exist on its side of a real split. A named exemption scope admits it only while the record being applied is BARRIERED (barriered=1) and, for Magma's P7 debt, only on a DirectVulkan server
```

Note `barriered=1`: **no id test hook was needed**. Deleting the scope is what makes the revert loud
today rather than after ra flips the wait rule.

**(2) The buffer walk.** `ResolveDrawVertexBuffersFromRecord` reverted to
`MG_State::pGLContext->GetBoundVertexArray()->GetAllAttributes()`:

```
../MobileGL/MG_Test/SanityTest.cpp:5088: Failure
Expected equality of these values:
  count
    Which is: 1
  2u
    Which is: 2
the walk did not resolve st.VertexBuffers[Start..+Count): two DISTINCT handles are named there (A twice and B), plus one client-memory array with no store to ensure
```

**(3) The index arm.** `ResolveDrawIndexBufferFromRecord` reverted to the VAO's element slot:

```
../MobileGL/MG_Test/SanityTest.cpp:5149: Failure
Expected equality of these values:
  request.Res
    Which is: 8-byte object <02-00 00-00 00-00 00-00>
  publishedHandle
    Which is: 8-byte object <01-00 00-00 00-00 00-00>
the index arm did not read st.IndexBuffer.Res; if it answered the VAO's element slot it is reading a binding the client has already changed
../MobileGL/MG_Test/SanityTest.cpp:5152: Failure
  request.Serial
    Which is: 0
  11u
IndexBufferSerial did not travel with the handle
```

**(4) Client arrays under run-ahead.** `kMGPipeP5eRunAheadReady` flipped to `true` in
`MG_Backend/Init.cpp`, `ClientVertexArrayScenario` run in the split lane — exit 134 and, on the GL
thread's own log:

```
[08:03:18] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}
```

**(ID-95 window pin, extra.)** `EmitVertexBuffers`' count narrowed by one:

```
../MobileGL/MG_Test/Pipe/VertexInputEmitTest.cpp:490: Failure
Expected: (i) < (window.Start + window.Count), actual: 7 vs 7
create_vertex_elements declares attribute 7 ENABLED but set_vertex_buffers' window ends at 7 - the same skip, at the other end. A narrower window is the sink's Fatal{ProtocolCorruption, "SetVertexBuffers.Count"}
```

Every revert was put back and re-verified green before the commits.

## 4 The two answers the gate asked for

**(a) Does any existing scenario exercise a client vertex array under split? NO.** Every
`glVertexAttribPointer` in `MG_IntegrationTest/Scenarios` is issued with `GL_ARRAY_BUFFER` bound, so
its last argument is a byte offset; `DoublePrecisionScenario` — the brief's candidate — uses
`glVertexAttribFormat`/`glVertexAttribBinding`/`glVertexAttribLFormat` against real buffer objects
(`DoublePrecisionScenario.cpp:944-959`). `ClientVertexArrayScenario` is therefore new and is the
only lane entry that can reach the refusal.

**A DEVIATION FROM THE BRIEF that needs the integrator's eye.** The brief makes the
`DrawArrays`/`MultiDrawArrays` arms *monolith-only*. **They cannot be, before ra lands.** Under split
LOCKSTEP a client vertex array is not refused (ID-82: lockstep unchanged), so a monolith-only arm
would silently drop the attribute upload and render wrong *today*. What landed instead: the arm
stays on both, resolves the twin BY HANDLE (so the probe and both scopes go either way), takes the
frontend VAO only when `AnyClientSideVertexArrayInRecord()` is true — which is what actually retires
`GetBoundVertexArray@DrawArrays` for ordinary draws — and aborts by name if it is ever reached from
an UNBARRIERED apply, which escalation (ii) of §2.1 and the client refusal both make unreachable.
The new scenario passes on all four arms (DirectGLES / DirectVulkan / DirectGLES.Split × 2 cases),
which is the evidence that the arm is load-bearing. `SyncClientSideAttributesForDrawArrays` is
likewise NOT made monolith-only.

**(b) Does any GL-thread path bump `CurrentBufferMutationEpoch` under a transport? YES — exactly one
class, and it is before the apply thread exists.** An unconditional refusal in
`BumpBufferMutationEpoch` took the whole split lane down in EGL bring-up, which is how this was
found rather than reasoned: `BackendObject_DirectGLES::Initialize()` runs on the APP thread by
design and ends in `RegisterBufferBackendOps()`, whose last act is a bump. At that moment
`ServerLoop::Detail::g_applyThreadKey == 0` — no record applied, no twin, no memo stamp for it to
slide under. What landed is the narrower pin: a bump off the apply thread *while one is running* is
counted (`BufferMutationEpochBumpsOffTheApplyThread()`) and **Fatal under
`MOBILEGL_IPC_STRICT_ERRORS`**; across the 113-entry split lane it never fires. Not Fatal by default
because `StagedShadowProductionTest`/`ServerLoopTest` drive the real op tables from the test thread
beside a started loop — 7 unit cases went red on the wider form, and policing a fixture is not what
this pin is for.

## 5 Gate

| step | result |
|---|---|
| `build` | rc=0 |
| `unit` | **2209/2209** (base 2206; +3, each with a pull skip twin) |
| `isplit` | **113/113** (base 111; +2 `ClientVertexArrayScenario` split entries) |
| `gens` | all 7 rc=0; `check_doc_citations.py --strict` rc=0 (its three printed lines are pre-existing `CONTRACT-P5.md` ambiguities) |
| `one`, the eight named scenarios | **274/274**, covering BOTH the monolith `DirectGLES.*` arm and `DirectGLES.Split.*`, so ruling 1's A/B is exercised and `ObjectSubsystemControlScenario`'s `0x0ff`/`0x17f` verdicts are unchanged |
| `strict` | red by design (110/113 abort), markers below |

**Strict markers, before → after** (per-entry private logs, ID-53; counts are log lines):

| `field@verb` | base (111) | after (113) | owner |
|---|---|---|---|
| `GetBoundVertexArray@DrawArrays` | **14** | **0 — GONE** | vi |
| `GetProgramForDraw@DrawArrays` | 0 | **14 (new)** | pg — the next field in *those same 14 entries* |
| `GetFramebufferBindingSlot@Clear` | 39 | 41 | fb (+2 = my two new lane entries, which clear first) |
| `GetImageTextureBinding@BindImageTexture` | 13 | 13 | fb |
| `GetTextureUnitObject@Clear` | 9 | 9 | tx2 |
| `GetTextureObject@CopyImageSubData` | 6 | 6 | tx2 (barriered, allowlisted) |
| `GetProgramForDispatch@DispatchCompute` | 3 | 3 | pg |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | 2 | pg (barriered) |

This family's only visible field is retired; the field that surfaced behind it is pg's.

## 6 G1 / monolith impact

The pull build is **not rebuilt here** (the gate configures only `build-split`); the delta is named
rather than measured. Everything is under `#if MOBILEGL_BUILD_DISAGGREGATED` or
`#if MOBILEGL_PIPE_PUSH` except: one new **file-local `static`** function
(`SyncClientSideVertexArraysForDrawArrays`) re-homing two byte-identical inline blocks from the two
`DrawArrays` entry points (no exported symbol); three constant-folded branches
(`VertexInputReadsRecords()` is `return false` there, so the null-VAO guard, `PrepareForDraw`'s
selector and the `AccessorCalls` count all fold) plus two default-constructed null `SharedPtr`
locals; and `RefuseElementArrayBufferFromTheFrontend` compiling to `(void)entry;`. No struct layout
moves — `iboSerial` and the `PendingAttribValueMask` fields are `MOBILEGL_PIPE_PUSH`-only. G2/G14:
the new unit cases have pull skip twins; the new scenario's ctest entries appear in both arms
symmetrically. **Please re-run the pull configure at landing.**

## 7 Seams assumed, and what I need

1. **ra — `ClientSession::RunAheadArmed()`** does not exist yet. `EmitTables.cpp`'s
   `RunAheadWouldSkipTheWait(session)` spells the same conjunction `MGPipeTypes.h:148` states
   (`BarrierArmed() && Ipc.RunAhead && Caps().HasCap(kCapRunAheadApply)`). **ra should collapse that
   body into the latch**: a duplicated predicate that can disagree with the wait rule about which
   arm the session is on is the one failure mode here. Inert until the caps bit is published.
2. **tx2 — `CheckUnitWindows(st)`.** I pinned the PRODUCER half of ID-95 / ruling 19 only; the sink
   check and its `Fatal{ProtocolCorruption, "SetVertexBuffers.Count"}` are tx2's. My test names that
   exact string so the two halves meet.
3. **pg.** `SyncCurrentVertexAttributeValues` still reads `program->GetActiveAttributeLocationMask()`
   and `GetAttribType(location)` (kimi row 106, S1's R3). My half is done; it stays a frontend reader
   — a BARRIERED one — until pg carries the mask and types in the `ShaderCso` record.
   `GetProgramForDraw@DrawArrays` is now the marker those 14 entries die on, so pg will see it.

**Rulings needed:** **R1** — the "monolith-only" wording for the two `DrawArrays` arms and for
`SyncClientSideAttributesForDrawArrays` (§4a); confirm my reasoning or tell me to take the
regression. **R2** — `MultiDraw.cpp:96 BoundIndexBuffer()` / `:105 BoundIndexBufferId()` (kimi rows
15, 128) read `MGB_CTX->GetBoundVertexArray()` on every multi-draw with no transport arm, and
`MultiDraw.cpp` is in no package's scope in BRIEF-P5E; it is a two-line change of exactly §5.1's
shape. **Left as-is** — widen vi's range or take it. **R3** — `BumpBufferMutationEpoch` is in
`Managers.cpp` outside my declared range; the gate asked for the pin and there was nowhere else for
it. **R4** — `SyncToBackendFromApplier` moved private → public in `Managers.h` (a declaration, not a
body), for the reason §4.1 gives.

## 8 What I did not do

- `ResolvedDrawBuffers`' deletion (S1 R7) — trailing by instruction, measurement first. It is now
  cheap to measure: the memo's entries are `{handle, resource, bindingIndex}` over a window that is
  1–3 entries on the measured workload.
- Staging client vertex arrays (P8).
- A `RemoteClientTest` unit twin for `PlanDrawInfo`'s `kDrawClientArrays` flag — that file is not in
  vi's scope; the flag is covered end to end by the scenario's red-once instead.
- No pull-build configure (§6).
