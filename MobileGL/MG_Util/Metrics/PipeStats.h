// MobileGL - MobileGL/MG_Util/Metrics/PipeStats.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// MGPipe boundary counters (plan B section 11 "P0 - hygiene, measurement, gates and
// skeleton", and the corollary in section 2.3.1).
//
// WHAT THIS IS FOR. The disaggregation plan has to size two things it cannot size by
// reading the tree: how many BYTES cross the frontend/backend boundary per frame (that
// sizes SEG_STAGE and the command segment), and how many accessor CALLS and memo-gate
// probes the backends actually execute per draw (that decides whether pushing state is
// cheaper than pulling it at all). Section 2.3.1 makes the second one the load-bearing
// number: the static call-site counts everyone quoted - Espryt 124 / Magma 169 - are NOT
// the dynamic per-draw cost, because every one of those paths is memo-gated, and the real
// steady state is believed to be 10-25 accessor calls per backend per draw. Without a
// dynamic counter the P2 verdict stays a guess.
//
// COST WHEN OFF. g_pipeStatsEnabled is a plain global Bool latched once at Init() from
// MG_Config::Features.PipeStats (MOBILEGL_PIPE_STATS). Every counting site in the two
// backends is written as
//
//     if (MG_Util::PipeStats::Enabled()) MG_Util::PipeStats::Add...(...);
//
// so with the feature off a site costs one load of a hot global plus one never-taken,
// perfectly-predicted branch, and none of the counter state is touched. The counters
// themselves are relaxed atomics rather than plain integers because texture and buffer
// staging can be reached from more than one thread; relaxed adds cost nothing extra on the
// off path, which never reaches them. The off-path cost is not a guess: see the paired
// A/B in the branch's evidence.
//
// WHAT IS COUNTED AND WHAT IS NOT: see the site inventory in PipeStats.cpp. That inventory
// is the contract - it names every path that is NOT wired, because a byte class that reads
// zero while a real copy runs uncounted is worse than a missing counter.
namespace MobileGL::MG_Util::PipeStats {

