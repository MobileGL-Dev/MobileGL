# P5d round 3 brief: inproc performance, the profile-directed cuts

Repo: MobileGL (branch `feat/disaggregated`, head `85362a85`). You work in an isolated git worktree
of it (your cwd). Read `docs/Disaggregated/P5D-INPROC-PERFORMANCE.md` (rounds 1-2), the relevant
parts of `MobileGL/MG_Remote/CONTRACT-P5C.md` (role guards, rule E) and `docs/Disaggregated/ROADMAP.md`
"通用纪律" before touching code.

## Why this round exists (measured, not guessed)

Workload: FCL + Minecraft 26.3-rc-3, view distance 12, Redmi 2f7cbe2e (Adreno 830), DirectGLES.
Split (`MOBILEGL_TRANSPORT=inproc`) 63-65 fps vs monolith 110-118 fps, ~852 draws/frame.
simpleperf (cpu-cycles, whole process, 15 s in world) at head 56a77348, symbolized:

Process split: GL/client thread (`Thread-17`) 39.5% of cycles, apply thread (`mgl-srv-apply`) 36.9%.
Monolith: GL thread 52.6% of a 39% smaller total.

### Apply thread (mgl-srv-apply), self %
- `std::mutex::unlock` 13.6, `std::mutex::lock` 12.4, `pthread_mutex_lock` 12.1, `pthread_mutex_unlock` 11.0
  -> ~49% of the thread. Call graph: 58-60% of `mutex::lock/unlock` samples have
  `ServerLoop::ApplyThreadMain()` as the DIRECT caller; 99.8% of `pthread_mutex_lock` via `mutex::lock`
  is ApplyThreadMain. These are NOT the backend `pendingMutex` sites the round-2 report guessed.
  They are the idle poll: `for(;;){ PumpControlRequest(); DrainRing(); if(ready()) continue; bell.Wait(...) }`
  where `PumpControlRequest()` takes `m_controlMutex` every iteration and the `ready` lambda calls
  `ControlIsPending()` which ALSO takes `m_controlMutex` (ServerLoop.cpp:371-384, 452-475).
- `ApplyThreadMain` self 7.95, `@plt` 6.1 (31% of plt from ApplyThreadMain), `__emutls_get_address` 7.4
- Inclusive: `ApplyThreadMain` 78.4%, `DrainRing` only 29.0% (= the real apply work: OnDrawVbo 22.7%,
  PrepareForDraw 16.3%, SyncNeccessaryTextures 8.6%, SyncMipmapsToBackend 3.3%, HandleOf<ITextureObject> 3.0%,
  `MGPipeRefuseAllocatorFromApplyThread` 2.4% incl.)
- `MGPipeServerStampVerbBoundary` 1.1 self (63-field loop per verb, PipeInputs.cpp:103-130)
- `__aarch64_swp2_rel`/`cas2_acq`/`ldadd8` ~2.5 (outlined LSE atomics; leave alone)

### Client thread (Thread-17), self % / inclusive %
- `SessionProducer::WaitForApplied` 27.9 self / 34.2 incl (the R-1 barrier spin; `EmitAndWaitTails` 36.3 incl)
- persistent-map push: `PushDrawConsumers` 1.96 self / 22.5 incl; `PushBlocksForChecked` 5.57 self / 21.5 incl;
  `XXH3_hashLong_64b_default` 6.51 + `XXH3_64bits` 2.02 (callers: `PushBlocksForChecked` via PushDrawConsumers 98%);
  `BufferObject::MappedData()` 1.16 self / 3.86 incl; `IsMapped` 0.58; `GetMappedRange` 0.43; `PushIsArmed` 0.47
- `MGPipeValidateForVerb` 4.11 self / 10.9 incl (65% called from `DrawElementsInstancedBaseVertex_Backend`);
  inside it `MGPipeTracker::Update` 1.46, `MGPipeFillAccess::CopyField` 0.64 self / 2.54 incl
  (45% of `BufferState::GetBindingSlot` 1.73 comes from CopyField in the draw fill)
- clock: `steady_clock::now()` 2.34 self / 5.91 incl, `__kernel_clock_gettime` 2.37, `clock_gettime` 1.94
  -> ~6.6% of the thread is reading the clock. `Doorbell::Wait` (Doorbell.h:123-178) reads the clock at
  least twice per wait (start + spinEnd) and once per 64 spins; ~900+ waits/frame at 64 fps.
