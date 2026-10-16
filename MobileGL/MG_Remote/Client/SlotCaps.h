// MobileGL - MobileGL/MG_Remote/Client/SlotCaps.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Capability probes over GLFunctionsTable slots. A call site that tests a slot for null to learn
// whether the backend supports something cannot do so under a transport, where the client's
// emit table has no null slot; it reads a capability bit instead (ARCHITECTURE.md §3.3).
//   MGL_BACKEND_SLOT_CAP(Slot, CapBit): split reads the server's CapBit; monolith tests the slot.
//   MGL_BACKEND_SLOT_LOCAL(Slot): no published bit; split answers "absent" so the site's fallback
//     runs. Use it only where that fallback is correct, not silently wrong (the XFB capture-order
//     probe reads kCapBackendOwnsXfbCapture for that reason). Monolith tests the slot.
// Without MOBILEGL_BUILD_DISAGGREGATED both expand to the plain null check.

#pragma once
#include <Includes.h>

#include <Config.h>

#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Pipe/MGPipeTypes.h>
#include <MG_Remote/Client/CapsMirror.h>

#define MGL_BACKEND_SLOT_CAP(Slot, CapBit)                                                         \
    (::MobileGL::MG_Config::Transport != ::MobileGL::MG_Config::TransportMode::Monolith             \
         ? ::MobileGL::MG_Remote::Client::CapsMirrorInstance().HasCap(CapBit)                       \
         : ::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot != nullptr)

#define MGL_BACKEND_SLOT_LOCAL(Slot)                                                               \
    (::MobileGL::MG_Config::Transport == ::MobileGL::MG_Config::TransportMode::Monolith &&          \
     ::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot != nullptr)

// The POINTER-valued forms, for the several sites shaped `if (const auto f = TABLE.GL.Slot)`.
// They exist for G1 and for nothing else: a site rewritten from that shape into
// `if (MGL_BACKEND_SLOT_CAP(...)) { const auto f = TABLE.GL.Slot; ... }` is semantically the
// same and generated DIFFERENT CODE - the first measurement of this change moved
// GL_Getter.cpp's GetInteger64v by -150 bytes and GetIntegerv by +2, which is two "resized"
// symbols and a red G1. In a pull build these two expand to the slot expression ITSELF, so the
// init-statement survives verbatim and the pull build's code generation cannot move.
#define MGL_BACKEND_SLOT_PTR_CAP(Slot, CapBit)                                                     \
    (::MobileGL::MG_Config::Transport != ::MobileGL::MG_Config::TransportMode::Monolith             \
         ? (::MobileGL::MG_Remote::Client::CapsMirrorInstance().HasCap(CapBit)                      \
                ? ::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot                            \
                : nullptr)                                                                          \
         : ::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot)

#define MGL_BACKEND_SLOT_PTR_LOCAL(Slot)                                                           \
    (::MobileGL::MG_Config::Transport != ::MobileGL::MG_Config::TransportMode::Monolith             \
         ? nullptr                                                                                  \
         : ::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot)

#else

#define MGL_BACKEND_SLOT_CAP(Slot, CapBit)                                                         \
    (::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot != nullptr)
#define MGL_BACKEND_SLOT_LOCAL(Slot)                                                               \
    (::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot != nullptr)
#define MGL_BACKEND_SLOT_PTR_CAP(Slot, CapBit)                                                     \
    (::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot)
#define MGL_BACKEND_SLOT_PTR_LOCAL(Slot)                                                           \
    (::MobileGL::MG_Backend::gBackendFunctionsTable.GL.Slot)

#endif
