# P3a package A `c0` — adversarial review of the contract (`contract-review-v1`)

Reviewed commit `39722687` (tag `p3a/contract`), parent `44c2b5cf`, worktree `/home/swung/w7/p3a-contract`.
Everything below was checked by opening the file at the commit (a throwaway `git worktree` at
`/home/swung/w7/p3a-review`, removed at the end, was used for `gen_pipe.py`; no build was run there).

## Verdict

**ACCEPT WITH MINORS — conditional.** The contract itself is right: every payload, flag, struct, subsystem
bit, entry-point signature, record and generated table I checked matches the B decisions it claims to
implement, and I could not find a path by which `c0` changes what a push or a verify build copies,
suppresses or emits. The three other trees are already branched from the tag and should proceed.

Two **major** findings and seven minors are listed below. Neither major changes an ABI, a signature or a
generated table, so **neither requires the tag to be re-cut**: M1 is a comment-only fix that A can land on
`p3a/wire`, M2 is a four-line guard in a file `C.5` hands to package C at the tag. Both must be closed
before D.1's `espryt` merge, not before the packages start.

---

## Findings

### M1 (major) — `Coverage.def`'s "target by target" split is not exhaustive: `DispatchIndirect` and `Query` are missing

`MobileGL/MG_Pipe/Coverage.def:41-47` (the new split), `:48-56` (the argument that rests on it).

The rewritten comment enumerates the split as ArrayBuffer / ElementArrayBuffer / DrawIndirectBuffer +
ParameterBuffer / Uniform + ShaderStorage + AtomicCounter + TransformFeedback / CopyRead + CopyWrite +
PixelPack + PixelUnpack + Texture — **13 of the 15 `BufferTarget` enumerators**
(`MG_State/GLState/BufferState/BufferObject.h:15-33`: Vertex, Index, Uniform, CopyRead, CopyWrite,
PixelPack, PixelUnpack, **Query**, Texture, TransformFeedback, AtomicCounter, **DispatchIndirect**,
DrawIndirect, Parameter, ShaderStorage). `DispatchIndirect` and `Query` appear in no bucket at all, not even
the "still pulled" one, while the block asserts "The split, target by target" and `:53-56` argues the row
may stay one row *because* the split is stated here instead.

`DispatchIndirect` is not a hypothetical: it is a live backend read at
`MG_Backend/DirectGLES/DirectGLES.cpp:725` (`SyncBoundBuffer(BufferTarget::DispatchIndirect, …)`) and
`MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:7423`, and it has **no call in the catalogue** —
`MGPIndirectBuffers` (`MGPipeTypes.h:405-409`) carries `DrawIndirect` and `Parameter` only. So the one
target that a reader would most plausibly assume `set_indirect_buffers` covers is exactly the one that
silently does not travel.

Failure scenario: P4b (shader buffers) or P8 (indirect) reads this block as the authoritative per-target map,
sees 13 targets accounted for and no residue, wires `EmittedCallSuppliesTheWholeField(GetBufferBindingSlot)`
or retires the pull for the field on the strength of it, and `m_bufferBindingSlot[DispatchIndirect]` stops
being written for every `glDispatchComputeIndirect`. No P3a gate can see it (the row is deliberately out of
the emitted list), and `Coverage.def` is A-only in `C.5`, so no later package may correct it.

Refutation attempted, and it failed: I checked whether `Query` and `DispatchIndirect` might be excluded
because no backend reads them — `Query` indeed has no backend read, but `DispatchIndirect` has two, so the
omission cannot be "only backend-read targets are listed". I also checked whether they might be covered by
another accessor row: `GetBufferBindingSlot` is the only row for that field, and `gen_pipe.py --check`
reports 0 UNMAPPED, i.e. nothing else picks them up.

Fix: two lines in the comment — add `DispatchIndirect` to the still-pulled bucket with the reason (no call
carries it; `MGPIndirectBuffers` is a two-handle payload) and `Query` beside it. Use the enum's own spelling
(`Vertex`, `Index`, `DrawIndirect`, `Parameter`) rather than the GL token names, so the list can be
cross-checked mechanically against `BufferObject.h:15-33`.

