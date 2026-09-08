// MobileGL - MobileGL/MG_Util/SelfTest/PersistentBufferOrderingProbe.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "DriverBugProbes.h"

#include <array>

namespace MobileGL::MG_Util::SelfTest {
    enum class BufferOrderingProbeStatus : Uint8 { NotRun, Complete, Failed };

    struct BufferOrderingSample {
        BufferOrderingProbeStatus status = BufferOrderingProbeStatus::NotRun;
        Uint frames = 0; // Independent FBO readbacks, not draw calls or application frames.
        Uint badFrames = 0;
        Uint badComponents = 0;
        GLenum error = GL_NO_ERROR;

        Bool Passed() const { return status == BufferOrderingProbeStatus::Complete && badFrames == 0; }
    };

    struct BufferOrderingUploadMeasurement {
        BufferOrderingSample unmapped;
        BufferOrderingSample mapped;
        BufferOrderingSample finishBefore;
        BufferOrderingSample finishBoth;
        BufferOrderingSample mapThenUnmap;
        BufferOrderingSample barrierBefore;

        Bool Detected() const {
            return unmapped.Passed() && finishBoth.Passed() &&
                   mapped.status == BufferOrderingProbeStatus::Complete && mapped.badFrames != 0;
        }
    };

    struct PersistentBufferOrderingMeasurement {
        Bool supported = false;
        // SubData; CopyBufferSubData from coherent persistent staging; CopyBufferSubData
        // from ordinary SubData staging. Each has its OWN otherwise-identical controls.
        std::array<BufferOrderingUploadMeasurement, 3> uploads;
    };

    // POST-only: native GLES calls, no MobileGL buffers, renderer-name rules or config changes.
    // The Mali r54p1 finding: updating an immutable vertex arena that has been persistently
    // mapped can corrupt queued draws even when the application never accesses that mapping.
    // Queue eight update/draw pairs into separate FBOs BEFORE any Finish/readback, then check
    // every pixel of both old and new draws. Staging slots never overlap while in flight.
    //
    // Each upload runs a never-mapped control with identical storage flags. Try up to three
    // fresh mapped allocations to catch intermittent failures. On corruption, measure explicit
    // waits, map-then-unmap and a barrier as diagnostics. Only a passing never-mapped AND
    // Finish-before-and-after control permits a finding. Setup/GL failures are inconclusive.
    // Explicit allocations are one 128 MiB arena, its initializer, and small staging/FBOs;
    // allocations, batches and draws are bounded. Every touched GL state is restored.
    PersistentBufferOrderingMeasurement ProbePersistentBufferUpdateOrdering(
        const MG_External::GLESFunctionsTable& gl);

    // Used by the POST collector. A report never labels an inconclusive sample as a bug.
    Optional<DriverBugFinding> DescribePersistentBufferOrderingBug(
        const PersistentBufferOrderingMeasurement& measurement);
} // namespace MobileGL::MG_Util::SelfTest
