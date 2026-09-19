# P5d round 3, package B: persistent-map push - page-granular shadows, no edge hashing

Worktree `MobileGL-p5d-B` (branch `p5d-B`, base `85362a85`). Patch: `patches/B.patch` (5 files,
+416/-91). Not built, not committed.

## What changed (per file)

**`MG_State/GLState/BufferState/PipeResource.h`**
- `ShadowAllocationBytesFor(count)` (split-only): rounds an allocation SIZE up to a multiple of
  `SHADOW_ALLOCATION_ALIGNMENT` (4096). `MapAlignedAllocator::allocate` asks the heap for that many
  bytes under `#if MOBILEGL_BUILD_DISAGGREGATED`; the pull-build arm of `allocate` is the old text,
  token for token. Round 2 gave shadows a page-aligned BASE only; the last page could still end
  mid-way with a neighbour allocation in the remainder.
- `PipeResource::ShadowAllocationBytes()` (split-only): `ShadowAllocationBytesFor(m_shadow->capacity())`,
  0 for an adopted store or an unsized shadow. `capacity()` is exactly the count `std::vector` passed to
  `allocate()` (libc++/libstdc++/MSVC all record it), and the rounding is the allocator's own function,
  so the extent the tracker widens into can never exceed what was allocated.
- Confirmed: a live member's `MappedData()` is always `m_shadow->data()` - `Bytes()` returns the GPU map
  only when `m_gpuMapped != nullptr`, and `IsLivePersistentMap`'s second row (`IsBackendPersistentMapped`)
  excludes exactly that; nothing else backs a shadow. `ResizeShadow` is reserve(bit_ceil)+resize, so the
  allocation is `capacity()` bytes and `[0, size) ⊆ [0, capacity)`; the shadow is never reallocated while a
  map is live (Respecify/AllocateImmutableStorage call `ReleaseMemory(false)` first, which untracks;
  EnsureGpuResidentStorage/TryAdoptLargeStorage refuse while mapped).

**`MG_State/GLState/BufferState/BufferObject.h`**: `ShadowAllocationBytes()` accessor inside the existing
`#if MOBILEGL_BUILD_DISAGGREGATED` block (pull build: no text, no layout change).

**`MG_Remote/Client/PersistentMapTracker.cpp`**
- `TrackWriteMap(..., shadowExtent)`: **outward** alignment when the shadow is page-granular (base
  page-aligned, extent != 0, extent a page multiple, `rangeEnd <= extent`): `base = RoundDown(begin)`,
  `end = RoundUp(end)` clamped to `shadow + extent`; `hasEdges = base > rawBegin || end < rawEnd` is then
  false and a sub-page map is one protected page. Anything failing the test keeps the **inward** arm
  (fallback, comment rewritten to state both ownership arguments; the round-1 reasoning is kept, not
  deleted). `static_assert(SHADOW_ALLOCATION_ALIGNMENT % kPageBytes == 0)` pins the precondition.
- `InstallWriteFaultHandler`: refuses (once, `g_pageSizeRefused`) when `sysconf(_SC_PAGESIZE) != 4096`.
  Every ownership claim is made at 4 KB; on a 16 KB-page kernel (Android 15 admits them) a 4 KB
  `mprotect` covers three foreign quarters - the process-killer. Pre-existing hazard, made deterministic:
  hash arm on such kernels, never an unsafe protect.
- `PushBlocksForChecked`: both XXH3 edge hashes are gated on `tracked->hasEdges`; the `interiorBegin/End`
  comments no longer claim `>= begin / <= end`; the page loop's clamp to `[begin, end)` is unchanged and is
  what keeps a head/tail page that starts before `begin` or runs past `end` inside the mapped range.
- `PushDrawConsumers` epoch skip: walks the new `m_edgedMembers` set (members whose slot has unprotected
  edges - inward fallback only) instead of the whole membership; empty in steady state. Keys copied out
  before the walk because `PushBlocksForChecked` may `Forget()`.
- `NoteMapStateChanged` passes `buffer.ShadowAllocationBytes()` and maintains `m_edgedMembers` beside
  `m_untrackedMembers` (erase on re-note, Forget, ClearForTest).
- Test hooks: `MprotectArmAvailableForTest`, `TrackedSlotForTest`, `TrackForTest`, `UntrackForTest`,
  `FaultEpochForTest`.
- SIGSEGV handler: logic untouched. Comment on the every-overlapping-slot loop updated (no two tracked
  ranges can share a page any more; loop kept as the checkable form).

**`MG_Remote/Client/PersistentMapTracker.h`**: `TrackedWriteMap` edge-field comments (edges exist only on
the inward fallback), `m_edgedMembers`, the five test hooks.

**`MG_Test/Buffer/SplitBufferTest.cpp`**: two cases + two skip stubs (G2/G14 name-set rule, "eighteen").