    // Byte classes. Every one of these names a population of bytes that would have to be
    // MOVED across the boundary once the backend no longer shares an address space with
    // the frontend, which is why they are grouped this way rather than by call site.
    enum class ByteClass : Uint32 {
        // Buffer object contents flushed to the driver: glBufferData / glBufferSubData /
        // map-write ranges / the persistent upload ring (Espryt), and every host->device
        // copy of a buffer object's contents (Magma).
        StageBuffer = 0,
        // Texel bytes handed to glTexSubImage & friends / packed into the Vulkan upload
        // staging slice, whichever upload shape was chosen.
        StageTexture,
        // The default-uniform-block ("global UBO") image, uploaded at most once per program
        // per frame.
        StageUboGlobal,
        // Named uniform-block bytes that a backend has to repack itself, i.e. Magma's UBO
        // ring. Espryt binds the frontend buffer straight to the driver and contributes
        // nothing here - which is exactly the asymmetry D-B8 is about.
        StageUboNamed,
        // Client-memory vertex arrays uploaded into a scratch VBO / transient arena slice on
        // the draw path.
        StageVertexClient,
        // Client-memory / rewritten index data staged on the draw path.
        StageIndexClient,
        // Draw-parameter bytes a backend synthesises and stages for the draw itself: the
        // indirect-command array and the compute path's per-draw info array. These are the
        // bytes that become MGPipe command-record payload once the boundary is explicit,
        // which is why they are not folded into the index class.
        StageIndirectCmd,
        // Bytes pushed because a persistently mapped range was published to the backend.
        PersistentMapPush,
        // PLACEHOLDER (plan section 6.3): the residual value block does not exist yet. The
        // class is minted now so the counter names never churn; it stays at 0 until P2.
        ResidualValueBlock,
#if MOBILEGL_PIPE_PUSH
        // P4a's, and THE PUSH GUARD IS NEW ON THIS ENUM: CallClass has had one since P2 and
        // ByteClass has never had one, so the block is opened here rather than the member
        // simply appended. Without it the pull build's two counter arrays, the name table, the
        // short-name table and FormatWindowLine all resize for a class that could never leave
        // zero - and the pull build has to stay symbol-identical.
        //
        // The bytes of every CSO BLOB the client declares in a frame: P3a's vertex-elements
        // blobs (which MEASUREMENTS.md recorded as a client-side array that was never
        // measured, and left to P4a to give the summary line a class for), P4a's sampler
        // parameter blobs, and P4a's program archives. It is the number that says what a
        // transport would actually have to move for the CSO families, as opposed to what the
        // records themselves cost.
        CsoBlobBytes,
        // P6 GATE 8's FIRST NUMBER (CONTRACT-P6.md §9 item 8), and the reason it is a ByteClass
        // rather than one more Gauge: the deliverable is BYTES PER FRAME, and a byte class IS
        // the windowed sum this module already divides by the window's frame count. A gauge is
        // a run total deliberately (see the Gauge enum) and windowing one is not a thing this
        // module does.
        //
        // WHAT IT MEASURES, AND WHY IT IS NOT ANY OF THE stage-* CLASSES ABOVE. Those count what
        // a BACKEND moves into its own driver, on the apply thread, from bytes the wire already
        // delivered. This counts what the WIRE put into SEG_STAGE - the client-side staging
        // arena a record's blobs are copied into - and it is read as "staged content per frame".
        //
        // COUNTED AT THE ALLOCATOR, WHICH IS THE ONE CHOKE POINT. Every writer that puts bytes
        // into SEG_STAGE reaches them through PipeWireEncoder::StageBytes and that function's
        // only allocation call is StageAllocate (PipeWireCodec.h:376), so a single site here
        // covers the buffer content walks, the texture slabs, the CSO archives and the
        // storage-block names alike. Summing at the CALLERS instead would be a hand-kept list of
        // the paths that happen to exist today, and the next producer added would be missing
        // from it with nothing to notice - which is the failure mode the site inventory at the
        // top of PipeStats.cpp exists to forbid.
        //
        // IT IS THE PAYLOAD, NOT THE ARENA OCCUPANCY. Align8 slack and the allocator's wrap skip
        // (a run that would straddle the segment's end skips the remainder) are real bytes of
        // the segment and are NOT counted: neither is content the producer wrote, and neither
        // is what MOBILEGL_IPC_STAGE_MB has to hold for one record. The per-blob distribution
        // this average hides is the staged-blob histogram below.
        StageSegmentBytes,
#endif
        Count
    };

