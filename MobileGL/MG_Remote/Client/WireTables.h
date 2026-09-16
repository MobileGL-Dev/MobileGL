// MobileGL - MobileGL/MG_Remote/Client/WireTables.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE CLIENT ARM OF R-17's ROUTING: the encode twin of `gMGPipeWireRecordApply`. Owner: c1.
//
// Thirty-seven thin emitters over `ClientSession::EmitAndWait`, installed over the two
// generated tables and the escape table that `MG_Pipe/PipeRoute.h` declares, so that under
// split every resource, CSO, texture and program record leaves the GL thread as a WIRE RECORD
// instead of executing synchronously against a context the apply thread now owns.
//
// WHAT ARMS `integration-split`. The 21 `DirectGLES.Split.*` entries skip on
// `ClientSession::Active() == nullptr`. `Install()` below is called from the END of a
// successful `ClientSession::Start()`, on the GL thread (the thread that called
// `MG_Backend::Init()`), AFTER the Hello/Welcome handshake, after the first CapsSnapshot has
// been adopted, and after the in-process server role's apply thread has been started. That
// order is not a preference:
//   - after the handshake, because an emitter that published before Welcome would be writing
//     into a ring the peer has not mapped;
//   - after the first snapshot, because R-8's liveness gates read the caps mirror and a
//     placeholder mirror consumes nothing, so a record emitted before it would be emitted to a
//     server this client has not yet been told consumes that family;
//   - after the apply thread exists, because `EmitAndWait` BLOCKS on `appliedSeq` and nothing
//     would advance it - a barrier wait with no applier is the 30-second Fatal, not a hang;
//   - on the GL thread, because that is the only thread that may touch `gPipeInputs` while the
//     barrier holds (table 3), and installing from the apply thread would publish the table to
//     the GL thread with no synchronisation at all.
// `Uninstall()` runs at the TOP of `Stop()`, before the rings go away, so the last thing any
// straggling GL-thread call reaches is the monolith arm rather than a dangling session.

#pragma once
#include <Includes.h>

#if MOBILEGL_BUILD_DISAGGREGATED

namespace MobileGL::MG_Remote::Client {

    // Installs the thirty-seven wire emitters over gMGPipeScreen / gMGPipeContext /
    // gMGPipeRouteEscapes and records the arm. Idempotent.
    void InstallClientWireTables();

    // Puts the monolith adapters back. Idempotent; safe to call when nothing was installed.
    void UninstallClientWireTables();

    // How many records the thirty-seven emitters have published. This is t1's FOURTH arming
    // fact - "the client encoder's record ordinal actually moving during the case" - counted
    // at the only place that can count it, and it is deliberately NOT the encoder's EmitSeq:
    // EmitSeq moves for the five class-B verbs too, so a lane that armed on it would arm on a
    // Clear and call the resource path proven.
    Uint64 ClientWireRecordsEmitted();

    // How many of those were refused by the server, by acceptance row. Counted rather than
    // inferred, R-8's rule one level out.
    Uint64 ClientWireRecordsDeclined();

} // namespace MobileGL::MG_Remote::Client

#endif // MOBILEGL_BUILD_DISAGGREGATED