## Why (profile)
Client thread at 56a77348: `XXH3_hashLong_64b_default` 6.51 + `XXH3_64bits` 2.02 = 8.5%, 98% via
`PushBlocksForChecked` from `PushDrawConsumers`; plus `PushBlocksForChecked` 5.57 self, `MappedData` 1.16,
`IsMapped` 0.58, `GetMappedRange` 0.43 around it. Cause: nearly every mapped end is not page-aligned, so
nearly every tracked map had `hasEdges` and the epoch-skip fast path hashed its edges for EVERY live map
on EVERY draw. With page-granular shadows there are no edges, the fast path touches no member, and steady
state has zero per-draw hashing and zero `MappedData/IsMapped/GetMappedRange` calls from this module.

## The ownership argument, precisely (cb06538c's danger note)
A page may be protected only if every byte of it belongs to this shadow. Outward: the heap handed the
shadow out as ONE block of `extent` bytes at a 4096-aligned base (`operator new(extent, align_val_t{4096})`,
extent a multiple of 4096); allocator metadata lives before the block, never inside; therefore
`[shadow, shadow + extent)` contains no byte of any other allocation, and every page intersecting
`[shadow+begin, shadow+end)` with `end <= extent` lies inside it. A signal-blocked driver/runtime worker
never writes a client shadow (rule E: the server reads its own staged copy). Bytes of a head/tail page
outside the mapped range are written only by the client's API paths (SubData/FlushMappedRange/Respecify
memcpy, the readback writeback landing in `WaitForApplied`'s event drain on the client thread) - threads
that run the handler; such a fault is answered like an interior one, the push clamps to `[begin, end)`,
and the byte itself crossed by its own record. `Forget`/unmap/re-note all route through `UntrackWriteMap`
(restore writable, then retire) before any reallocation, unchanged.

## Per-draw work remaining in PushDrawConsumers, epoch unmoved
| step | cost |
|---|---|
| `PersistentMapTracker::Instance()` (3 call sites in EmitTables) | function-static guard load |
| `PushIsArmed()` | one `MG_Config::Transport` load + compare |
| `OnServerRole()` | `ServerLoop::OnApplyThread()` (package D makes it a relaxed load + compare) |
| `m_livePersistentMaps.empty()` | one load |
| `g_faultEpoch.load(acquire)` + compare with `m_lastFaultEpoch` | one atomic load |
| `m_untrackedMembers.empty()`, `m_edgedMembers.empty()` | two loads |
| per member | nothing |
First push after a map still ships everything once (all bits set at registration; first fault moves the
epoch, the walk pushes every set bit). The loop was kept (over `m_edgedMembers`, not the membership) because
the inward fallback still exists; it is not entered in steady state.

## Red-once
- `SplitBufferSet.ASubPageMapIsOneProtectedPageAndAWriteThroughItFaults`: reverting the outward alignment
  gives `pageCount == 0`, `hasEdges == true`, and a write through the pointer that does not move the fault
  epoch - three red assertions. Reverting the allocator rounding alone: `ShadowAllocationBytes() % 4096`
  fails for a 100-byte buffer (capacity 128).
- `SplitBufferSet.AShadowThatFailsThePageGranularTestKeepsTheInwardAlignmentAndItsEdges`: an unaligned base,
  an unknown extent (0) and a range past its extent each register inward (1 page, edges); the true extent
  widens to 3 pages, no edges, end == extent. Widening without the extent test goes red on the third
  registration.
- Existing: `ThePushCutsTheMappedSpanIntoBlocksAndMovesPmap`, `TheLastBlockIsTheRemainderAndNotAWholeBlock`
  (64K+7: now one page-rounded span, still 2 blocks / kSize bytes by the clamp), `PersistentCoherentMapScenario`
  (pixels through the push) keep passing by inspection.

## G1 / monolith
Pull build (`MOBILEGL_BUILD_DISAGGREGATED=OFF`): zero delta. `ShadowAllocationBytesFor`,
`PipeResource::ShadowAllocationBytes`, `BufferObject::ShadowAllocationBytes` exist only under the option;
`allocate()`'s pull arm is the previous text verbatim. Push monolith build with the option ON but
`Transport=Monolith`: allocations of shadows/staging stores round up to 4096 multiples (invisible to the
application; capacities >= 4096 are already powers of two, so only sub-4K stores grow), tracker inert as
before (`PushIsArmed` false).

## Risks / not done
- Not built (per rules). Watch for: `sysconf`/`unistd.h` already included; `uintptr_t` in the test via
  `<cstdint>` through the tracker header; `flat_hash_map::value_type::first` for `m_edgedMembers`.
- The 16 KB-page refusal is new behaviour on such kernels (hash arm instead of mprotect); on the Redmi
  (4 KB) it is a no-op. It is the honest answer, but it is a performance cliff there rather than a fix.
- Two tracked shadows can no longer share a page; the handler still scans every slot per fault (unchanged).
- Not touched: role guards (`RefuseLegacyBufferArmFromApplyThread`, `OnServerRole`) - package D; the hash
  arm's `MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS` XXH3 for untracked members - still exists for table-full /
  handler-unavailable / >256 MB maps; docs (`P5D-INPROC-PERFORMANCE.md`) not updated - integrator's row.

## Fix round

