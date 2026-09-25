// P6.5 transport measurements. Called only by the single client producer.
#pragma once
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {
    // Disabled sessions never read a clock. Samples are a fixed 32-bucket histogram.
    void LinkMetricsBegin();
    void LinkMetricsEnd();
    std::uint64_t LinkMetricsBeginReply(bool wantsReply);
    void LinkMetricsReplyApplied(std::uint64_t startedNs);
    void LinkMetricsStageBytes(std::uint64_t bytes);

    // THE SAME NUMBER THE PER-FRAME LINE PRINTS AS `wait_replies` (P65LinkMetrics), read back
    // in-process. P12 item B needs it as a GATE rather than as a log line: "the texture half no
    // longer buys a round trip per record" is otherwise only observable by timing, and a timing
    // is a measurement, not a gate. It reads 0 while MOBILEGL_PIPE_STATS is unset - which is
    // exactly why a case that asserts 0 for one row must ALSO assert that a reply-owning row
    // still counts, or it would pass on a session where nothing was counted at all.
    std::uint64_t LinkMetricsReplyWaits();
    void LinkMetricsPresent();
    void LinkMetricsServerPresent(std::uint64_t serial);
}
