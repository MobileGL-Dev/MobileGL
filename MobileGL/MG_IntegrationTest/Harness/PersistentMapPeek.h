// MobileGL - MobileGL/MG_IntegrationTest/Harness/PersistentMapPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// WHICH ARM A PERSISTENT|WRITE|COHERENT MAP LANDED IN, read from a scenario.
//
// It exists for exit gate E3's first assertion, and package b1 (b1-v1.md 4.1) names the
// spelling: `IsBackendPersistentMapped()` must be FALSE, i.e. the store was NOT adopted and the
// CPU shadow is still the source of truth, i.e. the emulated arm. The question has no answer in
// the GL API at all - both arms map, both arms take the application's writes, both arms draw the
// same pixels - so a scenario with no peek is a scenario that silently tests whichever arm the
// driver and the build happened to choose. `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` does not
// separate them either: it is read only inside TryAdoptLargeStorage, and a scenario-sized buffer
// never reaches the 16 MiB threshold that calls it.
//
// A separate translation unit for BackendCapsPeek.h's reason, verbatim: the scenario sources
// include the GL headers with prototypes and MobileGL's umbrella header is not meant to meet them
// in one file.
//
// Every entry point returns FALSE, touching nothing, where the state is out of reach - on Android
// this module links the shipping libMobileGL.so built -fvisibility=hidden, so no internal symbol
// resolves, and the tracker half additionally needs a build that compiled MG_Remote/Client. A
// caller that gets false must SKIP rather than pass: "could not look" is not "it was emulated".

#pragma once

namespace MGITest {

    // True when the peek can answer at all in this build. A scenario asks this first so that its
    // skip message can name WHY it could not look.
    bool PersistentMapPeekAvailable();

    // BufferObject::IsBackendPersistentMapped() for the buffer with this GL name.
    //
    //   returns false -> could not look (no peek in this build, no current context, or no such
    //                    buffer). *outAdopted is untouched.
    //   returns true  -> *outAdopted is true on the ADOPTED arm (the resource owner minted
    //                    host-visible coherent storage and the shadow was released) and false on
    //                    the EMULATED arm (the owner declined; the shadow is the truth and the
    //                    client has to push blocks). R-6 pins the split arm at emulated.
    bool PeekBufferIsAdoptedPersistentMap(unsigned int bufferName, bool* outAdopted);

    // True when this build compiled package b1's client-side persistent-map tracker, so the
    // membership predicate below means something. CMake answers it, by probing MG_Remote/Client
    // for the symbol: a build that never compiled MG_Remote cannot link the call, so the decision
    // has to be made before the compiler sees it rather than by __has_include.
    bool PersistentMapTrackerAvailable();

    // MG_Remote::Client::PersistentMapTracker::IsLivePersistentMap() for this buffer - b1-v1.md
    // 4.1 item 2. The membership set is meant to be exactly the early-out chain of
    // SyncPersistentMappedRange; asking it here is what makes a drift between the two fail in a
    // named test rather than silently stop the push.
    //
    // Same contract as above: false means "could not look".
    bool PeekBufferIsLivePersistentMap(unsigned int bufferName, bool* outLive);

    // Separate-process clients own these counters; the remote Present window
    // belongs to the server and cannot report client map acquisitions or bytes.
    bool PeekSeparateClientMapStats(unsigned long long* acquisitions, unsigned long long* pushedBytes);

} // namespace MGITest