    // Call classes: the dynamic per-draw cost section 2.3.1 says P2 cannot be decided
    // without.
    enum class CallClass : Uint32 {
        // Draws that reached an instrumented backend draw-preparation entry point. The
        // denominator for every "per draw" number below.
        Draws = 0,
        // GLContext accessor calls actually EXECUTED on the instrumented paths. Counted in
        // static tallies at the ~10 hot entry points, not by wrapping all 293 call sites -
        // see the inventory in PipeStats.cpp for exactly what is and is not in this number.
        AccessorCalls,
        // Texture upload emissions: one per (upload target, level) that actually shipped
        // texels. The eventual resource_subdata record count.
        TextureUploadEmissions,
        // Emissions that took the union-box shape (one driver upload job).
        TextureUploadBoxEmissions,
        // Emissions that took the refined rect-list shape (N driver upload jobs). The
        // box/rect split is the thing SSIM cannot see and the +6 ms/frame Mali cliff came
        // from, so it is counted separately from the byte total.
        TextureUploadRectEmissions,
        // Driver upload jobs issued by those emissions: 1 per box emission, N per rect-list
        // emission.
        TextureUploadJobs,
#if MOBILEGL_PIPE_PUSH
        // P2's two, and they are PUSH-ONLY on purpose: a render-state CSO exists only in a
        // push build, and the pull build has to stay symbol-identical (G1) - growing this
        // enum there would resize the counter arrays, the name table and FormatWindowLine
        // for a pair of counters that could never leave zero.
        //
        // Render-state CSOs MINTED: a pipeline-subset hash that missed the CsoCache and had
        // to be created. The Blaze3D blend toggle is the shape this exists to answer for -
        // enable/draw/disable/draw forever must mint 2 and then never mint again - and it is
        // half of what a P13 retune of the 64-entry capacity reads.
        RenderStateCsoMints,
        // Render-state CSOs BOUND: one per bind_render_state, mint or reuse. mints/binds is
        // the cache's hit rate, and it is the number the CSO content-addressing negative
        // control moves.
        RenderStateCsoBinds,
        // P3a's, and push-only for the same reason as the two above.
        //
        // EVERY map_persistent EMISSION, i.e. every acquisition ATTEMPT - a mint or a decline
        // - because every one of them needs an answer from the resource owner. Counted that
        // way on purpose: "round trips actually taken" is 0 by construction in a monolith and
        // could never go red, which is not a counter, it is a decoration. Counted as attempts
        // the number is identical in both modes, it is exactly "one per storage definition",
        // and a regression that acquires per DRAW instead of per definition shows up on the
        // first window. Counted at the client emitter, behind the usual Enabled() predicate;
        // no timer anywhere.
        MapPersistentRoundtrips,
        // P4a's five, push-only for the same reason as the three above, and every one of them
        // counts a record that ACTUALLY WENT OUT - post-suppressor - because the number an
        // operator needs is the traffic, not the number of times the emitter was asked.
        //
        // The four set counters are how the suppressors' hit rates become readable at all: a
        // suppressor that stopped suppressing is invisible in the pixels and shows up here as
        // a per-frame count that tracks the draw count instead of the state changes.
        FramebufferEmissions,
        SamplerViewEmissions,
        SamplerStateEmissions,
        ShaderImageEmissions,
        // The CLIENT-side twin of Espryt's TextureUploadEmissions, which counts the same
        // records on the server. Two published numbers rather than one is the whole point:
        // SSIM is completely blind to the box-versus-rect upload shape, and the Mali cliff it
        // hides is ~+6 ms/frame, so an emission-shape divergence has to be a difference of two
        // numbers rather than something only a GPU can see.
        ClientTextureUploadEmissions,
        // THE TEXTURE-REMINT PULL RATE (ROADMAP open question 2; P4a final review M-A). Counted
        // by Espryt once per transition in which a texture that ALREADY HAD backend storage is
        // re-minted image-bindable and its defined levels are replayed from the client's shadow
        // (RequireImageBindableStorage) - the reach-back a split cannot make (D-M) and the one
        // ImageBindableHint exists to prevent. A texture whose hint arrived before its first
        // sync is allocated image-bindable up front and never counts. `trp=` on the summary
        // line; the number that decides MOBILEGL_PIPE_TEXEL_RETAIN_MB's default.
        TextureRemintPulls,
        // P5's, and push-only for the same reason as the nine above.
        //
        // `rsp` - THE SIZE OF THE P6/P7/P8 DEBT. One per read, on the server side, of a
        // BARRIER-PULLED PipeInputs field (CONTRACT-P5.md table 2): a field no pushed record
        // supplies, which the server answers by reading the value the CLIENT's residual fill
        // left in the single shared gPipeInputs while the verb barrier holds both threads
        // apart. That is correct only because of the barrier, which is what makes the barrier
        // load-bearing rather than cautious - so the count is the debt, and a phase that
        // retires a family of fields is expected to move it down.
        //
        // Counted at the ONE place that decides what a stale read means
        // (MG_Backend/MGPipe/PipeInputs.cpp), so the 56 checked accessors and the seven sticky
        // forwards cannot drift apart on it. It is zero in every monolith lane by
        // construction: nothing arms it but a server verb-boundary stamp.
        ResidualPulls,
        // P5e (gl), ID-115: `vbs` - SERVER VERB BOUNDARIES STAMPED. One per record that IS a
        // verb boundary, counted at MGPipeServerStampVerbBoundary itself.
        //
        // IT EXISTS BECAUSE "THE STRICT LANE IS GREEN" AND "STRICT WAS NEVER ARMED" WERE
        // OBSERVATIONALLY IDENTICAL. Every strict check downstream - the poison, rsp, the
        // BARRIER-PULLED verdict - is reachable ONLY after this stamp, and the monolith arm
        // never stamps at all. So of the entries that passed strict before this counter existed,
        // three were monolith transport (where the whole mechanism is structurally unreachable),
        // two self-skipped, one was a death test and one was a Python check: not one was a
        // record-carrying split GL scenario, and the lane could not tell that from rigour.
        //
        // rsp CANNOT PLAY THIS PART, which is the reason for a counter rather than a reused one:
        // rsp goes to zero exactly when the phase SUCCEEDS, so a positive control built on it
        // would start failing on the day the debt is paid. `vbs` is non-zero whenever the
        // server applied a verb at all, before and after retirement alike.
        ServerVerbBoundaries,
        // P6 GATE 8's SECOND NUMBER (CONTRACT-P6.md §9 item 8): "records/frame post-chunking".
        //
        // ONE PER RECORD THE WIRE ENCODER ACTUALLY WROTE, which is what makes it the
        // post-chunking count rather than a restatement of the emitter's call count. Before the
        // stage chunk budget landed, one application call produced one record and the question
        // could be answered from the emitter's side; now a call whose content is wider than
        // MGPipeStageChunkBytes() produces SEVERAL - the buffer content walks cut themselves at
        // the budget and one texture level is emitted as whole-width slabs - and only the
        // encoder knows how many. So this is counted where the record is committed
        // (PipeWireCodec.cpp's EncodeRecord, on the successful path), not where a caller asked
        // for one.
        //
        // A kRecPad RING FILLER IS NOT A RECORD AND IS NOT COUNTED. Those are the bytes Reserve
        // lays down when a record would have straddled the ring's boundary; they carry no
        // opcode, both sides skip them, and they are already visible in the gauges as ringpads.
        // An emission the ring REFUSED is not counted either: EncodeRecord returns kInvalidSeq
        // and the caller publishes and retries, so the record is counted once, when it goes.
        //
        // Read it against the Client*Emission counters above: a sampler/framebuffer/texture set
        // that is resolved and then suppressed does not reach this number, so records-per-frame
        // is wire traffic while those are emission calls, and the difference between them is
        // exactly what the suppressors absorbed.
        //
        // NOT SetBytes(), and that is the whole reason it is a CallClass rather than a Gauge: the
        // deliverable is records PER FRAME, so it needs the windowed division and the label rule
        // that goes with it (FormatWindowLine prints the run total instead when the window holds
        // no Present).
        WireRecords,
#endif
        Count
    };

#if MOBILEGL_PIPE_PUSH
    // P5's GAUGES, and they are a THIRD KIND of counter rather than three more CallClass rows.
    //
    // A ByteClass and a CallClass are SUMS this module owns and a call site increments. These
    // three are neither: they are the wire producer's own running readings - a MAXIMUM and two
    // RUN TOTALS that live on MG_Remote's encoder, which this module cannot see and must not
    // link against (MG_Util is below MG_Remote, and the pull build has no MG_Remote at all).
    // The owner publishes its current value at the frame boundary and this module prints the
    // last one it was given. Summing them here would be wrong twice: a maximum is not additive,
    // and the encoder already holds the run total, so adding deltas would double-count.
    //
    // THEY ARE RUN TOTALS ON A WINDOWED LINE, deliberately and against the file's own habit.
    // Everything else on the summary line covers "since the previous line" because a run total
    // over a workload whose shape changes hides the number P2 wants. These three are the
    // opposite: "the largest record this run ever wrote" and "did the ring ever wrap" are
    // questions about the RUN, and a windowed maximum would read 0 in every window that did not
    // happen to contain the biggest record - which is the shape of a proof obligation that
    // cannot fail. The label says so in the line itself (`maxrec=` is bytes, not bytes/frame).
    //
    // PUSH-ONLY for the reason every counter added since P2 is: the pull build must stay
    // symbol-identical (gate G1), and a gauge whose only publisher is MG_Remote could never
    // leave zero there.
    enum class Gauge : Uint32 {
        // R-10's PROOF OBLIGATION. The largest single record the wire encoder has written, in
        // bytes, and the cap it must stay under - RingProducer::MaxRecordBytes() ==
        // MOBILEGL_IPC_RING_MB / 2. The content rows cut their blobs at the stage chunk
        // budget, so what this measures is a record's own bytes. Until this pair existed the
        // only consumers of PipeWireEncoder::MaxRecordBytesSeen() were
        // codec unit tests, so BRIEF 8 item 3 had no measurement from any real workload
        // (joint-v1.md 5, "Maximum record bytes: NO MEASUREMENT").
        MaxRecordBytes = 0,
        MaxRecordBytesCap,
        // R-9's three producer readings. `RingWraps` is SEG_CMD going ROUND - the head crossing
        // a multiple of the capacity - which is the event exit gate E3(e)'s small-ring lane
        // asserts, because it is guaranteed once the workload writes more bytes than the ring
        // holds. `RingWrapPads` is the kRecPad fillers laid when a record would have STRADDLED
        // that boundary, which is R-9's "a pad does not advance seq" path and is RECORDED, not
        // asserted: a uniform record stride over a power-of-two ring lands on the boundary
        // exactly and never straddles it (measured). `RingWaits` is SEG_STAGE allocations that
        // had to wait on the consumer's retiredSeq. See PipeWireCodec.h for why the command
        // ring contributes no wait count while the verb barrier is armed.
        RingWraps,
        RingWrapPads,
        RingWaits,
        // P5d ROUND 3's WAIT LEDGER, the four numbers that say where the split's frame went.
        // Under `inproc` the lockstep frame is "client work + server apply + handoff", and the
        // handoff is made of waits: simpleperf on the workload device measured
        // SessionProducer::WaitForApplied at 27.9% self of the GL thread and the apply thread's
        // idle poll at ~49% of its own. A wait that SPUN and a wait that PARKED cost three
        // orders of magnitude apart, so each side publishes both: `ServerWaits` /
        // `ClientWaits` are entries into Doorbell::Wait, `ServerParks` / `ClientParks` are the
        // subset that ran out of spin budget and blocked.
        //
        // EACH SIDE PUBLISHES ITS OWN. CONTRACT-P5C rule E forbids either role naming the
        // other's memory, so the client does NOT read ServerLoop::ParkCount() across the role
        // line - the apply thread publishes the server pair from its own loop and the GL thread
        // publishes the client pair from EmitPresent. PipeStats is the meeting point precisely
        // because it is below both - IN ONE PROCESS. Under `spawn` the server pair is published
        // into the SERVER process's PipeStats, so the client's summary line prints
        // `srv=0 srvpark=0` for the whole run: a zero that means "another process", printed in
        // the shape of a zero that means "never waited". That is how `rsp` and the rest of the
        // server-published gauges already behave; the round this pair was added for is `inproc`.
        //
        // THE PARK NUMBER IS THE SUBSET OF THE WAIT NUMBER ON PURPOSE, and stays one only
        // because both halves of each pair are counted by the same code path: Doorbell::Wait
        // increments a tally the WAITER owns (its `parkTally` out-parameter), once per wait that
        // really blocked. A per-bell counter would not do - a bell belongs to an endpoint and
        // every waiter on it shares it - and a park counted per Park() call would not either,
        // since one wait can park, wake on a remembered notify and park again.
        ServerWaits,
        ServerParks,
        ClientWaits,
        ClientParks,
        Count
    };

