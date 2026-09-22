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
    void LinkMetricsPresent();
}
