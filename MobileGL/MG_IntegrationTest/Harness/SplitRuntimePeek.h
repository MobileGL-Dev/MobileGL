// MobileGL - MobileGL/MG_IntegrationTest/Harness/SplitRuntimePeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// WHETHER THIS PROCESS IS REALLY RUNNING SPLIT, asked of the process rather than of the source
// tree. This is what arms every `DirectGLES.Split.` entry.
//
// WHY IT REPLACED A CONTENT PROBE, and the lesson is worth the paragraph. The first version of
// this arming condition was a CMake `file(STRINGS ... REGEX)` conjunction: "some source under
// MG_Remote/Client names the c1 symbols AND no source under Client or Server still matches
// `Fatal.Unimplemented`". Review finding M-1 did not argue with it, it PERFORMED it: a
// `sed -i 's/Fatal{Unimplemented/Fatal{NotYetImplemented/'` over c0's six stub files - every
// entry point still ending in std::abort(), RemoteEmitTable() still aborting on sight,
// ImplementedVerbCount() still returning 0 - armed all eleven lanes and EIGHT OF THEM WENT GREEN
// having run monolith end to end. A comment line containing the marker did the opposite and kept
// them dark forever (M-2), and two further stub files outside the two probed directories made a
// partial arm possible (M-3).
//
// The general form of that defect: A STATEMENT ABOUT SOURCE TEXT CAN ALWAYS BE FALSIFIED BY
// EDITING SOURCE TEXT, and the people most likely to edit it are the ones landing the packages
// the probe is watching for. A statement about what the process actually did cannot. So the three
// facts below are read out of the running process, and each is structurally impossible in a
// monolith build:
//
//   1. `MG_Config::Transport != Monolith`. In a build without MOBILEGL_BUILD_DISAGGREGATED,
//      `Transport` is a `constexpr` Monolith (Config.h:514) and this whole translation unit is
//      compiled out. Read from the VARIABLE, never from a log line - the log-grep spelling of
//      this question is satisfied by a DEBUG-level pull build's `Config: Accepted env variable:
//      MOBILEGL_TRANSPORT=inproc` (review finding M-5).
//   2. `ClientSession::Active() != nullptr`. c0 made this one deliberately return null rather
//      than Fatal, because "does a session exist" has a legitimate "no" - it is the monolith
//      answer (ClientSession.cpp:31). So it is exactly "a client session exists in this process",
//      and no amount of editing stub MESSAGES makes a null pointer non-null.
//   3. `ImplementedVerbCount() > 0`. c0's stub returns 0; the contract gives this function the
//      job of making "a table that silently lost an emitter" distinguishable from "a table that
//      never had one" (EmitTables.h). Zero means there is no emitter to test.
//
// And one BEHAVIOURAL fact, which is the half that says the run went through the wire rather than
// merely that it could have: `ClientSession::Active()->Encoder().EmitSeq()`, the highest record
// ordinal this client has produced. A scenario that armed, drew, and emitted nothing has a
// sequence that did not move, and that is the shape of an emit table that resolves the transport
// and then falls through to the driver.
//
// Every entry point returns false, touching nothing, where the state is out of reach: in a build
// that never compiled MG_Remote, and on Android where this module links the shipping
// libMobileGL.so built -fvisibility=hidden. A caller that gets false must SKIP.

#pragma once

#include <string>

namespace MGITest {

    // What this process can say about itself. Every field is false/0 where the peek cannot look.
    struct SplitRuntimeState {
        // The peek is compiled in at all (MOBILEGL_BUILD_DISAGGREGATED, not Android).
        bool peekAvailable = false;
        // MG_Config::Transport != Monolith - this process RESOLVED a split transport.
        bool transportResolved = false;
        // The resolved transport, for a message: "monolith", "inproc", "spawn", "unix", "pipe".
        std::string transportName = "monolith";
        // ClientSession::Active() != nullptr.
        bool sessionActive = false;
        // ImplementedVerbCount(), out of kRemoteEmitSlotCount (71).
        unsigned int implementedVerbs = 0;
        unsigned int totalVerbSlots = 0;
        // The encoder's highest produced record ordinal, or 0 when there is no session.
        unsigned long long emitSeq = 0;
    };

    SplitRuntimeState PeekSplitRuntime();

    // Empty when this process is a real split run that can be asserted about; otherwise the
    // reason to GTEST_SKIP() with, naming the first fact that is not true and the package that
    // owns it.
    std::string SplitRuntimeSkipReason();

} // namespace MGITest