    // Publishes the owner's current reading. Cheap and unconditional on the caller's side:
    // the call sites are per-frame, not per-record.
    void PublishGauge(Gauge gauge, Uint64 value);
    Uint64 GaugeValue(Gauge gauge);
#endif

    // Memo gates. Each is a place where a backend decides "nothing moved, skip the work".
    // Hit == the gate short-circuited; Miss == it fell through and did the work. The six
    // are exactly the ones section 2.3.1 tabulates.
    enum class Gate : Uint32 {
        // DirectGLES.cpp SyncRenderState: the render-state-version early-out.
        EsprytRenderState = 0,
        // DirectGLES.cpp SyncNeccessaryTextures: the six-value sync-list key compare.
        EsprytTextureSyncList,
        // DirectGLES.cpp CurrentUnitBindingsEpoch: the (context, max unit, bind generation)
        // shutter over the unit walk.
        EsprytUnitBindingsEpoch,
        // VulkanRenderer.cpp TrySetupDrawFastPath: the whole snapshot fast path.
        MagmaDrawFastPath,
        // VulkanRenderer.cpp GetOrCreatePipeline: the pipeline memo.
        MagmaPipelineMemo,
        // VulkanRenderer.cpp ApplyDynamicDrawStateTail: the version+extent tail gate.
        MagmaDynamicTail,
        Count
    };