- `ServerLoop::OnApplyThread()` 2.66 self / 3.64 incl on the CLIENT thread (role guards on hot paths:
  `RefuseLegacyBufferArmFromApplyThread` in `BufferObject::MappedData/IsMapped/...`,
  `PersistentMapTracker::OnServerRole`, `RefusePipeInputsTouchWhileApplierOwnsIt`, `RunsAsTheServerRole`,
  `MGPipeRefuseAllocatorFromApplyThread`). `OnApplyThread` = `ServerLoopInstance()` (function-static guard)
  + `m_running` atomic + `m_applyThreadId` atomic<std::thread::id> + `std::this_thread::get_id()` +
  thread::id compare (pthread_equal call). `pthread_self` shows 0.56 self.
- `MarkWritableImageBufferTextures` 2.15 self / 3.59 incl (GpuWritePending.cpp:85-101: sweeps all
  `MAX_TEXTURE_IMAGE_UNITS` image units per draw; `GetImageTextureBinding` 1.37)
- `__emutls_get_address` 0.46 client; on MONOLITH's GL thread it is 7.7% (top symbol!) with callers
  `MGPipeFrontendKeyedRegistryScope` ctor 18.75% / dtor 13.95%, `MGPipeReverseAnnouncementScope` ctor/dtor 8.4%
  (SlotAllocator.cpp:41-75: `thread_local Uint32` depth counters; the scope is constructed at ~20 sites in
  DirectGLES.cpp, e.g. inside `StateBackendObjectRegistry::HandleOf`). On the apply thread emutls is 7.4%.

### The shape of the frame (why the client-side cuts matter)
Lockstep (R-1): every kCtxVerb (draw/clear/blit/dispatch/readback) and every reply-slot row waits for
`appliedSeq == seq`. Frame time ~= client work + server apply time (waited for) + handoff latency.
Nothing in this round removes the barrier (that is P11's versioned inputs / P3b-P4b twin tables). This
round removes overhead on BOTH threads: every cycle cut on the apply thread's real work shortens the
client's wait one-for-one; every cycle cut on the client thread shortens the frame directly.

## Packages (one agent each, independent, non-overlapping files where possible)

### T - transport: apply loop idle poll, wait spin, park counters
Files: `MobileGL/MG_Remote/Server/ServerLoop.{h,cpp}`, `MobileGL/MG_Remote/Transport/Doorbell.{h,cpp}`,
`MobileGL/MG_Remote/Transport/Ring.cpp`, `SessionRings.h`, `MobileGL/MG_Remote/Client/ClientSession.cpp`,
`MobileGL/MG_Util/PipeStats*` (find the "MGPipe stats:" line producer).
1. `ControlIsPending()` and `PumpControlRequest()` must not take `m_controlMutex` when nothing is posted:
   add `std::atomic<Bool> m_controlPosted` (set release under the lock by the poster in
   `RunOnApplyThread`, cleared by the pump under the lock when it takes the request). The mailbox
   handshake semantics (C2 critical section, NOT_INITIALIZED answer at exit, kMaxControlFramesPerPump)
   must stay exactly as documented in the file. The `ready` lambda reads only atomics.
2. `DrainRing()` on an empty ring must be a couple of loads, no lock, no clock. Check.
3. `Doorbell::Wait`: the spin phase must not read the clock at all in the common case. Replace
   "spin until steady_clock passes spinEnd, clock read every 64 iterations" with a calibrated iteration
   budget: once per process (or per Doorbell), measure how many `ready()`+`CpuRelax()` iterations fit in
   `spinUs` (or simply how many CpuRelax fit in 1 us) and spin that many iterations clock-free; only the
   park phase (rare) computes deadlines. Keep `timeoutMs`/`kWaitForever` semantics, the parked-flag fence
   protocol and the Dead() handling byte-for-byte in meaning. Document the overshoot bound.
   `WaitForApplied`'s common path (server not yet there) currently pays 2+ clock reads per wait; after
   this it pays zero unless it parks.
4. Counters for the measurement: server-side `m_parks` already exists (`ParkCount()`); add a client-side
   park counter (SessionProducer::Park -> count parks that reach `Doorbell::Park`) and a wait counter,
   and print both (server parks, client parks, waits) in the periodic `MGPipe stats:` line (find how the
   line is assembled; keep it one line; add a `wait[...]` group). If the stats line producer lives in a
   place that cannot see MG_Remote symbols, expose the counters through a small accessor registered by
   the client session (no new include of MG_Remote into MG_Util if that violates the include-closure
   gate `scripts/check_include_closure.py` - read its rules first).
Tests: existing `MG_Test/Wire/ServerLoopTest.cpp`, `DoorbellTest`/`RingTest` must keep passing by
inspection; add one small test for "a posted control request is visible to ControlIsPending without
the pump" and one for the calibrated spin (e.g. Wait returns true once ready flips within the spin
budget, and parks otherwise) if the existing fixtures make that cheap. Do not build.

