// MobileGL - MobileGL/MG_Test/Backend/GpuHangWatchTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE GPU HANG WATCH (MG_Backend/GpuHangWatch.h): which session's submission stops the GPU.
//
// While one submission hangs, EVERY session's work is pending - the compositor's included - so the
// attribution may only rest on "the GPU started it and it did not finish". These cases pin that:
//   * the submission seen running past the budget is named, and only once;
//   * a submission queued behind it (never started) is never named, however long it waits - red with
//     the Detector counting pending-but-not-started work as running (the compositor would be blamed);
//   * a long but finishing submission is never named;
//   * the budget runs from the first sample that saw it running, not from submission;
//   * on the watch thread: a fake device whose submission never finishes is named within budget plus
//     one interval, an innocent device waiting behind it is not, and Unregister waits out a sample.

#include <gtest/gtest.h>

#include <MG_Backend/GpuHangWatch.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace MobileGL;
namespace Watch = MobileGL::MG_Backend::GpuHangWatch;

namespace {
    Vector<Watch::SubmissionProgress> One(Uint64 id, Bool started, Bool finished) {
        return {Watch::SubmissionProgress{id, started, finished}};
    }
} // namespace

TEST(GpuHangDetectorTest, ASubmissionRunningPastTheBudgetIsNamed) {
    Watch::Detector detector(2000);
    EXPECT_EQ(detector.Observe(One(7, true, false), 10000), 0u);
    EXPECT_EQ(detector.Observe(One(7, true, false), 11000), 0u);
    EXPECT_EQ(detector.Observe(One(7, true, false), 11999), 0u);
    EXPECT_EQ(detector.Observe(One(7, true, false), 12000), 7u);
    EXPECT_EQ(detector.RunningMs(7, 12000), 2000u);
}

TEST(GpuHangDetectorTest, ASubmissionQueuedBehindTheHangIsNeverNamed) {
    // The compositor's frame waits behind a client's hang for the whole ~10 s stall: never started.
    Watch::Detector detector(2000);
    for (Uint64 now = 0; now <= 12000; now += 250) {
        const Vector<Watch::SubmissionProgress> sample = {{1, true, false}, {2, false, false}};
        const Uint64 named = detector.Observe(sample, now);
        if (named != 0) {
            EXPECT_EQ(named, 1u) << "the queued submission was blamed at " << now << " ms";
        }
    }
    Watch::Detector innocentOnly(2000);
    for (Uint64 now = 0; now <= 12000; now += 250)
        EXPECT_EQ(innocentOnly.Observe(One(2, false, false), now), 0u) << "at " << now << " ms";
}

TEST(GpuHangDetectorTest, ALongSubmissionThatFinishesIsNeverNamed) {
    Watch::Detector detector(2000);
    EXPECT_EQ(detector.Observe(One(3, true, false), 0), 0u);
    EXPECT_EQ(detector.Observe(One(3, true, false), 1900), 0u);
    EXPECT_EQ(detector.Observe(One(3, true, true), 2100), 0u);
    // Gone from the sample once retired: forgotten, so a new submission starts its own clock.
    EXPECT_EQ(detector.Observe({}, 2200), 0u);
    EXPECT_EQ(detector.Observe(One(4, true, false), 2300), 0u);
    EXPECT_EQ(detector.Observe(One(4, true, false), 4200), 0u);
    EXPECT_EQ(detector.Observe(One(4, true, false), 4300), 4u);
}

TEST(GpuHangDetectorTest, TheBudgetRunsFromTheFirstSampleThatSawItRunning) {
    Watch::Detector detector(2000);
    // Submitted and queued for 5 s (another process's work), then started.
    EXPECT_EQ(detector.Observe(One(9, false, false), 0), 0u);
    EXPECT_EQ(detector.Observe(One(9, false, false), 5000), 0u);
    EXPECT_EQ(detector.Observe(One(9, true, false), 5250), 0u);
    EXPECT_EQ(detector.Observe(One(9, true, false), 7000), 0u);
    EXPECT_EQ(detector.Observe(One(9, true, false), 7250), 9u);
}

