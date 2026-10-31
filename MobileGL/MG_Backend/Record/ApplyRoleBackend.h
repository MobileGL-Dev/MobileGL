// MobileGL - MobileGL/MG_Backend/Record/ApplyRoleBackend.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: WHOSE BACKEND ANSWERS A CAPABILITY QUESTION ON THE APPLY SIDE. Under a transport the
// server's own backend (ServerLoop's) is the authority for the records it applies; the record arm
// asked MG_Remote for it by name, which a library without MG_Remote cannot. The answer now comes
// through this hook, which ServerLoop.cpp installs at static init. Without it - a library without
// a transport - the answer is null, which is exactly what a monolith process got before (a
// monolith has no server loop, so its server backend was always null).

#pragma once

namespace MobileGL::MG_Backend {
    class BackendObject;

    using ApplyRoleBackendHook = BackendObject* (*)();
    inline constinit ApplyRoleBackendHook gApplyRoleBackendHook = nullptr;

    inline BackendObject* ApplyRoleBackend() {
        return gApplyRoleBackendHook != nullptr ? gApplyRoleBackendHook() : nullptr;
    }
} // namespace MobileGL::MG_Backend