### D - role guards and TLS: make every hot-path guard nearly free
Files: `MobileGL/MG_Remote/Server/ServerLoop.{h,cpp}` (OnApplyThread only - coordinate: package T also
edits ServerLoop.cpp; keep your hunk confined to `OnApplyThread`, the thread-identity member(s) and
`ApplyThreadMain`'s first lines where the identity is stored), `MobileGL/MG_Impl/Pipe/SlotAllocator.cpp`
(+ its header), `MobileGL/MG_State/GLState/BufferState/BufferObject.cpp` (RefuseLegacyBufferArmFromApplyThread),
`MobileGL/MG_Remote/Client/PersistentMapTracker.cpp` (OnServerRole / IsLivePersistentMap guard order),
`MobileGL/MG_Remote/Client/ClientSession.cpp` (RefusePipeInputsTouchWhileApplierOwnsIt: order the
early-outs cheapest-first: Transport==Monolith, BatchWaits!=0, then the rest),
`MobileGL/MG_Remote/Client/WireTables.cpp` (RunsAsTheServerRole), `MobileGL/MG_State/GLState/TextureState/MipmapStorage.cpp`.
1. `ServerLoop::OnApplyThread()`: store the apply thread's identity as a plain integer key readable
   with a relaxed atomic load and compare it against a register-cheap identity of the calling thread:
   on aarch64 and x86_64 with clang/gcc use `__builtin_thread_pointer()` (verify availability per
   target with `__has_builtin`; NDK clang 18/19 supports aarch64; check x86_64 support and fall back to
   `pthread_self()` (POSIX) or `std::this_thread::get_id()` (MSVC) where the builtin is unavailable).
   Keep the semantics: false when no apply thread is running (m_running false / identity 0), true only
   on the running apply thread. The function-static `ServerLoopInstance()` guard on every call must go
   (a namespace-scope atomic key is enough; the ServerLoop object still owns it logically).
2. `MGPipeFrontendKeyedRegistryScope` / `MGPipeReverseAnnouncementScope` (SlotAllocator.cpp:39-75):
   the depth counters are only ever READ by `MGPipeRefuseAllocatorFromApplyThread`, which returns early
   unless `OnApplyThread()`. So the counters only matter on the apply thread. Replace the `thread_local`
   with a plain (non-TLS) counter incremented/decremented only when `OnApplyThread()` is true (cheap after
   item 1); on the GL thread the ctor/dtor become one predicate call. Prove in a comment why no data race
   exists (only the apply thread writes and reads; monolith never reads). Keep `Active()` semantics on the
   apply thread exact. If you find a reader other than the apply-thread guard, stop and report instead of
   changing semantics.
3. `RefuseLegacyBufferArmFromApplyThread` (BufferObject.cpp:42-52) and `PersistentMapTracker::OnServerRole`,
   `IsLivePersistentMap`: with item 1 these are ~free; make sure `PushIsArmed()` (a config read) is not
   itself expensive (it reads `MG_Config::Transport` - fine) and that no guard does more than one
   OnApplyThread call per accessor.
4. Look for any other per-draw `thread_local` reads on the client and apply paths (`t_activeDecoder` in
   PipeWireCodec.cpp:359, `g_reply` in PipeRoute.cpp:61, `g_inBarrierWait` in ClientSession.cpp:117,
   `g_textureLegacyArmScopeDepth` MipmapStorage.cpp:73, `tl_initializing`) and report which are on the
   hot path; fix the cheap ones the same way ONLY if the single-writer argument holds; otherwise report.
Monolith equivalence: the pull build (`MOBILEGL_BUILD_DISAGGREGATED=OFF`) must compile and its behaviour
must be identical; G1 (pull-build symbol/.text identity) changes must be named in your report.

### B - persistent-map push: no edge hashing, page-granular shadows
Files: `MobileGL/MG_State/GLState/BufferState/PipeResource.h` (the aligned allocator), the shadow
container in `BufferObject.{h,cpp}` / `PipeResource.*`, `MobileGL/MG_Remote/Client/PersistentMapTracker.{h,cpp}`
(TrackWriteMap ~259-330, the SIGSEGV handler ~200-240, PushBlocksForChecked 492-685, PushDrawConsumers 704-818).
Today: the shadow allocator gives 4096-byte ALIGNMENT but not a page-multiple SIZE, so the last page of
a shadow can hold foreign bytes; TrackWriteMap therefore aligns the protected interior INWARD and the
partial-page edges (head edge when the mapped offset is not page-aligned; tail edge for every buffer
whose mapped end is not page-aligned - i.e. nearly every buffer) are hashed with XXH3 on EVERY draw for
every live map with edges (the epoch-skip fast path pushes edges of ALL live maps, not just consumers).
That is 8.5% of the client thread in XXH3 plus the MappedData/IsMapped/GetMappedRange guards around it.
Goal: zero per-draw hashing in steady state.
1. Make every shadow allocation page-granular: round the allocation SIZE up to a multiple of
   SHADOW_ALLOCATION_ALIGNMENT in the allocator (verify the container: what `Bytes()` returns, whether
   capacity vs size matters, whether any shadow can be backed by something else - adopted GPU stores are
   excluded from tracking by IsLivePersistentMap; confirm). Then every byte of
   `[base, base + RoundUp(size, 4096))` belongs to this shadow and no other allocation.
2. TrackWriteMap: when the shadow base is page-aligned and the allocation is known page-granular, align
   the protected range OUTWARD to page boundaries (clamped to the shadow's own page-rounded extent), so
   there are no edges and `hasEdges` is false; sub-page maps become one protected page. Keep the inward
   arm as the fallback for any shadow that fails the page-granular test (and keep the hash arm for
   untracked maps). The SIGSEGV handler's ownership tests (SEGV_ACCERR, inside a live tracked range,
   bitmap-bit replay discrimination) must hold for the widened range: bytes outside the mapped range but
   inside the shadow are written by the client's own API paths (SubData/FlushMappedRange memcpy into the
   shadow) - a fault there is answered like any interior fault; make sure the push clamps to [begin,end)
   as today and that nothing else (e.g. readback writeback into the shadow on the client thread,
   `Forget`, resize/respecify that reallocates the shadow, `NoteMapStateChanged`) breaks the
   "protected pages are re-armed / released" invariant. Re-read the round-1 commit message of cb06538c
   (git show cb06538c) for the reasons behind inward alignment before you widen anything.
