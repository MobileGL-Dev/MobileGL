// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <Config.h>
#if MOBILEGL_PIPE_PUSH
// kMGPipeSubsystem* - the runtime bitmask's named bits. Push-only, so the pull build's
// include graph is unchanged.
#include <MG_Pipe/MGPipe.h>
#endif

#include <cstdlib>

// Magma's arm selector for the P2 Track H / render-state re-keys (P2 brief D14).
//
// Two switches decide which arm a re-keyed site runs, and they are NOT the same switch:
//
//   MOBILEGL_PIPE_PUSH (compile)          - is the pushed state there to be keyed on at all
//   Features.PipePush  (runtime bitmask)  - is THIS subsystem migrated in THIS run
//   MOBILEGL_PIPE_LEGACY_MEMOS (compile)  - is the pre-handle arm compiled beside it
//   Features.PipeLegacyMemos (runtime)    - may the pre-handle arm be ENTERED in this run
//
// ARCHITECTURE.md 9.6's point: once a handle wave lands, a clear MOBILEGL_PIPE_PUSH bit is
// only a valid A/B while the legacy arm is still compiled, because with the bit clear the
// backend would otherwise still run the re-keyed code. So a clear bit selects the legacy
// arm, and a run that has explicitly disabled the legacy arm may not fall into it.
//
// The whole header is inert in a pull build: MOBILEGL_PIPE_PUSH is 0 there, every helper
// below is behind it, and the pull build's translation units are byte-identical (G1).
namespace MobileGL::MG_Backend::DirectVulkan {

#if MOBILEGL_PIPE_PUSH
    // Is `subsystemBit` (MG_Pipe/MGPipe.h's kMGPipeSubsystem*) migrated in this run?
    inline Bool MagmaPipeSubsystemOn(Uint64 subsystemBit) {
        return (MG_Config::Features.PipePush & subsystemBit) != 0;
    }

    // The legacy arm is about to be entered. Features.PipeLegacyMemos=0 is the operator
    // asserting "the pre-handle arm is never entered in this run", which is the lever
    // HandleRecycleScenario.Handles pulls (P2 brief D18): entering it anyway would make
    // that arm green for the wrong reason, so it is Fatal rather than a fallback.
    inline void MagmaPipeRequireLegacyArm(const char* site) {
#if MOBILEGL_PIPE_LEGACY_MEMOS
        if (MG_Config::Features.PipeLegacyMemos) return;
        MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled} %s wanted the pre-handle arm but "
                "MOBILEGL_PIPE_LEGACY_MEMOS=0 forbids entering it",
                site);
#else
        MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled} %s wanted the pre-handle arm but this "
                "build did not compile one (cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF)",
                site);
#endif
        std::abort();
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace MobileGL::MG_Backend::DirectVulkan