    // Per-draw command payload size histogram (plan section 4.5.7: SEG_CMD has to be sized
    // off the DISTRIBUTION, not off a per-frame total). PLACEHOLDER in P0: MGPipe emits no
    // records yet, so nothing in the backends calls RecordDrawPayloadBytes. The bucketing
    // and the reporting are implemented and unit-tested so that the first generator to
    // emit records only has to add the one call.
    inline constexpr Uint32 kPayloadHistogramBuckets = 24;

#if MOBILEGL_PIPE_PUSH
    // Per-STAGED-BLOB size histogram, and it is a SECOND histogram rather than a wider
    // kPayloadHistogramBuckets because the two answer different questions about different
    // populations: the one above is the command payload of a draw (plan section 4.5.7, sizing
    // SEG_CMD), this one is how big the blobs a workload stages into SEG_STAGE actually are.
    //
    // IT EXISTS FOR CONTRACT-P6.md §9 item 8, and MEASUREMENTS.md:342 says in as many words why:
    // the chunking work landed (1e7c372e buffer walks, 9469d48e texture slabs) and every
    // `maxrec` number the file carries predates it, so "the per-blob byte distribution after
    // chunking" had no measurement at all. A per-frame AVERAGE cannot answer it either: an
    // average of 8 MiB is the same number whether one blob of 8 MiB was staged or two of 4 MiB
    // that the budget should have cut to one. The bucket edges are PayloadBucketOf's, shared so
    // the two distributions are read the same way.
    //
    // PUSH-ONLY like every counter added since P2: its only sampler is the wire encoder, which a
    // pull build does not compile, so the array, the reset, the JSON row and the accessor would
    // all exist there for a population that could never be sampled (gate G1).
    inline constexpr Uint32 kStagedBlobHistogramBuckets = kPayloadHistogramBuckets;
    Uint64 TotalStagedBlobBucket(Uint32 bucket);
    // Sample one staged blob. Called by the wire encoder where a blob is staged, behind the
    // usual Enabled() predicate.
    void RecordStagedBlobBytes(Uint64 bytes);
#endif