3. The epoch-skip fast path (PushDrawConsumers 727-737) then does nothing per member; keep the loop or
   drop it - measure nothing, reason. First push still ships everything once.
4. Report: a table of the per-draw work that remains in PushDrawConsumers when the epoch has not moved.
Tests: `MG_Test/Buffer/SplitBufferTest.cpp` and `MG_IntegrationTest/Scenarios/PersistentCoherentMapScenario.cpp`
exercise this; add/adjust a unit case for the outward alignment (a sub-page map gets a protected page;
an unaligned base falls back to inward+edges). Do not build.
Danger notes from cb06538c: a signal-blocked driver/runtime worker thread faulting on a foreign byte
inside a protected page kills the process (that was the ctest SmallRing segfault) - the whole point of
item 1 is that no foreign byte can be inside a protected page any more; state that argument precisely.

### C - client verb overhead: validate/fill, image-unit sweep, server stamp loop
Files: `MobileGL/MG_Impl/Pipe/PipeFill.cpp` (MGPipeValidateForVerb 2781+, the residual fill / CopyField),
`MobileGL/MG_Pipe/PipeInputs*`/`MobileGL/MG_Backend/MGPipe/PipeInputs.cpp` (MGPipeServerStampVerbBoundary),
`MobileGL/MG_Remote/Client/GpuWritePending.cpp`, `MobileGL/MG_Remote/Client/EmitTables.cpp` (draw emit path),
`MobileGL/MG_State/GLState/TextureState/*` only if an image-unit high-water mark is needed.
1. `MarkWritableImageBufferTextures`: replace the 32-unit sweep per draw with a high-water mark
   (the GLContext already keeps touched-unit marks for texture units and buffer binding points; add the
   same for image units at the BindImageTexture site if none exists - note this is MG_State (shared with
   monolith) so keep it minimal and name the G1 impact; or, if `ImageEmit`/the tracker already knows
   whether any writable image-buffer unit is bound, use that latch).