Patch regenerated from `MobileGL-p5d-B` (6 files, +654/-149; BufferObject.cpp is new to the set). Not built.

**Blocker (both reviews): the epoch skip lost the drain the old edge service did by accident.**
Agreed, and the diagnosis is exact: `m_lastFaultEpoch = epoch` before a walk that pushed only
consumers left a faulted non-consumer marked, writable and unable to move the epoch again. Fix, in
`PersistentMapTracker.cpp`:
- The walk now DRAINS every tracked member with a marked page, consumer or not, before the binding
  walk (`AnyPageMarked`, one word load per 64 pages, walk-time only). The skip's proof then holds:
  the epoch moves only on a mark, and the last walk drained every mark there was. A drained push is
  bitmap-driven (faulted blocks only), so nothing pays the whole-range rescan the consumer filter
  exists to avoid. `m_lastFaultEpoch` is still taken before the drain, so a fault landing mid-drain
  forces the next draw to walk again.
- `TrackWriteMap` bumps the epoch after publishing a slot: a fresh slot has every page marked, which
  is a fault of every page as far as the push is concerned. This closes the second review's
  "registered and drawn without a fault is never shipped" hole - "first push ships everything once"
  is now true of the draw path, not only of the read-only verbs' whole push.
- The handler's `g_faultEpoch.fetch_add` is now `release` (was relaxed) so the walk's existing
  `acquire` load actually orders the bit before the epoch across threads (chunk builders write
  arenas off the GL thread). Cheap: handler-only.
- Skip path unchanged in cost: one atomic load, two empties, nothing per member. The per-draw table
  above still stands. The walk (epoch moved) gained one word-scan per tracked member.
- The binding walk moved under `if (ctx != nullptr)` so the drain runs without a context too; the
  walk body is re-indented, otherwise unchanged.

**Major (review 1): no red-once for the skip semantic.** Added
`SplitBufferSet.AFaultedMapTheDrawDidNotBindStillShipsBeforeTheDrawThatDoes` (+ skip stub; name
sets 20/20, "TWENTY"). Two one-page maps A and B, B bound to SSBO point 0, driven through
`PushDrawConsumers` itself: draw 1 ships both (2 blocks: fresh A ships although unbound - the
registration bump), draw 2 ships nothing, a write through A's pointer faults (asserted via the
epoch), draw 3 with only B bound must ship A's block (3), draw 4 binding A ships nothing more (3,
`BytesPushed == 3 * 4096`). Reverting the drain: draw 3 stays at 2 and draw 4 skips - red at the
"3u" assertion. Reverting the registration bump alone: draw 1 stays at 0 - red at "2u".

**Minor (review 1): `g_segvInstalled` published before the page-size check.** The check is now
`KernelPageIsTheTrackerPage()`, a function-static memo asked BEFORE the CAS; `g_pageSizeRefused`
is gone. No caller can pass `TrackWriteMap`'s installed gate while the size is undecided.

**Minor (review 1): `ShadowAllocationBytes()` unguarded.** Now out of line in `BufferObject.cpp`
(under the option) with `RefuseLegacyBufferArmFromApplyThread("ShadowAllocationBytes")`, beside
`MappedData`. Pull build: no text (declaration and definition are both under the option).

**Minor (review 1): outward can exceed `kMaxTrackedPages` by one page where inward fit.** Widening
never demotes: if the outward count exceeds the capacity, `TrackWriteMap` takes the inward arm for
that map (edges, `m_edgedMembers`) instead of refusing it onto the hash arm, so no map that was
tracked before this round becomes an untracked member that vetoes the skip process-wide. Only a
~256 MB map on the boundary pays the old edge-hash cost; no test (it would need a 256 MB shadow).

**Minor (review 2): extent inferred from `capacity()`.** `PipeResource` now records
`m_shadowExtent = ShadowAllocationBytesFor(reserved)` in `ResizeShadow` (the count it asked the
allocator for) and zeroes it in `AdoptPersistentMap`; `ShadowAllocationBytes()` returns it (0 while
GPU-resident). When the reserve did not reallocate, the block behind `Bytes()` is a larger earlier
reserve's, so the recorded value is at most the true extent in every case; `size <= reserved`
keeps `rangeEnd <= extent`. `Shadow()` has no callers, and every shadow allocation goes through
`ResizeShadow`, so nothing else can move the block. Split-only member (G1 layout unchanged).

**Minor (review 2): the `hasEdges &&` conjuncts were dead.** Dropped both; the comment now says
the two geometric conditions are self-gating under outward alignment and that this, not a flag,
is what removed the per-draw XXH3. `hasEdges` stays for `m_edgedMembers` (header comment updated).

**Not changed:** the 16 KB refusal, the outward arithmetic, the allocator rounding, the SIGSEGV
handler's ownership tests, `PushAllMembers`. Risks: not built - watch `if (const auto& ctx =
MG_State::pGLContext)` (UniquePtr in an if-declaration, same shape as the `vao` line below it),
`entry.first` on `UnorderedMap<Uint64, MemberEntry>`, and `MGLOG_W` inside the function-static
lambda.