TEST(GpuHangDetectorTest, AZeroBudgetNamesNothing) {
    Watch::Detector detector(0);
    for (Uint64 now = 0; now <= 60000; now += 1000) EXPECT_EQ(detector.Observe(One(1, true, false), now), 0u);
}

namespace {
    // A device double: its one submission's progress, and what the watch told it.
    struct FakeDevice {
        std::atomic<Bool> started{false};
        std::atomic<Bool> finished{false};
        std::atomic<int> samples{0};
        std::atomic<int> named{0};
        std::atomic<Uint64> namedId{0};
        std::atomic<int> sampleDelayMs{0};

        static Bool Sample(void* self, Vector<Watch::SubmissionProgress>& out) {
            auto* device = static_cast<FakeDevice*>(self);
            device->samples.fetch_add(1);
            if (const int delay = device->sampleDelayMs.load(); delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            out.push_back({42, device->started.load(), device->finished.load()});
            return true;
        }
        static void Hung(void* self, Uint64 id, Uint64) {
            auto* device = static_cast<FakeDevice*>(self);
            device->namedId.store(id);
            device->named.fetch_add(1);
        }
    };

    template <typename F>
    Bool PollUntil(F condition, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    }

    struct ScopedWatchTiming {
        ScopedWatchTiming(Uint32 pollMs, Uint32 budgetMs) {
            Watch::SetPollIntervalForTesting(pollMs);
            Watch::SetBudgetForTesting(budgetMs);
        }
        ~ScopedWatchTiming() {
            Watch::SetPollIntervalForTesting(0);
            Watch::SetBudgetForTesting(0);
        }
    };
} // namespace

TEST(GpuHangWatchThreadTest, TheHangingDeviceIsNamedOnceAndTheOneWaitingBehindItIsNot) {
    const ScopedWatchTiming timing(20, 200);
    FakeDevice guilty;
    FakeDevice innocent;
    guilty.started = true;   // the GPU runs it and it never ends
    innocent.started = false; // queued behind it for the whole stall
    const Uint64 a = Watch::Register(&guilty, &FakeDevice::Sample, &FakeDevice::Hung);
    const Uint64 b = Watch::Register(&innocent, &FakeDevice::Sample, &FakeDevice::Hung);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(PollUntil([&] { return guilty.named.load() > 0; }, 3000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(ms, 180) << "named before the budget ran out";
    EXPECT_LE(ms, 1500) << "named far past budget + interval";
    EXPECT_EQ(guilty.namedId.load(), 42u);
    // Long after, still once, and the innocent never.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(guilty.named.load(), 1);
    EXPECT_EQ(innocent.named.load(), 0);
    EXPECT_GT(innocent.samples.load(), 5) << "the innocent device was not even sampled";
    Watch::Unregister(a);
    Watch::Unregister(b);
}

TEST(GpuHangWatchThreadTest, UnregisterWaitsOutASampleInProgressAndNothingRunsAfterIt) {
    const ScopedWatchTiming timing(10, 5000);
    FakeDevice device;
    device.sampleDelayMs = 100;
    const Uint64 handle = Watch::Register(&device, &FakeDevice::Sample, &FakeDevice::Hung);
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(PollUntil([&] { return device.samples.load() > 0; }, 2000));
    Watch::Unregister(handle);
    const int after = device.samples.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(device.samples.load(), after) << "the watch sampled a device after it unregistered";
}

TEST(GpuHangWatchThreadTest, ARegistrationWithoutASamplerIsRefused) {
    EXPECT_EQ(Watch::Register(nullptr, nullptr, nullptr), 0u);
    Watch::Unregister(0); // and unregistering "nothing" is harmless
}
