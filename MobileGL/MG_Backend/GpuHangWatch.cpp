// MobileGL - MobileGL/MG_Backend/GpuHangWatch.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GpuHangWatch.h"

#include <MG_Util/Debug/Log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <thread>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#endif

namespace MobileGL::MG_Backend::GpuHangWatch {

    namespace {
        constexpr Uint32 kDefaultPollMs = 250;
        std::atomic<Uint32> g_pollOverrideMs{0};
        std::atomic<Uint32> g_budgetOverrideMs{0};
        std::atomic<Bool> g_budgetOverridden{false};

        Uint64 NowMs() {
            return static_cast<Uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
        }

        Uint32 ReadBudgetMs() {
            const char* value = std::getenv("MOBILEGL_GPU_HANG_BUDGET_MS");
#if defined(__ANDROID__)
            char property[PROP_VALUE_MAX] = {};
            if (value == nullptr && __system_property_get("debug.mobilegl.gpu_hang_budget_ms", property) > 0)
                value = property;
#endif
            if (value == nullptr || *value == '\0') return kDefaultBudgetMs;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value) return kDefaultBudgetMs;
            return static_cast<Uint32>(std::min<unsigned long>(parsed, 600000ul));
        }

        struct Entry {
            Uint64 Handle = 0;
            void* Self = nullptr;
            SampleFn Sample = nullptr;
            HungFn Hung = nullptr;
            Detector Decide{0};
            Bool Done = false; // named a hang, or its device is gone: never sampled again
        };

        // Leaked at exit, with its thread: the watch holds no GPU object of its own, and a backend
        // unregisters before it destroys what its sampler reads.
        struct Watch {
            std::mutex Mutex;
            std::condition_variable Wake;
            Vector<Entry> Entries;
            Uint64 NextHandle = 1;
            Bool ThreadStarted = false;
        };

        Watch& TheWatch() {
            static Watch& watch = *new Watch();
            return watch;
        }

        void PollOnce(Watch& watch) {
            const Uint64 now = NowMs();
            Vector<SubmissionProgress> sample;
            for (Entry& entry : watch.Entries) {
                if (entry.Done) continue;
                sample.clear();
                if (!entry.Sample(entry.Self, sample)) {
                    entry.Done = true;
                    continue;
                }
                const Uint64 hung = entry.Decide.Observe(sample, now);
                if (hung == 0) continue;
                entry.Done = true;
                entry.Hung(entry.Self, hung, entry.Decide.RunningMs(hung, now));
            }
        }

        void WatchThreadMain() {
#if defined(__linux__) || defined(__ANDROID__)
            pthread_setname_np(pthread_self(), "mgl-gpu-watch");
#endif
            Watch& watch = TheWatch();
            std::unique_lock<std::mutex> lock(watch.Mutex);
            for (;;) {
                watch.Wake.wait_for(lock, std::chrono::milliseconds(PollIntervalMs()));
                PollOnce(watch);
            }
        }
    } // namespace

    Uint32 BudgetMs() {
        if (g_budgetOverridden.load(std::memory_order_acquire)) return g_budgetOverrideMs.load(std::memory_order_acquire);
        static const Uint32 budget = ReadBudgetMs();
        return budget;
    }

    Uint32 PollIntervalMs() {
        const Uint32 overrideMs = g_pollOverrideMs.load(std::memory_order_acquire);
        return overrideMs != 0 ? overrideMs : kDefaultPollMs;
    }

    void SetPollIntervalForTesting(Uint32 ms) { g_pollOverrideMs.store(ms, std::memory_order_release); }

    void SetBudgetForTesting(Uint32 ms) {
        g_budgetOverrideMs.store(ms, std::memory_order_release);
        g_budgetOverridden.store(ms != 0, std::memory_order_release);
    }

    Uint64 Detector::Observe(const Vector<SubmissionProgress>& sample, Uint64 nowMs) {
        Vector<Running> still;
        still.reserve(sample.size());
        Uint64 named = 0;
        for (const SubmissionProgress& progress : sample) {
            if (!progress.Started || progress.Finished) continue;
            Uint64 since = nowMs;
            for (const Running& seen : m_running) {
                if (seen.Id == progress.Id) {
                    since = seen.SinceMs;
                    break;
                }
            }
            still.push_back({progress.Id, since});
            if (named == 0 && m_budgetMs != 0 && nowMs - since >= m_budgetMs) named = progress.Id;
        }
        m_running.swap(still);
        return named;
    }

    Uint64 Detector::RunningMs(Uint64 id, Uint64 nowMs) const {
        for (const Running& seen : m_running)
            if (seen.Id == id) return nowMs - seen.SinceMs;
        return 0;
    }

    Uint64 Register(void* self, SampleFn sample, HungFn hung) {
        const Uint32 budget = BudgetMs();
        if (budget == 0 || sample == nullptr || hung == nullptr) return 0;
        Watch& watch = TheWatch();
        const std::lock_guard<std::mutex> lock(watch.Mutex);
        Entry entry;
        entry.Handle = watch.NextHandle++;
        entry.Self = self;
        entry.Sample = sample;
        entry.Hung = hung;
        entry.Decide = Detector(budget);
        watch.Entries.push_back(entry);
        if (!watch.ThreadStarted) {
            watch.ThreadStarted = true;
            std::thread(WatchThreadMain).detach();
        }
        return entry.Handle;
    }

    void Unregister(Uint64 handle) {
        if (handle == 0) return;
        Watch& watch = TheWatch();
        // The poll pass runs under this lock, so taking it waits out any call into the entry.
        const std::lock_guard<std::mutex> lock(watch.Mutex);
        for (SizeT i = 0; i < watch.Entries.size(); ++i) {
            if (watch.Entries[i].Handle != handle) continue;
            watch.Entries.erase(watch.Entries.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }

} // namespace MobileGL::MG_Backend::GpuHangWatch
