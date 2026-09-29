// P6.5 transport measurements. The Window state is written only by the single client
// producer; the socket-io counters drained by LinkMetricsTakeReadStats/TakeSendStats are
// atomic and cross-thread (io thread writes, apply/client thread reads) - see below.
#pragma once
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {
    // Disabled sessions never read a clock. Samples are a fixed 32-bucket histogram.
    void LinkMetricsBegin();
    void LinkMetricsEnd();
    // PER-OP: WHICH ROWS BOUGHT THE WAITS. The handoff measured the total (55,428 reply waits in
    // one frame) and left "which rows" open, and a total cannot answer it: an upload's wait is what
    // P12 item B removed, a create's is per resource and a parameter set's is per call - three
    // different fixes, and the split is what says whether B is the whole fix or a fraction of one.
    // `op` is an MGPWireOp value passed as an unsigned so this file keeps its single include; a
    // value past the table lands on the last slot rather than being dropped, so a reader gets a
    // number that does not add up instead of a silent zero.
    inline constexpr std::uint32_t LinkMetricsMaxOps = 128;
    std::uint64_t LinkMetricsBeginReply(bool wantsReply, std::uint32_t op);
    void LinkMetricsReplyApplied(std::uint64_t startedNs);
    void LinkMetricsStageBytes(std::uint64_t bytes);
    // Wall time spent in the producer's transport park (flush, spin and sleep).
    // The server can run concurrently, so this is an independently measured
    // duration, not a residual obtained by subtracting role CPU clocks.
    std::uint64_t LinkMetricsBeginTransportWait();
    void LinkMetricsEndTransportWait(std::uint64_t startedNs);

    // THE SAME NUMBER THE PER-FRAME LINE PRINTS AS `wait_replies` (P65LinkMetrics), read back
    // in-process. P12 item B needs it as a GATE rather than as a log line: "the texture half no
    // longer buys a round trip per record" is otherwise only observable by timing, and a timing
    // is a measurement, not a gate. It reads 0 while MOBILEGL_PIPE_STATS is unset - which is
    // exactly why a case that asserts 0 for one row must ALSO assert that a reply-owning row
    // still counts, or it would pass on a session where nothing was counted at all.
    std::uint64_t LinkMetricsReplyWaits();
    std::uint64_t LinkMetricsReplyWaitsFor(std::uint32_t op);

    // RECORDS EMITTED, PER OP - the other half of the same question. A wait count says what a frame
    // PAID; this says what it SENT, and the device run that motivated it showed one atlas frame
    // paying 43,232 waits while the upload row paid none - so the next thing to know is how many
    // records those waits were bought for and by whom. Counted at the encoder's one commit point
    // (PipeWireCodec.cpp, ++m_emitSeq), so no row can be emitted without being counted.
    void LinkMetricsNoteRecord(std::uint32_t op);
    std::uint64_t LinkMetricsRecordsFor(std::uint32_t op);
    void LinkMetricsPresent();
    void LinkMetricsServerPresent(std::uint64_t serial);

    // P65READ: THE SERVER'S SOCKET-READ PATH, ACCOUNTED WHERE IT CAN BE SEEN. The counters are
    // published from the io thread's recv loop and read by the APPLY thread, because the io
    // thread's own MGLOG_ lines never reach the phone's forwarded log (only mgl-srv-apply and
    // mgl-display-ser do - measured, not assumed). Taking them RESETS them, so one call is one
    // frame's worth of the stage nothing else measures: the frame's time is not the client's GL
    // thread (13%) and not PipeApplier::ApplyOne (11%), and this is what sits between them.
    //
    // Free functions, NOT StreamLink members: the P6.5 seam gate (scripts/ci/link_seam_purity.py)
    // rejects a concrete-link include outside Transport, and the readers of these numbers live in
    // ClientSession/ServerLoop. The counters themselves stay in StreamLink.cpp beside the recv /
    // sendmsg loops that feed them.
    struct LinkIoStats {
        std::uint64_t bytes = 0;     // payload bytes taken off the socket
        std::uint64_t calls = 0;     // recv() calls it took
        std::uint64_t reads = 0;     // Link::Read() calls (one per header, envelope or chunk)
        std::uint64_t nsInRecv = 0;  // wall time inside recv - blocking included
        std::uint64_t nsTotal = 0;   // wall time inside Read
    };
    LinkIoStats LinkMetricsTakeReadStats();
    // The client's half: bytes handed to the io thread's sendmsg, the calls it took, and the
    // wall time inside sendmsg (blocking included). nsInRecv carries the send time; reads is
    // unused on this side.
    LinkIoStats LinkMetricsTakeSendStats();
}
