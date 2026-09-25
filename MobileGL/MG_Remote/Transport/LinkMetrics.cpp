// SPDX-License-Identifier: LGPL-3.0-only
#include "LinkMetrics.h"
#include <MG_Util/Debug/Log.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <time.h>
#endif

namespace MobileGL::MG_Remote::Transport {
namespace {
    struct Window {
        std::uint64_t waitReplies = 0, samples = 0, replyNs = 0, stageBytes = 0;
        std::array<std::uint64_t, 32> histogram{};
        // One slot per wire op, plus a last slot for anything past the table. See
        // LinkMetricsMaxOps: the point is that the per-op numbers still SUM to waitReplies.
        std::array<std::uint64_t, LinkMetricsMaxOps> waitRepliesByOp{};
    };
    // The client already has exactly one record producer. The server never writes these.
    struct Metrics {
        bool active = false;
        std::uint64_t frame = 0, started = 0, lastFrame = 0, cpuStarted = 0, cpuLast = 0;
        Window total{}, current{};
    } metrics;
    std::uint64_t ClockNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::uint64_t ThreadCpuNs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec value{};
        if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
            return std::uint64_t(value.tv_sec) * 1000000000ull + value.tv_nsec;
#endif
        return 0;
    }
    std::uint64_t QuantileUpperUs(const Window& w, unsigned percentile) {
        if (w.samples == 0) return 0;
        const auto target = (w.samples * percentile + 99) / 100;
        std::uint64_t count = 0;
        for (unsigned i = 0; i < w.histogram.size(); ++i) {
            count += w.histogram[i];
            if (count >= target) return 1ull << i;
        }
        return 1ull << 31;
    }
    void Emit(const char* kind, const Window& w, std::uint64_t wallNs, std::uint64_t cpuNs) {
        char histogram[768]{};
        std::size_t at = 0;
        for (unsigned i = 0; i < w.histogram.size(); ++i) {
            const int n = std::snprintf(histogram + at, sizeof(histogram) - at, "%s%llu",
                i == 0 ? "" : ",", static_cast<unsigned long long>(w.histogram[i]));
            if (n < 0 || static_cast<std::size_t>(n) >= sizeof(histogram) - at) break;
            at += static_cast<std::size_t>(n);
        }
        MGLOG_I("P65LinkMetrics kind=%s frame=%llu wait_replies=%llu rtt_samples=%llu "
                "rtt_mean_us=%.3f rtt_p50_upper_us=%llu rtt_p99_upper_us=%llu "
                "stage_bytes=%llu wall_ns=%llu client_thread_cpu_ns=%llu rtt_hist_us_pow2=%s",
            kind, static_cast<unsigned long long>(metrics.frame),
            static_cast<unsigned long long>(w.waitReplies), static_cast<unsigned long long>(w.samples),
            w.samples == 0 ? 0.0 : double(w.replyNs) / double(w.samples) / 1000.0,
            static_cast<unsigned long long>(QuantileUpperUs(w, 50)),
            static_cast<unsigned long long>(QuantileUpperUs(w, 99)),
            static_cast<unsigned long long>(w.stageBytes), static_cast<unsigned long long>(wallNs),
            static_cast<unsigned long long>(cpuNs), histogram);
    }
}
void LinkMetricsBegin() {
    metrics = {};
    const char* enabled = std::getenv("MOBILEGL_PIPE_STATS");
    metrics.active = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    if (!metrics.active) return;
    metrics.started = metrics.lastFrame = ClockNs();
    metrics.cpuStarted = metrics.cpuLast = ThreadCpuNs();
}
std::uint64_t LinkMetricsBeginReply(bool wantsReply, std::uint32_t op) {
    if (!metrics.active || !wantsReply) return 0;
    const std::uint32_t slot = op < LinkMetricsMaxOps ? op : LinkMetricsMaxOps - 1;
    ++metrics.current.waitReplies; ++metrics.total.waitReplies;
    ++metrics.current.waitRepliesByOp[slot]; ++metrics.total.waitRepliesByOp[slot];
    return ClockNs();
}
void LinkMetricsReplyApplied(std::uint64_t startedNs) {
    if (!metrics.active || startedNs == 0) return;
    const auto ns = ClockNs() - startedNs;
    const auto us = (ns + 999) / 1000;
    unsigned bucket = 0;
    while (bucket < 31 && us > (1ull << bucket)) ++bucket;
    for (Window* w : {&metrics.current, &metrics.total}) {
        ++w->samples; w->replyNs += ns; ++w->histogram[bucket];
    }
}
std::uint64_t LinkMetricsReplyWaits() { return metrics.total.waitReplies; }
std::uint64_t LinkMetricsReplyWaitsFor(std::uint32_t op) {
    return metrics.total.waitRepliesByOp[op < LinkMetricsMaxOps ? op : LinkMetricsMaxOps - 1];
}
void LinkMetricsStageBytes(std::uint64_t bytes) {
    if (!metrics.active) return;
    metrics.current.stageBytes += bytes; metrics.total.stageBytes += bytes;
}
void LinkMetricsPresent() {
    if (!metrics.active) return;
    ++metrics.frame;
    const auto now = ClockNs(), cpu = ThreadCpuNs();
    Emit("frame", metrics.current, now - metrics.lastFrame, cpu - metrics.cpuLast);
    metrics.current = {}; metrics.lastFrame = now; metrics.cpuLast = cpu;
}
void LinkMetricsEnd() {
    if (!metrics.active) return;
    Emit("summary", metrics.total, ClockNs() - metrics.started, ThreadCpuNs() - metrics.cpuStarted);
    metrics.active = false;
}
void LinkMetricsServerPresent(std::uint64_t serial) {
    // Independent of the client latch: spawn servers never call LinkMetricsBegin.
    // The baseline belongs to the apply thread, and is discarded if that thread is replaced.
    static thread_local const bool enabled = [] {
        const char* value = std::getenv("MOBILEGL_PIPE_STATS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) return;
    static thread_local std::uint64_t previousSerial = 0, previousWall = 0, previousCpu = 0;
    const auto wall = ClockNs(), cpu = ThreadCpuNs();
    const bool valid = previousWall != 0 && serial > previousSerial && cpu >= previousCpu;
    MGLOG_I("P65ServerMetrics frame=%llu valid=%u wall_ns=%llu apply_thread_cpu_ns=%llu",
        static_cast<unsigned long long>(serial), valid ? 1u : 0u,
        static_cast<unsigned long long>(valid ? wall - previousWall : 0),
        static_cast<unsigned long long>(valid ? cpu - previousCpu : 0));
    previousSerial = serial; previousWall = wall; previousCpu = cpu;
}

}