    // Frames between two summary lines when MOBILEGL_PIPE_STATS=1.
    inline constexpr Uint64 kDefaultSummaryFramePeriod = 120;
    // The period Init() latched from MOBILEGL_PIPE_STATS_PERIOD (kDefaultSummaryFramePeriod
    // when unset); never 0.
    Uint64 SummaryFramePeriod();

    // The latch. Read directly by Enabled() so the off path is a global load and a
    // predicted branch - do not turn this into a function call.
    extern Bool g_pipeStatsEnabled;

    inline Bool Enabled() { return g_pipeStatsEnabled; }

    // Latches g_pipeStatsEnabled from MG_Config::Features.PipeStats and clears every
    // counter. Called from MobileGL::Initialize() right after the config load.
    void Init();

    // Final summary line plus, if MOBILEGL_PIPE_STATS_FILE names a path, the JSON dump.
    // Called from MobileGL's teardown. Idempotent.
    void Shutdown();

    void AddBytes(ByteClass byteClass, Uint64 bytes);
    void AddCalls(CallClass callClass, Uint64 count);
    void CountGate(Gate gate, Bool hit);
    void RecordDrawPayloadBytes(Uint64 bytes);

    // Frame boundary: publishes the frame's values to Tracy (when TRACY_ENABLE), folds them
    // into the run totals, clears the frame accumulators, and every kSummaryFramePeriod
    // frames emits the summary line. Called from each backend's Present().
    void OnPresent();

