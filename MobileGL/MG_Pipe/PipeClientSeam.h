// MobileGL - MobileGL/MG_Pipe/PipeClientSeam.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: THE CLIENT SESSION'S ANSWERS THE RECORD ARM ASKS FOR, WITHOUT NAMING MG_Remote. The record
// arm's client half (the persistent-map tracker, the verb port) runs in a library without a
// transport too; there the answers are the monolith's (no session, so nothing was lost). With
// MG_Remote linked, ClientSession.cpp installs the real probe at static init.

#pragma once

#include <Includes.h>

namespace MobileGL::MG_Pipe {

    // Has the calling process's client session latched a lost device (ClientSession::DeviceLost)?
    using MGPipeClientDeviceLostProbe = Bool (*)();
    void MGPipeSetClientDeviceLostProbe(MGPipeClientDeviceLostProbe probe);
    Bool MGPipeClientDeviceLost();

    // A declined readback asks the session, briefly, whether a device loss is about to be
    // reported (ClientSession::ConfirmLossAfterDecline). Without a session the answer is no.
    using MGPipeClientLossConfirmer = Bool (*)(Uint32 waitMs);
    void MGPipeSetClientLossConfirmer(MGPipeClientLossConfirmer confirmer);
    Bool MGPipeClientConfirmLossAfterDecline(Uint32 waitMs);

} // namespace MobileGL::MG_Pipe
