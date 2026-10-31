// MobileGL - MobileGL/MG_Backend/ServerRole.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server role's bring-up, as one entry point two shapes share.
//
// Steps 1 and 2 of Init.cpp's split bring-up - the server's private
// BackendObject, and the two CallMask halves computed from THAT backend's own
// function table. Under `inproc` InitSplitRoles calls it and then starts the
// client in the same process; under `spawn` the child's ServerMain calls it and
// then accepts a connection instead.
//
// ONE ENTRY POINT, not two, because the capability bits are a statement about
// the server's backend and only the server can compute them. A second copy in
// ServerMain would be a second answer to "does this backend own the XFB
// capture", diverging silently the first time a backend registers a slot.

#pragma once

#include <Includes.h>

// DECLARED ONLY IN A DISAGGREGATED BUILD. G1 requires the pull build's symbol
// set and .text to be byte-identical, and a definition guarded only on the
// inside still emits a symbol - measured, not assumed.
#if MOBILEGL_BUILD_DISAGGREGATED

namespace MobileGL::MG_Backend {

    // Creates the server's BackendObject and publishes the consumed-subsystem
    // mask and the capability bits onto ServerSessionInstance(). Does NOT create
    // an EGL context: that happens later, on the apply thread, when the client's
    // first eglMakeCurrent crosses as a blocking control request.
    //
    // Returns false when the backend could not be created, and the caller must
    // then REFUSE TO CONTINUE rather than fall back - a server process that came
    // up without a backend would accept a session it can never apply.
    Bool InitServerRoleForSpawn();

} // namespace MobileGL::MG_Backend

#endif // MOBILEGL_BUILD_DISAGGREGATED
