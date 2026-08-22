// MobileGL - MobileGL/MG_Util/SelfTest/DriverBugProbes.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Util::SelfTest {
    // ===================== KNOWN DRIVER BUGS =====================
    //
    // THIS IS THE DESIGNATED HOME FOR DRIVER-CAPABILITY LIES.
    //
    // The rest of the POST suite answers a different question: does the extension exist, and
    // does a simple probe show it working. The entries here are not extension questions at
    // all - they are CORE functionality that a driver advertises, accepts without error, and
    // then does not perform. Nothing in an extension string or a limit query says so, which
    // is exactly why each one needs its own executable probe.
    //
    // The inventory comes from CAMPAIGN FINDINGS, not from anything the driver reports.
    //
    // EVERY PROBE MUST CARRY A CONTROL. The geometry entry below is why the rule is written
    // down: the same defect was first characterised as "this driver drops all geometry-stage
    // storage-buffer writes", which would have justified withdrawing
    // GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS entirely. A control showed geometry-stage writes
    // land perfectly well when they precede EmitVertex(), so the limit is not a lie and
    // withdrawing it would have broken shaders that work today. A probe without a control
    // measures a symptom and invites exactly that over-correction.
    //
    // ADDING A SIBLING IS ONE FUNCTION: write an `Optional<DriverBugFinding> ProbeXxx(gl)`
    // that returns nullopt when the driver is not affected, and add it to the table in
    // CollectGlesKnownDriverBugs(). Known siblings still to be probed: the R32F-MSAA swizzle
    // corruption, the image-location-per-name link limit, the cross-stage qualifier merge, and
    // the coherency residual.

    // What MobileGL can do about a bug this device HAS. There is deliberately no "not
    // affected" member: a driver that passes the probe produces no finding at all, so the
    // report only ever lists bugs actually present on this device.
    enum class DriverBugVerdict : Uint8 {
        // A MobileGL quirk repairs or substitutes for the defect and the application sees
        // correct behaviour.
        Fixed,
        // There is no substitute. `detail` says what MobileGL does defensively instead, and
        // what an application can still rely on.
        Unfixable,
    };

    struct DriverBugFinding {
        // Short name of the bug, not of the feature.
        String name;
        DriverBugVerdict verdict = DriverBugVerdict::Unfixable;
        // One line: what the driver does wrong, and what MobileGL does about it.
        String detail;
    };

    // Draws one point through VS+GS+FS whose geometry stage writes two storage buffers: one
    // BEFORE its EmitVertex()/EndPrimitive() and one AFTER. Returns true only when the
    // before-emit write lands and the after-emit write does not.
    //
    // The before-emit write is the control, and it is the whole point of the probe. Adreno 830
    // discards geometry-stage storage writes issued after the last emit while performing the
    // identical write issued before it (measured both ways, and for both point and triangle
    // geometry shaders, so the primitive shape is not the variable). Reading only the
    // after-emit half would say "geometry storage writes do not work on this driver", which is
    // false and would justify withdrawing a limit applications legitimately use.
    //
    // Deterministic by construction - the write either reaches memory or the driver
    // structurally discards it - so the answer is latched, not sampled. Returns false when the
    // driver advertises no geometry storage blocks, when an entry point is missing, or when
    // anything about the probe fails to set up: an inconclusive probe must never be reported
    // as a bug. Restores every piece of GL state it touches.
    Bool ProbeGeometryStageSsboWriteAfterEmitDropped(const MG_External::GLESFunctionsTable& gl);

    // ProbeGeometryStageSsboWriteAfterEmitDropped(), evaluated at most once per process.
    Bool GeometryStageSsboWriteAfterEmitDropped(const MG_External::GLESFunctionsTable& gl);

    // Every known driver bug this GLES driver actually has. Bugs it does not have are absent,
    // so an unaffected device renders an empty section rather than a wall of "not affected".
    Vector<DriverBugFinding> CollectGlesKnownDriverBugs(const MG_External::GLESFunctionsTable& gl);
} // namespace MobileGL::MG_Util::SelfTest