    // --- introspection, for the unit test and the JSON dump -------------------------
    Uint64 FrameBytes(ByteClass byteClass);
    Uint64 TotalBytes(ByteClass byteClass);
    Uint64 FrameCalls(CallClass callClass);
    Uint64 TotalCalls(CallClass callClass);
    Uint64 TotalGateHits(Gate gate);
    Uint64 TotalGateMisses(Gate gate);
    Uint64 TotalPayloadBucket(Uint32 bucket);
    Uint64 FrameCount();

    const char* NameOf(ByteClass byteClass);
    const char* NameOf(CallClass callClass);
    const char* NameOf(Gate gate);

    // The compact fixed-format one-liner MGLOG_I prints, covering the CURRENT window (see
    // AdvanceSummaryWindow). PURE: calling it twice returns the same text and changes no
    // counter, so a probe, a test or a second reporting channel can format the window
    // without stealing it from the log.
    String FormatWindowLine();
    // Closes the current window: the run totals as of now become the base the next
    // FormatWindowLine() subtracts. Emitting the line and advancing the window are separate
    // on purpose - the pair used to be one function whose name promised a formatter.
    void AdvanceSummaryWindow();
    // The teardown dump. Run totals only: a per-frame JSON stream is a different tool.
    String FormatJson();

    // Test hooks, used by no shipping path. Init() clears the counters through an internal
    // ResetCounters() rather than by calling ResetForTesting().
    void SetEnabledForTesting(Bool enabled);
    void ResetForTesting();

#if MOBILEGL_BUILD_DISAGGREGATED
    // The role-derived path the JSON dump will actually write, or empty when
    // MOBILEGL_PIPE_STATS_FILE is unset. Empty when unset; never the bare configured name.
    //
    // IT EXISTS BECAUSE THE OBVIOUS TEST IS NOT A CONTROL. The first version of this case asserted
    // things about MG_Util::Debug::RoleLogPath directly - that the two roles get different paths,
    // that neither equals the base name. Every one of those assertions passes with the fix REVERTED,
    // because they test the naming helper, which was never broken; what was broken was that
    // WriteJsonDump did not CALL it. A test that stays green while the defect is present is worse
    // than no test, so this accessor exists to make the call site itself observable.
    //
    // Guarded on the DISAGGREGATED option rather than PIPE_PUSH, because the collision it describes
    // only exists with two roles - a monolith build has one and needs no derivation (gate G1).
    String RoleDerivedJsonDumpPathForTesting();
#endif

} // namespace MobileGL::MG_Util::PipeStats