2. `MGPipeValidateForVerb` draw path: find what the 10.9% inclusive is made of beyond tracker.Update
   (1.5%) and CopyField (2.5%): the residual fill's per-verb copies (which fields does the draw class
   copy, and which of those are already record-supplied under a live wire - P5c rv skips the value-class
   rows; check the object-class rows' copy cost, e.g. `GetBindingSlot` from CopyField), the
   `RefusePipeInputsTouchWhileApplierOwnsIt` guard order (package D reorders it; don't duplicate),
   `ContextValuesWireLive()` / `RunsAsTheServerRole()` per verb (cache per session if legal),
   `ParsePoisonOmissionKnob()` per verb (should be once). Cut what is provably redundant; leave the
   emitters' semantics alone.
3. `MGPipeServerStampVerbBoundary`: the 63-field loop per verb on the apply thread computes
   `answerable(field, verb)` from constant tables - precompute per verb once (a static table of
   which fields are answerable per verb, built lazily or constexpr) so the per-verb work is a
   table-driven fill. Keep the withdrawal semantics exactly (FilledGen = serial or 0).
4. Anything else on the draw emit path in EmitTables.cpp that is per-draw and redundant (e.g. repeated
   session lookups, ReadDrawBindings re-reading slots the fill already read). Report what you found and
   what you left.
Monolith: PipeFill.cpp is compiled into the monolith push build too; keep monolith behaviour identical
(G1 for the pull build; push-build equivalence by inspection).

### E - research only (no code): can the draw barrier be deferred by one verb?
Deliverable: a design note, not a patch. Question: the client today fills the residual object-class
rows of `gPipeInputs` (FieldOwnership.def BARRIER_PULLED: GetBoundVertexArray, GetBufferBindingSlot,
GetBufferBindingPoint, GetFramebufferBindingSlot, GetImageTextureBinding, GetTextureUnitObject,
GetProgramForDraw, GetProgramForDispatch, GetTransformFeedbackProgram, GetBufferBindingPointCount,
GetProgramObject, GetTextureObject, HasOpenTransformFeedbackSpan, ValidateProgramName, RecordError),
publishes the draw, and WAITS. Map, for the draw/clear/blit verbs on DirectGLES under inproc:
(a) every server-side read that reaches client-owned memory through those rows (what does the server
dereference through each pointer: VAO attribute array? texture unit binding slots? UBO binding point
table entries + ranges? program object internals?), including the two exemption scopes
(`MGPipeFrontendKeyedRegistryScope` sites in DirectGLES.cpp, `MGPipeReverseAnnouncementScope`);
(b) which of those reads would be UNSAFE if the client continued past the publish and executed the
next draw's GL calls (glBindBufferRange / glBindTexture / glUseProgram / glVertexAttribPointer / ...)
before the server applied this draw;
(c) the minimal "snapshot at fill" scheme: what would have to be copied per draw into a per-verb
snapshot the server reads instead (count the bytes/refcount ops per draw at 852 draws/frame), and
where the client would then wait ("before the next fill" instead of "after this publish"), i.e.
one-verb pipelining that overlaps the client's inter-draw work with the server's apply;
(d) what the existing pushed records already cover (SetSamplerViews / VertexElements CSO /
set_vertex_buffers / set_context_values) such that the server COULD stop reading the live tables;
(e) a cost/benefit estimate and a recommendation: P5d round 4, or leave to P11 / P3b-P4b.
Write it to the report path given below (markdown, <= 400 lines, with file:line citations at head 85362a85).

## Rules for every package
- Do NOT build (no gradle, no cmake, no ndk), do NOT touch adb/devices, do NOT push, do NOT commit
  in any shared tree. Work only in your isolated worktree; deliver `git diff HEAD -- MobileGL` as a
  patch FILE at the path given below, plus a short report file. Both must be written before your
  final message.
- No `Co-Authored-By`, no attribution lines anywhere.
- No hot-path instrumentation left in (counters that print per frame are fine when behind the
  existing `MG_Util::PipeStats::Enabled()` / stats period; no per-draw logging).
- Prefer changes inside `#if MOBILEGL_BUILD_DISAGGREGATED` or in MG_Remote (split-only). When a change
  touches code compiled into the pull build (MOBILEGL_BUILD_DISAGGREGATED=OFF, MOBILEGL_PIPE_PUSH=OFF),
  say so in the report (G1 names every symbol/.text delta) and keep behaviour identical.
- Every correctness-bearing change gets a red-once argument: name the existing test (or the one you
  add) that goes red if the change is reverted or broken. Keep tests light; do not add a test for a
  refactor with no semantic change.
- Keep the file's own comment discipline: explain WHY in the code comment, in the voice of the
  surrounding code, and do not delete the reasoning comments you replace - update them.
- Report format (markdown): what changed (per file), why (tie to the profile numbers above), the
  red-once test, G1/monolith impact, risks and what you did not do. <= 120 lines.
