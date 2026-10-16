# P5d round 3, package D - role guards and TLS made nearly free

Worktree `MobileGL-p5d-D`, branch `p5d-D`, base `85362a85`. Not built, not committed, not pushed.
Patch `scratchpad/p5d/patches/D.patch` (618 lines, 11 files). **What changed, per file:**
### `MG_Remote/Server/ServerLoop.{h,cpp}` - item 1, the point of the package
- New `Server::Detail::g_applyThreadKey` (`inline std::atomic<Uint64>`, header) + `Detail::CurrentThreadKey()`.
  On aarch64 (`__has_builtin(__builtin_thread_pointer)`) the key is the thread pointer - one
  `mrs TPIDR_EL0`, no call; elsewhere `std::this_thread::get_id()` memcpy'd into a `Uint64`. x86_64
  deliberately stays on the fallback: the builtin only grew x86 support in clang 17 / GCC 11, and older
  toolchains accept `__has_builtin` for it and then fail in codegen - a build break on the desktop
  ctest lane for a target nobody profiles.
- `OnApplyThread()` is now **inline in the header**: `key != 0 && key == CurrentThreadKey()`. Gone:
  `ServerLoopInstance()`'s function-static guard byte, the acquire loads of `m_running` and of
  `atomic<std::thread::id>`, `std::this_thread::get_id()`, the out-of-line `pthread_equal`, and the
  cross-library PLT hop for every caller that includes the header.
- `m_applyThreadId` is **deleted**. The key is published as `ApplyThreadMain`'s first statement and
  cleared **inside the same `m_controlMutex` critical section that clears `m_running`** (the C2 exit
  block), so the two move together and the "live but exiting apply thread" window is byte-for-byte the
  old one. `Stop()` no longer clears an identity after the join.
- Relaxed on both sides. The one dangerous shape - a new thread reusing the exited apply thread's TLS
  block (hence its thread pointer) while still reading the pre-clear key - cannot happen: the clear is
  sequenced before that thread's exit, the exit releases the TLS block under libc's own lock and the
  receiving thread takes the same lock, a happens-before chain a relaxed load must respect by
  coherence. A stale *zero* is harmless. The argument is written out at the declaration.