### M2 (major) — `BackendSlotTable::GetOrCreate(MGPipeHandle)` adopts a **stale** generation and destroys the live twin at the slot

`MobileGL/MG_Backend/DirectGLES/SlotTables.h:272-286`, specifically `:282-284`:

```cpp
Entry& entry = EntryAt(handle.Slot);
if (entry.Live && entry.Gen != handle.Gen) entry.backend.reset();
entry.Gen = handle.Gen;
entry.Live = true;
```

The `!=` is symmetric; the comment above it (`:262-265`) argues only the asymmetric case ("the slot was
recycled, so the twin at it describes driver ids the new resource never made"). For the *minting* overload
(`:202-246`) the symmetric form is provably safe, because the handle comes straight out of
`MGPipeSlots().Acquire` and can never be behind the entry. For the **handle** overload the handle "ARRIVED,
in the call's payload" — that is the whole point of the overload — so `handle.Gen < entry.Gen` is a
reachable input, and the code then destroys the *incumbent, live* twin (a driver buffer id, a persistent
map, a pooled store) and stamps the slot back to the dead resource's generation. The successor's next
`FindByHandle` (`:294-301`) then correctly refuses (`entry.Gen != handle.Gen`) and it is silently handed a
fresh empty twin: the resource loses its storage with no diagnostic. That is the shape of the bug commit
`d7655247` fixed, and it is exactly what `MGPipeHandle::Gen` exists to prevent
(`MGPipeHandles.h:44-53`).

The two handle-keyed entry points A ships as the contract therefore give **opposite** answers to a stale
generation: `FindByHandle` refuses it, `GetOrCreate` adopts it. `C.4` tells package C it may assume this
overload, and the header's comment does not warn.

Refutations attempted:
- *"It is unreachable in P3a monolith."* Largely true, and this is why it is not a critical: under D-L's
  fixed order (`~BufferObject` → `MGPipeEmitResourceDestroy` → `MGPipeSlots().Free`) the backend sees
  handles in order, and Espryt's off-thread queues (`pendingResidentWrites`, `g_deferredBufferReleases`) are
  keyed on the resource, not on a handle. I could not construct a P3a-monolith sequence that reaches it.
  But the contract is written for the split, the applier's own record *does* gen-check
  (`MGPipeResourceRecord::Gen`, `PipeApply.h:99-118`) and this one does not, and `MOBILEGL_ASSERT` is a
  no-op in Release (`Defines.h:114`), so there is no debug trip wire either.
- *"C can fix it — `SlotTables.h` is C's from the tag on (`C.5`)."* True, and it is why this is not a
  REWORK: the fix does not re-open the tag. It is reported here because three packages are being written
  against the comment now, and because the result file's §1 states the behaviour as "resets a twin whose
  `Gen` no longer matches", which reads as complete and is not.

Fix (C's tree, or A's `c1`): refuse the backwards case rather than adopt it —
`if (entry.Live && entry.Gen != handle.Gen) { if (entry.Gen > handle.Gen) return m_nullTwin; /* or a
Fatal{ProtocolCorruption} */ entry.backend.reset(); }` — and say in the comment which direction each arm is.

Related, same call, worth one line of the same fix: `EntryAt(handle.Slot)` (`:427-430`) does
`m_slots.resize(slot + 1)` on a **client-supplied** slot index with no bound. The applier's blob-bounds gate
(`c1`/`c2`) does not sit between the payload and this call for the resource family — Espryt dispatches
`ops->Create(record.Res, …)` straight through. C must bound-check `handle.Slot` before calling, and nothing
in the contract says so.

---

### m1 (minor) — the new emitted row is inserted out of order

`Coverage.def:187`. `MGP_COVERAGE_EMITTED_LIST` was strictly `LC_ALL=C`-sorted (34 rows); the new
`X(GetBoundVertexArray, BindVertexElements)` sits at position 2 instead of 4. Functionally inert —
`gen_pipe.py:819` does `sorted(set(...))` for the emitter enum and `kMGPipeFieldEmittedBy` is indexed by the
accessor list — but it is the one list in the file a reader diffs by eye.

### m2 (minor) — `MGPVertexElements::Blob`'s comment still names the frontend types

`MGPipeTypes.h:257`: `MGPBlobRef Blob; // VertexAttribute[] followed by VertexBufferBindingPoint[]`. Those
are precisely the two types that **cannot** travel (each holds a `SharedPtr<BufferObject>` — D-G2, and the
whole reason `c0` mints the wire views). The authoritative layout is now in `MGPipeValueTypes.h:552-559`
and `PipeApply.h:311-316`, and this line contradicts both. `MGPipeTypes.h` is A-only.

### m3 (minor) — two comments now name the wrong default push mask, and one of them is in a file nobody owns

- `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:112-115`: the `Fatal{PipeLegacyMemosDisabled}` message
  tells the operator "or the default 0x%llx" and prints `kMGPipeSubsystemsMigratedAtP2`. The default is now
  `0x1ff`, and `0x7f` is the *control*, so the message now advises the operator to run the A/B's off-arm.
  `C.5` marks `MG_Backend/DirectVulkan/**` untouched for every package, so no package owns the fix — the
  integrator has to.
- `ConfigLoader.cpp:11`: the include comment still reads "For `kMGPipeSubsystemsMigratedAtP2`, the push
  build's `PipePush` default" while `:256` now uses `kMGPipeSubsystemsMigratedAtP3a`.

### m4 (minor) — the three new pairing `static_assert`s are inconsistent in form

`PipeFill.cpp:688-701`. The `NewVertexElements` arm compares against
`SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements)`; the `NewVertexBuffers` and `NewIndexBuffer`
arms compare against the literal `kMGPipeSubsystemVertexInput`. Equivalent today (the first assert pins the
two together), but a later edit that re-points the emitter would leave two of the three silently comparing
against the old constant.

### m5 (minor) — `TEST(PipeCatalogue, SixValueStructsHaveFieldLists)` now asserts eight

`PipeCatalogueTest.cpp:352-362`. Correctly **not** renamed (G14 forbids it) and the comment was updated;
recorded only so the next reader does not "fix" the name.

### m6 (minor) — `MGPVertexBuffer` and `MGPVertexAttribWire` disagree on the type of `Stride` and `BindingIndex`

`MGPipeTypes.h:363-370` has `Uint32 Stride; Uint32 BindingIndex;`; `MGPipeValueTypes.h:568-582` has
`Int32 Stride; Uint8 BindingIndex;`. Pre-existing on the `MGPVertexBuffer` side and not a `c0` defect, but
the two views of the same two numbers now sit in one contract and B has to convert and bound-check in both
directions. Nothing says so anywhere.

### m7 (minor) — `PipeApply.h` has no `#if MOBILEGL_PIPE_PUSH` guard, contrary to D-K

D-K says the new declarations are "inside `PipeApply.h`'s existing `#if MOBILEGL_PIPE_PUSH` (`:30`)". There
is no such guard: `:30` is a *comment* ("Compiled only under `MOBILEGL_PIPE_PUSH` (CMakeLists.txt)") and the
push-only-ness comes from `CMakeLists.txt:482-493` appending `PipeApply.cpp` to `SOURCE_FILES` only under
push. The landed code follows the file's actual convention, which is right; the brief is what is wrong. It
matters because a pull TU that includes `PipeApply.h` **compiles** and then fails at link — see the B/C/D
list below.

---

## Dimension-by-dimension verdicts

**(1) Does every change match the B decision it claims to implement? — yes, all of them.**

- *D-A5 predicate*: `MGPipeTypes.h:654-656` is `return desc.Immutable != 0;` verbatim; `PipeCalls.def:83`
  is `kNeedsAck`; the legend sentence landed at `PipeCalls.def:19-24`.
  `PipeCatalogueTest.cpp:509-529` pins the predicate against both idioms, pins that it is the **only**
  acking call, and pins opcode 3. ✔
- *D-G2*: `MGPipeValueTypes.h:568-604`. Both structs field-for-field as the brief writes them, `IsLong`
  separate from `Type`, `Divisor` and `LegacyStride`/`LegacyPointer` absent with their reasons in place, the
  surviving "why the second view travels" argument at `:586-595`, blob layout at `:552-559`. Sizes 24 / 16
  pinned at `:623-628`. ✔ (`MGPVertexBindingPointWire` needs no `Pad0`: 8+4+4 = 16 exactly.)
- *D-H1*: `MGPipeTypes.h:374-394`. `BaseInstance` + `Pad0` between `Count` and `ContentHash`,
  `MGP_ASSERT_POD(..., 24)`, the `ContentHash` requirement, the `MGPDrawInfo::StartInstance` distinction and
  the once-per-set rule all in the comment. `MGPVertexBuffer` untouched at 32 B. ✔
- *D-M*: `MGPipe.h:84-85, 95` — bits 7 and 8, `0x1ff`, reserved comment moved to "bits 9..62",
  `kMGPipeSubsystemsMigratedAtP2` kept with the A/B rationale. ✔
- *D-A1*: `PipeApply.h:74-91`. Nine members in the brief's order and signatures; `grep -c MG_State
  PipeApply.h` = **0**, so G13's grep half is satisfied. ✔
- *D-G4*: `PipeApply.h:99-135` (both records) and `:176-210` (eleven members). `MGPipeApplierReset`
  (`PipeApply.cpp:414-427`) clears exactly those eleven and deliberately not `g_resourceOps`, which
  `ResourceEmit.TheResourceOpTableIsUnregisteredUntilABackendInstallsOne` pins. ✔

**(2) Are the fourteen stubs and the three stub emitters behaviour-neutral in push and verify? — yes, and
B2's argument is correct.**

I re-derived it rather than reading it. The residual-fill skip is `PipeFill.cpp:1243-1249`:

```cpp
const MGPipeFieldEmitter emitter = kMGPipeFieldEmittedBy[i];
const Uint64 subsystem = SubsystemForEmitter(emitter);
const Bool supplied = subsystem != 0 && (subsystem & kMGPipeWiredSubsystems) != 0 && …
```

`kMGPipeWiredSubsystems` (`:718-721`) does not carry `kMGPipeSubsystemVertexInput`, so `supplied` is false
for `GetBoundVertexArray` and `CopyField` still runs — with the bit added it would be true down the whole
chain exactly as the result file traces, so **B2 is right and adding the bits at `c0` would have been a live
bug**. `kMGPipeFieldEmittedBy` has exactly **one** consumer in the tree (`PipeFill.cpp:1243`),
`kMGPipeEmittedFieldCount` has **none**, `kMGPipeFieldEmitterNames` has **none**, and `PipeCoverage.inc` is
generated from the accessor list (unchanged), so there is no second path. `GetBoundVertexArray` is **not**
sticky (`PipeFilled.inc:173` `false`), so the row is live once wired — the count 33 → 34 is real, not
decorative.

The three stub emitters are unreachable, not merely empty: `wants()` (`PipeFill.cpp:1171-1175`) requires
`MGPipeSubsystemForDirty(bit) != 0`, and `Tracker.h:119-135`'s `default:` arm returns 0 for bits 5/9/10. The
0x1ff default reaches only `PipeFill.cpp:1170`'s `pushMask`, `Managers.cpp:241/288` (bit 5),
`MagmaPipeArms.h:50` (bit 6) and `CsoCache.h:80` (bit 63) — no reader of bits 7/8 exists, and no code
validates or rejects unknown bits. **Neutral in push and in verify.**

**(3) G1 — every reaching edit is guarded, and the 0-resize report is credible.** I checked the guards, not
only the report:

| edit | why the pull build cannot see it |
|---|---|
| `MGPipeValueTypes.h` wire structs | new **types**, no members added to an existing type; `static_assert`s only |
| `MGPipeTypes.h` `kMGPipeMaxVertexAttribs`, `MGPipeResourceRespecifyNeedsAck` | `inline constexpr` / `inline`, odr-used by nothing outside push TUs |
| `MGPVertexBuffers` 16 → 24 | every use is a *pointer* or a struct definition: `PipeTables.inc:55` (function pointer, so `gMGPipeContext`'s size is unaffected), `PipeThunks.inc:144` (inline, unused), `PipeWire.inc:369-375` (definition + `sizeof`-expression assert, never instantiated), `PipeApply.{h,cpp}` (push-only) |
| `PipeCalls.def` flag word | no generator materialises `Call.Flags` (`gen_pipe.py:155,160` consult only `kVarTail`/`kReplySlot`); `MGPWireRecHeader::Flags` is a runtime field, not a table |
| `PipeApply.{h,cpp}` | `.cpp` appended to `SOURCE_FILES` only under push (`CMakeLists.txt:482-493`); the header's only non-test includer outside `MG_Impl/Pipe` is `VulkanRenderer.h:30-34`, and that include **is** `#if MOBILEGL_PIPE_PUSH` |
| `PipeFill.cpp` | same push-only source list |
| `SlotTables.h` | the whole `BackendSlotTable` is inside `#if MOBILEGL_PIPE_PUSH` (`:80` … `:497`) |
| `PipeStats.{h,cpp}` | the enumerator, the name and the `FormatWindowLine` field are all inside the existing push block; `kCallClassNames[kCallClassCount]` stays exactly `Count` long |
| `ConfigLoader.cpp` | the changed line is inside `#if MOBILEGL_PIPE_PUSH` (`:250-257`) |
| `Config.h`, `Coverage.def`, `PipeFields.def`, tests | comments / macro definitions / non-library targets |

The one residual theoretical risk — `PipeFilled.inc`'s `inline constexpr` arrays being emitted in a pull TU
(`MGPipe.h:120` includes it unguarded) — is closed by `kMGPipeFieldEmitterNames` having zero uses tree-wide
and `kMGPipeFieldEmittedBy` keeping the same length. I did **not** rebuild the pull library; the report's
`27799 → 27799, 0 resized` is consistent with this analysis and I have no reason to doubt it.

**(4) Generated interfaces — correct and consistent.** Verified in the review worktree:
`python3 scripts/gen_pipe.py --check` rc 0 (`71 calls, 71 verify payloads, 63 fields (7 sticky, 34 emitted),
69 verbs`); a full regenerate leaves `git diff --exit-code -- MobileGL/MG_Pipe/generated` rc 0;
`--self-test` trips 7 negative controls. Exactly two `.inc` moved (`PipeFilled.inc`, `PipeVerify.inc`) —
the other six are byte-identical **because they are generated from inputs that did not change**, which is
the right reason, not a missed regeneration. `MGP_VERIFY_PAYLOAD_LIST` 69 → 71, both new entries appended
before the blank line the parser terminates on (`gen_pipe.py:222`). Both field lists name every non-`Pad`
direct member, in declaration order: `MGPVertexAttribWire` 10 of 11 (`Pad0` excluded),
`MGPVertexBindingPointWire` 3 of 3, `MGPVertexBuffers` 4 of 5. `check_field_lists_cover_struct_members`
(`gen_pipe.py:416-446`) is set-based, so the member ordering is convention rather than gate — it is
nonetheless correct here. `PipeCatalogueTest.cpp:357`'s `kMGPipeVerifiedPayloadCount == 71` is the only
consumer of that constant and was updated. ✔

**(5) `GetOrCreate(MGPipeHandle)` — never touches `MGPipeSlots()`; generation semantics half right.**
Confirmed by reading `SlotTables.h:272-286`: no `Acquire`, no `FindByLifetimeId`, no `RememberHandle`, no
`Entry::stateRef` write, and `EnsureProcessTeardownSentinel()` (`Managers.cpp:168-171`) is a
`call_once`/`atexit` pair that touches nothing. The teardown sentinel is armed as the result file claims.
The stale-generation half is M2.

**(6) PipeStats `mpr` — complete.** `PipeStats.h:116-127` (enumerator inside the push block),
`PipeStats.cpp:173-179` (name inside the same guard, array length still `kCallClassCount`),
`PipeStats.cpp:428-431` (` mpr=` beside `csom`/`csob`, inside the guard), `PipeStatsTest.cpp:159-162` and
`:283` (both pins inside the guard). ✔

**(7) Traps for B/C/D — see the list below.** No signature I checked is unimplementable as D describes it:
`MGPResourceDesc` (88 B) carries `Resource`, `Immutable`, `Usage`, `StorageFlags`, `HasDefinedContent`,
`Width`, `BindMask`, `GlNameForDiag`; `MGPVertexElements::Blob` is an `MGPBlobRef` with a `Size`, so the
applier's count-vs-blob refusal is expressible; `MGPSubData`/`MGPFlushRange`/`MGPReadback`/`MGPHandleOnly`
all carry their handle. No missing include (`PipeApply.h` reaches the wire views through
`MGPipeTypes.h:34` → `MGPipeValueTypes.h`, and both are in the enclosing `MobileGL` namespace, so the
unqualified names in `MobileGL::MG_Pipe` resolve). No ODR problem: every new header-only definition is a
type, an `inline constexpr` variable, an `inline` function or a member of a class template. No
const-correctness trap: `MGPipeGetResourceOps()` returns `const MGPipeResourceOps*` and the members are
plain function pointers. **No `static_assert` fires when B maps the dirty bits**: with bits 5/9/10 →
`kMGPipeSubsystemVertexInput` the three conditional asserts each become the equality D-M asks for.

**(8) Commit hygiene — clean.** One line, no body, no `Co-Authored-By`/`Generated-with` (`git log -1`
`---BODY---` is empty), author `Swung0x48 <swung0x48@outlook.com>`. 21 files, every one in C.0's table;
the two C.0 rows not touched are §3 D1 and C1, both argued. `[Feat] (Pipe): …` matches the house style.

---

## Judgement on each declared deviation

| # | reason | judgement |
|---|---|---|
| **A1** minted `kMGPipeMaxVertexAttribs` | **Sound.** `MG_Pipe` may not include a frontend header (closure gate A), D-G4's bare `32` is a drift hazard, and `PipeFill.cpp:702-705` is genuinely the one TU that sees both constants. The drift assert is real and unconditional. |
| **A2** plain `static_assert`, not `MGP_ASSERT_POD` | **Sound, and verified**: `MGP_ASSERT_POD` is defined at `MGPipeTypes.h:44` and `MGPipeTypes.h:34` includes `MGPipeValueTypes.h`, so the macro is genuinely unavailable there. The three-part spelling (trivially copyable + exact size + standard layout) is a superset of what the macro asserts on the two relevant axes. |
| **A3** eleven members, not ten | **Sound.** D-G4's own block declares eleven; C.0's prose miscounts the two double-declaration lines. Nothing was dropped; the reset clears all eleven. |
| **B1** three conditional pairing asserts | **Sound, and it is the strongest form available.** `Tracker.h:119-135`'s `default:` returns 0 for bits 5/9/10 at `44c2b5cf`, so a direct equality would not compile, and `Tracker.h` is B's under C.5. The `== 0 \|\|` hatch is closed by `kMGPipeWiredSubsystems` not carrying the bit — argued in place at `PipeFill.cpp:683-686`. The D-M vs C.0/C.5 contradiction is real and the integrator should correct D-M. See m4 for the one stylistic wrinkle. |
| **B2** `kMGPipeWiredSubsystems` unchanged | **Sound, and the most important call in the commit.** Re-derived independently above: with the bits added, `supplied` is true for `GetBoundVertexArray` and `PipeInputs::m_boundVertexArray` goes unwritten in every push and verify build. This is not a deviation so much as a correction of D-M. |
| **B3** stub emitters wired into step 3 | **Sound.** Unreachable via `wants()`'s `subsystem != 0` test, `return 0` keeps the payload histogram clean, and they are *called*, so no `-Wunused-function`. It genuinely reduces B's commit to a body replacement. |
| **C1** `gen_pipe.py` needed no edit | **Sound, and I verified the underlying claim**: `MGPipeValueTypes.h` is already in `FIELD_LIST_STRUCT_HEADERS` (`gen_pipe.py:211-216`), `gen_verify` is driven off `MGP_VERIFY_PAYLOAD_LIST`, and `Call.Flags` is consulted only at `gen_pipe.py:155,160` for `kVarTail`/`kReplySlot`. The corollary is also right: **D-A5's "a flag-word edit changes the generated tables" is false in this tree**, and `PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage` is what catches it instead. |
| **C2** only `GetOrCreate(handle)` added | **Sound.** `FindByHandle(MGPipeHandle)` at `SlotTables.h:294-301` already refuses a null handle, an out-of-range slot and a generation mismatch, i.e. already meets C.0's wording verbatim. (It is also, per M2, *stricter* than the new sibling.) |
| **C3** the row could not literally split | **Reason (i) sound, reason (ii) wrong — this is M1.** The structural argument (one row → one `MGPipeInputField` enumerator → one `PipeInputs` member, and the field is a single array) is correct and well made, and keeping the row out of the emitted list is right. But the substitute — "the split is therefore stated exhaustively in the rewritten comment" — is not exhaustive. |
| **C4** exactly one emitted row | **Sound, and verified**: the accessor list holds exactly three vertex-input accessors — `GetBoundVertexArray` (`Coverage.def:36`), `GetBufferBindingSlot` (`:57`) and `GetCurrentVertexAttribute` (`:67`, emitted since P2). The note to the wiring commit is the right shape (see the B/C/D list, item 3, for what it does *not* say). |
| **D1** root `CMakeLists.txt` untouched | **Sound, and verified**: the push block at `CMakeLists.txt:482-493` appends only `.cpp` files, `SOURCE_FILES` lists no headers anywhere, and D-N fixes P3a's new client files as headers. The caveat is correctly stated: a `.cpp` later would re-open A's file. |
| **D2** `kMGPipeVerifiedPayloadCount` 69 → 71 | **Sound**, and mandatory — `PipeCatalogueTest.cpp:357` is the constant's only consumer. |
| **D3** suites named without the `Test` suffix | **Sound, and load-bearing.** G6/G7 grep `VertexInputEmit\.` and the line-1488 gate greps `…\|VertexInputEmit\.\|ResourceEmit\.`; with `--no-tests=error` a `…Test` suite makes those gates *unrunnable*, i.e. a gate that can never go red. The directory convention (`RenderStateSpansTest.cpp` → suite `RenderStateSpans`) agrees. The brief's prose case names must indeed lose the `Test`. |
| **D4** own `main()` + `GTest::gtest` | **Sound.** The stated reason (a case that reads a trip wire's line back out of `MOBILEGL_LOG_FILE_PATH` must set it before anything logs) is real, and both `main()`s do exactly that and clean up. It genuinely keeps B and C out of `MG_Test/Pipe/CMakeLists.txt`. |
| **D5** the placeholders are permanent | **Sound.** G14 forbids removing a name, so a deletable placeholder would itself be a break. Both cases assert something real, and both `GTEST_SKIP()` visibly in a pull build, which keeps `ctest -N` name-identical between the pull and push trees (G2's name half). |

---

## What package B / C / D authors must know that the result file does not say

1. **`PipeApply.h` does not guard itself.** There is no `#if MOBILEGL_PIPE_PUSH` in it (m7); the push-only
   property comes from `CMakeLists.txt:482-493`. Including it from a pull TU compiles and then fails at
   link. Every call site must carry its own guard — as `VulkanRenderer.h:30-34` and both new test files do.
2. **`MGPipeResourceSubsystemEnabled()` does not exist.** D-A1 writes B's dispatch shape around it; `grep`
   finds it nowhere in the tree. B mints it (in `MG_Impl/Pipe/ResourceTracker.h`) as
   `(MG_Config::Features.PipePush & kMGPipeSubsystemResources) != 0 && MGPipeGetResourceOps() != nullptr` —
   **both halves**, or a push build with no backend registered will emit into stubs and drop the mutation.
3. **Wiring the subsystem bit and declaring the field "fully supplied" are two independent edits, and only
   one of them is a compile error.** When B adds `kMGPipeSubsystemVertexInput` to `kMGPipeWiredSubsystems`
   (`PipeFill.cpp:718`), it must in the *same* commit add `MGPipeInputField::GetBoundVertexArray` to
   `EmittedCallSuppliesTheWholeField`'s `false` list (`:742-750`) — the arm's `default: return true` means
   forgetting it is silent, and `m_boundVertexArray` stops being copied. Nothing fails to compile; the only
   gate that catches it is **G4**, at retrace time, through `EntryCompare` → `SnapshotFromGLContext`
   (`PipeFill.cpp:431-456`), which re-reads every non-sticky field in the mask.
4. **The emitters go live before the residual-fill skip does.** `wants()` keys on
   `MGPipeSubsystemForDirty` (Tracker.h, B's) while `supplied` keys on `kMGPipeWiredSubsystems`
   (PipeFill.cpp). The moment B lands the three tracker arms the three emitters start running, whether or
   not the wired mask has the bit. That is intentional (emit and keep pulling — a free A/B), but it means
   B's `b2` commit must have working emitter bodies, not stubs.
5. **The stub emitters take `GLContext&` and return `Uint64` payload bytes**
   (`PipeFill.cpp:1111-1124`), and their call sites are already in place at `:1213-1221`, after
   `EmitVertexAttribDefaults`. B replaces bodies only. Note the landed order puts attrib-defaults *before*
   vertex elements/buffers/index, where `ARCHITECTURE.md:194`'s recommended order lists them the other way;
   D-G3 says that ordering is code organisation and not contract, so this is fine — but do not "fix" it.
6. **`GetOrCreate(MGPipeHandle)` trusts `handle.Slot` and adopts a stale `handle.Gen`** (M2). C must bound
   the slot and reject a backwards generation at the call site until the overload itself is fixed. Also:
   `Find(StateObject*)` and `HandleOf()` still go through `MGPipeSlots()`; a handle-keyed kind must use
   `FindByHandle` / `GetOrCreate(handle)` only. There is **no `const` overload of `FindByHandle`**.
7. **A handle-keyed entry is invisible to `ForEachLive()`** (`SlotTables.h:373-389` skips entries whose
   `stateRef` does not lock, and the new overload never writes `stateRef`). Correct for buffers; C must not
   add a `ForEachLive`-based sweep over the buffer table.
8. **`MGPipeApplyMapPersistent` returns `nullptr` at `c0`** — a decline. C's tree branches from the tag, so
   until it rebases onto `p3a/wire`'s `c1` every persistent-map acquisition in C's own push build declines
   silently, including `TryAdoptLargeStorage`. Do not read that as an Espryt regression.
9. **`MGPipeApplySetVertexBuffers` must police `Start + Count`.** `MGPipeApplierState::VertexBuffers` is a
   flat `Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs>` (`PipeApply.h:186`) with a separate
   `VertexBufferStart`; D-H3 always emits `Start = 0`, but the contract permits otherwise and nothing bounds
   it yet.
10. **`MGPVertexBuffer` and `MGPVertexAttribWire` disagree on `Stride`'s signedness and `BindingIndex`'s
    width** (m6). `BindingIndex` is a `Uint8` on the wire view — B must reject an attribute index ≥ 256
    before narrowing, even though `MAX_VERTEX_ATTRIBS` is 32.
11. **The `.Handles` and stats itest lanes still pin `MOBILEGL_PIPE_PUSH=0x7f`**
    (`MG_IntegrationTest/CMakeLists.txt:863, 965, 975`). With the default now `0x1ff` those lanes run with
    P3a's two subsystems **off**. Package D owns that file: G12's A/B needs an explicit `0x1ff` arm (or the
    pins re-read as the control they now are), or the itest matrix has no "P3a on" lane at all.
12. **`ResourceEmit.TheResourceOpTableIsUnregisteredUntilABackendInstallsOne` asserts
    `MGPipeGetResourceOps() == nullptr` at process start.** It holds because no backend is linked into that
    unit-test target; if C ever links one in, the case fails by design. Keep the target backend-free.
13. **`g_resourceOps` is a plain, non-atomic file static** (`PipeApply.cpp:383`), read on whatever thread the
    `BufferObject` dispatcher runs on. That is exact parity with `g_bufferBackendOps`
    (`BufferObject.cpp:18`), so it is not a regression — but C must install and uninstall it at bring-up and
    teardown only, never mid-frame.
14. **`kMGPipeVerifiedPayloadCount` has exactly one consumer** (`PipeCatalogueTest.cpp:357`). Any package
    that adds a payload to `MGP_VERIFY_PAYLOAD_LIST` must bump it, and `Coverage.def`/`PipeFields.def` are
    A-only, so that means asking A.

---

*Review worktree `/home/swung/w7/p3a-review` was created at `39722687` for the generator checks and removed
with `git worktree remove --force`. No file in `/home/swung/w7/p3a-contract` was modified.*
