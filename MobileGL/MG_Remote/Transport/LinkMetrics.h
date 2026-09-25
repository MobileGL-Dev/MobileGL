// P6.5 transport measurements. Called only by the single client producer.
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
}
