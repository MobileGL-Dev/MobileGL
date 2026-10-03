// MobileGL - MobileGL/MG_Backend/GpuHangWatch.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// A SESSION WHOSE GPU WORK NEVER ENDS IS FOUND AND NAMED, SO IT CANNOT HANG THE GPU AGAIN.
//
// A submission whose shader never ends stops the whole GPU, not one context: every process's work -
// the compositor's, other clients', processes outside MobileGL at the highest context priority -
// waits until the kernel resets it. Measured on Adreno 750: ~2 s when nothing of higher priority
// waits (the context is then lost: Vulkan answers VK_ERROR_DEVICE_LOST), ~10 s when something of
// higher priority does (a failed preemption; the context is NOT lost, and its next frame hangs the
// GPU again). Nothing in user space can shorten one such stall; what the server can do is make sure
// the session that caused it never submits again, so a page or program that hangs every frame
// costs the desktop one stall instead of one per frame.
//
// The attribution must be exact - every session's work is pending while the GPU is stuck, and the
// compositor's must never be blamed. A backend that can tell, per in-flight submission, whether the
// GPU has STARTED it and whether it has FINISHED it gives exactly that: the GPU runs one submission
// at a time on a ring, so the one that started and does not finish is the one that hangs, and the
// ones queued behind it have not started. Magma brackets each submission with two events
// (DirectVulkan/Renderer/GpuProgressMarkers.h). Espryt relies on the driver instead: its contexts ask
// for reset notification and glGetGraphicsResetStatus names the guilty one - measured after a ~2 s
// timeout reset; after a ~10 s failed-preemption reset the server's contexts were NOT told (a
// standalone process's context was), so on Espryt that path is not contained yet.
//
// The Detector is the decision, pure and testable; the watch thread samples every registered backend
// on its own clock, because the session that hangs is often not running - its apply thread waits for
// its own fence, or for the backend turn another session holds while it waits for the GPU.

#include <Includes.h>

#include <mutex>

namespace MobileGL::MG_Backend::GpuHangWatch {

    // MOBILEGL_GPU_HANG_BUDGET_MS: how long one submission may run once the GPU has started it before
    // its session is ended as the one that hangs the GPU. Default 2000 - the kernel resets a hung GPU
    // by itself after about that when nothing preempts it, and no desktop frame runs that long. 0 = off.
    Uint32 BudgetMs();
    constexpr Uint32 kDefaultBudgetMs = 2000;

    // One in-flight submission as the GPU reports progress on it.
    struct SubmissionProgress {
        Uint64 Id = 0;       // the backend's own monotonic id for it; never reused
        Bool Started = false; // the GPU has begun executing it
        Bool Finished = false; // ... and completed it
    };

    // Which submission, if any, has run past the budget: started, not finished, and seen in that
    // state for at least `budgetMs` (from the first sample that saw it running - the watch cannot know
    // when between two samples it began, so a hang is named between budget and budget + one interval).
    class Detector {
    public:
        explicit Detector(Uint32 budgetMs) : m_budgetMs(budgetMs) {}
        // One sample of every in-flight submission at `nowMs`. Returns the id of the submission that
        // ran past the budget, or 0. Submissions missing from the sample are forgotten.
        Uint64 Observe(const Vector<SubmissionProgress>& sample, Uint64 nowMs);
        // How long the named submission had been seen running when Observe named it.
        Uint64 RunningMs(Uint64 id, Uint64 nowMs) const;

    private:
        struct Running {
            Uint64 Id = 0;
            Uint64 SinceMs = 0;
        };
        Uint32 m_budgetMs = 0;
        Vector<Running> m_running;
    };

    // ---- the process-wide watch ---------------------------------------------------------------
    //
    // A backend registers a sampler and a callback. The watch thread calls the sampler every
    // PollIntervalMs() on its own thread, and the callback once, when the Detector names a
    // submission; the backend acts on it from its own thread (it may not latch a session from this
    // one). Both are called with the watch's lock held: keep them short, and never call Unregister
    // from them. Unregister returns only once no call into the registration runs or will run.
    using SampleFn = Bool (*)(void* self, Vector<SubmissionProgress>& out); // false: stop watching it
    using HungFn = void (*)(void* self, Uint64 submissionId, Uint64 runningMs);

    Uint64 Register(void* self, SampleFn sample, HungFn hung);
    void Unregister(Uint64 handle);

    Uint32 PollIntervalMs();
    // Tests: the interval and the budget the next Register uses (0 restores the defaults).
    void SetPollIntervalForTesting(Uint32 ms);
    void SetBudgetForTesting(Uint32 ms);

} // namespace MobileGL::MG_Backend::GpuHangWatch
