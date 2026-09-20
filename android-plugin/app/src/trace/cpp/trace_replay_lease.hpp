#pragma once

#include <atomic>

namespace mobilegl_trace {

// apitrace, its GLWS adapter and the environment are process-global. --singlethread
// controls trace dispatch only; it does not make two JNI invocations independent.
class TraceReplayLease {
public:
    TraceReplayLease() : acquired_(!running_.test_and_set(std::memory_order_acquire)) {}
    ~TraceReplayLease() {
        if (acquired_) {
            running_.clear(std::memory_order_release);
        }
    }

    TraceReplayLease(const TraceReplayLease&) = delete;
    TraceReplayLease& operator=(const TraceReplayLease&) = delete;

    explicit operator bool() const { return acquired_; }

private:
    inline static std::atomic_flag running_ = ATOMIC_FLAG_INIT;
    const bool acquired_;
};

} // namespace mobilegl_trace
