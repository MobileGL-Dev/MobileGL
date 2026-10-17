// MobileGL - MobileGL/MG_Pipe/PipeDeviceLossKnobs.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The device-loss debug knobs (MG_Pipe/PipeSessionFail.h): a device check, or one client's
// session, reported lost on demand, so the containment of a real loss can be driven without a GPU
// that faults.

#include <Includes.h>
#include <MG_Pipe/PipeSessionFail.h>
#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace MobileGL::MG_Pipe {
#if MOBILEGL_BUILD_DISAGGREGATED
    bool MGPipeDebugDeviceLossDue() {
        long at = 0;
        if (const char* value = std::getenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT")) {
            at = std::strtol(value, nullptr, 10);
        }
#if defined(__ANDROID__)
        else {
            char property[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.mobilegl.inject_device_lost_at", property) > 0) {
                at = std::strtol(property, nullptr, 10);
            }
        }
#endif
        static std::atomic<long> armedAt{0};
        static std::atomic<long> seen{0};
        static std::atomic<bool> fired{false};
        if (at <= 0) {
            // Unset (or 0) disarms: the same value set again counts afresh.
            if (armedAt.load(std::memory_order_relaxed) != 0) armedAt.store(0, std::memory_order_relaxed);
            return false;
        }
        // A NEW VALUE RE-ARMS. One server process outlives many sessions, so a second loss is
        // injected by setting the knob again (another value, or 0 and then a value): the count
        // restarts from the checks made after it was seen.
        if (long previous = armedAt.load(std::memory_order_acquire);
            previous != at && armedAt.compare_exchange_strong(previous, at, std::memory_order_acq_rel)) {
            seen.store(0, std::memory_order_release);
            fired.store(false, std::memory_order_release);
        }
        if (fired.load(std::memory_order_acquire)) return false;
        if (seen.fetch_add(1, std::memory_order_acq_rel) + 1 < at) return false;
        if (fired.exchange(true, std::memory_order_acq_rel)) return false;
        MGLOG_E("MGPipe: debug knob inject_device_lost_at=%ld - this device check reports the device LOST", at);
        return true;
    }

    bool MGPipeDebugSessionLossDue(unsigned clientPid) {
        if (clientPid == 0) return false;
        // The first call on a thread only starts the clock, so a session is first asked 100 ms
        // after it started rendering - never in the middle of its first make-current.
        thread_local auto lastAsked = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (lastAsked == std::chrono::steady_clock::time_point{}) {
            lastAsked = now;
            return false;
        }
        if (now - lastAsked < std::chrono::milliseconds(100)) return false;
        lastAsked = now;
        long target = 0;
        if (const char* value = std::getenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_PID")) {
            target = std::strtol(value, nullptr, 10);
        }
#if defined(__ANDROID__)
        else {
            char property[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.mobilegl.inject_device_lost_pid", property) > 0) {
                target = std::strtol(property, nullptr, 10);
            }
        }
#endif
        // Once per value: the session it hit is over, and a later session of the same process (the
        // recovery's) must not be lost again by the same setting. Another pid - or unset/0 and
        // then the same one - re-arms it.
        static std::atomic<long> firedFor{0};
        if (target <= 0) {
            if (firedFor.load(std::memory_order_relaxed) != 0) firedFor.store(0, std::memory_order_relaxed);
            return false;
        }
        if (static_cast<unsigned long>(target) != clientPid) return false;
        long previous = firedFor.load(std::memory_order_acquire);
        if (previous == target || !firedFor.compare_exchange_strong(previous, target, std::memory_order_acq_rel)) {
            return false;
        }
        MGLOG_E("MGPipe: debug knob inject_device_lost_pid=%ld - this session's device is reported LOST", target);
        return true;
    }

#endif // MOBILEGL_BUILD_DISAGGREGATED
} // namespace MobileGL::MG_Pipe