### `MG_Impl/Pipe/SlotAllocator.{h,cpp}` and `MG_State/.../MipmapStorage.{h,cpp}` - item 2
`g_reverseAnnouncementScopeDepth`, `g_frontendKeyedRegistryScopeDepth` and
`g_textureLegacyArmScopeDepth` stop being `thread_local`. The three scopes increment/decrement only
when `OnApplyThread()` is true, each keeping that decision in a new `Bool m_counted` so the destructor
undoes exactly what the constructor did (re-asking would leak a count across the apply thread's exit).
**Single-writer proof, in the files:** each counter's only reader in the tree is its apply-thread
guard - `MGPipeRefuseAllocatorFromApplyThread` (`SlotAllocator.cpp:30-31`),
`RefuseLegacyTextureArmFromApplyThread` (`MipmapStorage.cpp:44`) - and both return before they look
unless the transport is active **and** `OnApplyThread()`; I grepped every `::Active()` call site and
those three are all of them. So the apply thread is sole writer and sole reader, and a monolith process
never reads them. `Active()` on the apply thread is unchanged; off it, it is now always false, and both
headers say a future reader has to account for that.
### `MG_State/GLState/BufferState/BufferObject.cpp` - item 3
The Fatal body is split out as `FatalLegacyBufferArmFromApplyThread` (`[[noreturn]]`).
`SyncPersistentMappedRange` called `RefuseLegacyBufferArmFromApplyThread` and then re-asked
`PushIsArmed()`/`OnServerRole()` for an early-out whose condition was *identical* - so its `return` was
unreachable, the guard having already aborted. It now asks once; no accessor makes more than one
`OnApplyThread()` call. `PushIsArmed()` is a read of `MG_Config::Transport`; left alone.
### `MG_Remote/Client/ClientSession.cpp` - item 3
`RefusePipeInputsTouchWhileApplierOwnsIt` reordered cheapest-first: `Transport == Monolith`,
`Ipc.BatchWaits != 0`, then `OnApplyThread()`, `Started()`, `BarrierArmed()`,
`ApplyThreadIsInsideApplier()`, and the `thread_local` `InBarrierWait()` **last**. The chain is a pure
conjunction (every row returns "no violation"), so which touches abort is unchanged; only the cost of
answering "no" moves. With the shipping default `MOBILEGL_IPC_BATCH_WAITS=1` it is two loads on a path
`MGPipeValidateForVerb` reaches per verb (PipeFill.cpp:542, :1951, :2789).
### `MG_Remote/Client/WireTables.cpp`, `.../PersistentMapTracker.cpp`
Comments only. `RunsAsTheServerRole()` and `PersistentMapTracker::OnServerRole()` stay out of line
**on purpose**: inlining them would put a server header inside `WireTables.h` (whose comment says it
exists so PipeFill need not include one) and inside `PersistentMapTracker.h` (which MG_State's
`BufferObject.cpp` includes). One PLT hop is that boundary's price; the four-part predicate behind it
- the measured cost - is gone either way. `IsLivePersistentMap`'s row order is *not* touched: it is
contractually the same chain as `SyncPersistentMappedRange`'s, and item 1 already made its one guarded
accessor (`IsMapped`) cheaper than the reorder would have.

## Why (profile at head 56a77348)
`OnApplyThread()` 2.66% self / 3.64% incl **on the client thread** for an answer that is "no" every
time, plus `@plt` 6.1% self on the apply thread (item 1). `__emutls_get_address` 7.7% and the **top
symbol** on the monolith GL thread, 7.4% on the apply thread, 32.7% of it exactly these scopes
(FrontendKeyedRegistry ctor 18.75 + dtor 13.95, ReverseAnnouncement 8.4), built at ~20 sites in
DirectGLES.cpp including inside `StateBackendObjectRegistry::HandleOf` per draw (item 2). Every cycle
cut on the apply thread shortens the client's R-1 wait one-for-one, so both pay on both threads.

## Red-once
- **Item 1**: `MG_Test/Wire/ServerLoopTest.cpp:291-324` - a posted control request records
  `OnApplyThread()` inside itself (`EXPECT_TRUE`) and the test thread asserts `EXPECT_FALSE` after;
  publish the key late, clear it early or compare the wrong identity and it goes red. Backed by the 5
  `MGL_BUFFER_GUARD_TEST` + 9 `MGL_TEXTURE_GUARD_TEST` cases (`RemoteClientTest.cpp:1841-1912`), each
  needing `OnApplyThread()` true inside `RunOnApplyThread`.
- **Item 2**: three cases **added** to `RemoteClientTest.cpp` after the buffer guard block:
  `AnAllocatorProbeFromTheApplyThreadOutsideEveryScopeIsFatalByName` (the control),
  `AnAllocatorProbeInsideAnExemptionScopeOnTheApplyThreadIsAllowed` (red if the increment is dropped)
  and `AnExemptionScopeHeldOnTheGLThreadDoesNotExemptTheApplyThread` - **the one that pins the
  single-writer argument**: the scope is open on the GL thread across the posted request and the apply
  thread's probe must still abort; make the counter unconditional again and it goes green while a real
  violation is silently exempted.
- **Item 3, ClientSession**: pure reorder, no new test; `ClientPipeInputsFillWhileTheApplierOwnsIt
  IsFatalByName` (batch off -> Fatal), `...IsAllowedWhenBatchingIsOn` and
  `...WithTheApplierIdleIsAllowed` already drive both sides of the row I moved to the front.
  **BufferObject**: `BufferSyncPersistentMappedRangeFromTheApplyThreadIsFatalByName` still demands
  `Fatal{RoleViolation, "buffer-legacy-arm"}` from that path.

## G1 / monolith impact
**G1 = 0/0/0/0.** Nothing reaches the pull build: MG_Remote and `MG_Test/Wire` are not compiled there
(`MG_Test/CMakeLists.txt:105`), `SlotAllocator.cpp` is a `MOBILEGL_PIPE_PUSH` file (disagg forces push
on, `CMakeLists.txt:496-500`), and every hunk in `BufferObject.cpp`, `MipmapStorage.h` and
`MipmapStorage.cpp` - the new `m_counted` member included - sits inside `#if MOBILEGL_BUILD_DISAGGREGATED`.
**Push build under `Transport=Monolith`**: identical by inspection - no `ServerLoop` runs, so the key
is 0 and `OnApplyThread()` is false exactly as the old predicate was; the scopes count nothing and
`Active()` is false, which the only reader never reaches (first line returns on Monolith). G14: three
names added, none removed. G2: the added names sit in a disagg-only suite beside names already there.

## Risks
1. `__builtin_thread_pointer` is the one new intrinsic; if unavailable the `#if` falls back and only
   the speedup is lost. **Not compiled** (no-build rule), so this is inspection only.
2. The relaxed-load recycling argument leans on libc synchronising TLS-block reuse; it is written out
   at the declaration so a reviewer can reject it, and an acquire load would cost one `ldar` and keep
   every other saving. 3. `Active()` is now always false off the apply thread - harmless today (one
   reader, proven), latent for a future second reader; both headers say so.

## What I did not do (item 4 inventory)
- `g_inBarrierWait` (`ClientSession.cpp:117`) - ~900+ set/clear pairs per frame. **Must stay TLS** (the
  file's note: a second GL thread would read the first one's wait as its own), so single-writer fails;
  only mitigated by moving its read to the end of the guard chain.
- `t_activeDecoder` (`PipeWireCodec.cpp:359`; read :1289/:1292, saved/restored :1385-1393) - per
  applied record, so it *is* hot. **Not changed**: its comment says a unit suite drives several
  decoders from several test threads, so the apply thread is not its only writer.
- `g_reply` (`PipeRoute.cpp:61`) - the monolith adapter's one-deep reply mailbox, per reply-slot row
  rather than per draw, and per-thread by design (R-1 bounds the in-flight depth per thread). **Kept.**
- Cold, off the per-draw path: `tl_initializing` (`Init.cpp:31`), `tl_isPoolThread`
  (`ShaderCompilePool.cpp:30`), `t_current` (GLX/WGL), the `static thread_local Vector<GLuint> names`
  scratch buffers (`GL_ProgramPipeline.cpp:74,83`, `GL_Sampler.cpp:309,342`), `paddedUbo`
  (`UniformManager.cpp:2072`, DirectVulkan); `DirectGLES.cpp:12828` already says why the owner stamp is
  not thread_local.
- Also not done: inlining `RunsAsTheServerRole` / `OnServerRole` (layering), reordering
  `IsLivePersistentMap` (contract) - both argued in the code comments. No build, no ctest, no device
  run (the brief forbids all three): every claim is by inspection, no number was re-measured.

## Fix round

Both reviewers returned `apply-with-minors`, no blockers. Every major and every minor below is
addressed in code except the two that are explicitly integration notes. Still not built, not
committed, not pushed; no attribution lines. Patch regenerated (770 lines, 12 files).

### MAJOR (both reviewers) - the key clear at the `ServerLoop.cpp` C2 exit had no red-once. **Fixed.**
Both reviewers made the same argument and it is correct: the old predicate was
`m_running && id == mine`, so an uncleared identity was harmless by construction; the new one has no
second term, and delete-the-store left every test in the tree green. New case in
`MG_Test/Wire/ServerLoopTest.cpp:829` (right after the C2 `AForwarderCallAfterTheLoopStopped...`
case), `AThreadCreatedAfterTheLoopStoppedIsNotMistakenForTheApplyThread`, in three parts:
- `ASSERT_NE(Detail::g_applyThreadKey, 0)` while the loop is parked, so the post-Stop `EXPECT_EQ`
  cannot pass for the wrong reason (a key that was never published is also 0);
- `EXPECT_EQ(Detail::g_applyThreadKey, 0)` after `fixture.Stop()` - reviewer 1's deterministic half;
  it pins the **presence** of the store on every platform;
- reviewer 2's half, which pins **why it matters**: 8 sequentially created-and-joined
  `std::thread`s, each asserting `ServerLoop::OnApplyThread()` false. With the store removed the
  first thread to inherit the exited apply thread's TLS block (hence its thread pointer) answers
  true and the case goes red for its own reason. It is second, not alone, because TLS-block reuse is
  likely, not guaranteed - the case says so in its own comment.

**Position** (reviewer 1's second half). I did not add a `Detach()`-time hook: it would put a third D
hunk into `ServerLoop.cpp` and deepen the overlap with package T, for a property no unit case can
reach anyway. Instead the C2 comment now names the hazard concretely (`ServerLoop.cpp:458-472`):
`ServerLoopTest` has no GL context, so `CreateBackend` never runs and the
`~BackendObject_DirectGLES` -> `ReleaseEGLResources` self-post does not exist here; move the store
above `m_backend.reset()` and that post misses `RunOnApplyThread`'s re-entrancy arm
(`ServerLoop.cpp:576`, `if (OnApplyThread()) return work(user);`) while `m_running` is still true,
publishes into the one-slot mailbox and waits on `m_controlDone` for an answer only that thread could
give. The comment says in as many words: do not move this on the strength of a green ctest.

### MINOR (both) - the `std::thread::id` fallback copies an object representation with no padding check. **Fixed.**
`ServerLoop.h:178`: added `static_assert(std::has_unique_object_representations_v<std::thread::id>)`
beside the size assert, plus `#include <type_traits>`. I chose this over
`std::hash<std::thread::id>{}(id) | 1u`: the hash is well-defined, but `| 1` is injective only
because today's `pthread_t` happens to be pointer-aligned, and on MSVC (an FNV hash of the id) it
would fold two distinct threads onto one key with no diagnostic - a guard answering **true off the
apply thread**, which is worse than the failure being fixed. A padding implementation is now a build
error, which is the loud-not-silent direction CONTRACT-P5C rule E asks for. The comment beside it
names the concrete consequence both reviewers derived (lost re-entrancy shortcut -> `m_controlDone`
deadlock; exemption scopes stop counting -> `MGPipeRefuseAllocatorFromApplyThread` aborts at the
per-draw `HandleOf` probes).

### MINOR (reviewer 2) - the selector macro is unguarded, never `#undef`'d and not forceable from the build. **Fixed.**
`MOBILEGL_SERVER_APPLY_THREAD_POINTER` -> `MOBILEGL_DETAIL_APPLY_THREAD_POINTER`
(`ServerLoop.h:150-161`): `#ifndef`-guarded, always **defined** (to 0 when unavailable) and tested
with `#if`, not `#ifdef`. `-DMOBILEGL_DETAIL_APPLY_THREAD_POINTER=0` now really disables the builtin
path - the escape hatch for risk 1 the package named itself - and no longer collides with the
header's own define. The nested `#if defined(__aarch64__) && defined(__has_builtin)` /
`#if __has_builtin(...)` shape is kept (GCC < 10 must not evaluate the inner line).

### MINOR (reviewer 2) - `Active()` is silently thread-conditional in public headers. **Fixed by renaming.**
`MGPipeReverseAnnouncementScope::Active()`, `MGPipeFrontendKeyedRegistryScope::Active()` and
`MGPipeTextureLegacyArmScope::Active()` are now `ActiveOnApplyThread()` (declarations
`SlotAllocator.h:205`, `:229`, `MipmapStorage.h:70`; definitions `SlotAllocator.cpp:83`, `:101`,
`MipmapStorage.cpp:94`; the three readers `SlotAllocator.cpp:28-29`, `MipmapStorage.cpp:44`; the
comment at `RemoteClientTest.cpp:1861`). I re-grepped the tree myself: those are all of them, so this
is a closed rename with no external caller. Chose the rename over an `MGLOG_E_ONCE`/assert inside the
query because the guard reads it on the apply thread on the per-draw path, where a diagnostic is
either free and useless or costly and hot; a name that carries the precondition costs nothing and
cannot be ignored by the future P4b/P7 reader the finding is about. Both headers' comments now say
why the name is spelled that way.

### MINOR (reviewer 1) - `docs/Disaggregated/ROADMAP.md:69`'s G4 citation went stale. **Fixed, but OUTSIDE `D.patch`.**
Confirmed: `MobileGLResult ServerLoop::RunOnApplyThread(` is at `:543` at baseline `85362a85` and at
`:570` in this worktree (the fix round's C2 comment moved it a further 15 lines), and the old `:598`
end maps line-for-line to `:625`. The cell now reads `ServerLoop.cpp:570-625`. **`ROADMAP.md` is not under `MobileGL/`**, so the mandated
`git diff HEAD -- MobileGL` cannot carry it; it is delivered as a separate one-line patch beside it,
`scratchpad/p5d/patches/D-docs.patch`. Integrator: apply both.
`python scripts/check_doc_citations.py docs/Disaggregated/ROADMAP.md` -> 56 citations, 0 problems.
Note for the integrator: package T also moves `RunOnApplyThread`, so this cell needs re-checking once
T lands - the gate will not catch it (it only resolves the file and counts lines).

### MINOR (both) - D's `ServerLoop.cpp` hunk overlaps the C2 block package T rewrites. **No code change; integration note, as both reviewers asked.**
Accepted as stated, and I deliberately did **not** shrink the hunk: the key clear has to be inside
the same `lock_guard(m_controlMutex)` critical section as `m_running.store(false)`, or the window in
which a live-but-exiting apply thread answers true moves - which is the property item 1's equivalence
argument rests on. Land **D before T**, or rebase T onto D, and at the merge check that T's
`m_controlPosted` clear and D's `Detail::g_applyThreadKey.store(0)` both end up inside that same
section beside `m_running.store(false)`. The new ServerLoopTest case above is what makes a merge that
drops the clear fail loudly instead of landing green. The `ServerLoop.h` overlap (D prepends the key
block, T adds park counters) is textual only - different regions of the file.

### Findings I did not act on
None outstanding, and nothing in either review was disputed. The two items left uncoded are the two
both reviewers themselves scoped as "flag it to the integrator" (overlap ordering) and the one the
patch boundary cannot carry (ROADMAP, delivered separately).

### Re-verification after the fix round
- `python scripts/check_include_closure.py` -> 4 probes, 0 skipped, 0 problems (unchanged; the rename
  and the `<type_traits>` include add no header->header edge).
- `python scripts/check_doc_citations.py docs/Disaggregated/ROADMAP.md` -> 0 problems.
- Doc-citation sweep over every file this package touches: the four `ClientSession.cpp` citations in
  `ROADMAP.md` / `CONTRACT-P5C.md` are all at or below `:880` and both D hunks start at `:996`, so
  none moved. No document names `Scope::Active()`, so the rename breaks no citation.
- G1 unchanged and still 0/0/0/0: the new test is in `MG_Test/Wire` (not in the pull build), the
  rename and the new comments are all inside `#if MOBILEGL_BUILD_DISAGGREGATED`, and `<type_traits>`
  enters only `MG_Remote/Server/ServerLoop.h`, which the pull build does not compile.
- Still no build, no ctest, no device run (the brief forbids all three). Every claim here is by
  inspection or by a read-only script.
