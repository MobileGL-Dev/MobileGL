// MobileGL - MobileGL/MG_Remote/Client/SlotCaps.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE 41 NULL CHECKS. Owner: package c1 (CONTRACT-P5.md §7, ID-14).
//
// FORTY-ONE OF THE SIXTY-NINE GLFunctionsTable SLOTS ARE NULL-CHECKED AT THEIR CALL SITE, AND
// SEVERAL OF THOSE CHECKS ARE CAPABILITY PROBES RATHER THAN SAFETY CHECKS. R-4 forbids a null
// slot in the client's emit table - so in that table every one of those probes would answer
// "supported", and whatever fallback sits behind it would silently disappear. That is not a
// theoretical risk: it is how a split lane produces a plausible picture for the wrong reason.
// ARCHITECTURE.md:114 already said what replaces the probe - "CallMask replaces 'is this table
// slot null' as the implicit capability probe" - and this header is the concrete list.
//
// TWO SPELLINGS, AND WHICH ONE A SITE TAKES IS DECIDED BY THE SLOT'S CLASS, NOT BY TASTE:
//
//   MGL_BACKEND_SLOT_CAP(Slot, CapBit)
//       The capability has a published bit. Under split the answer is the SERVER's bit;
//       under monolith it is exactly today's null check, character for character.
//       The three cases CONTRACT-P5.md §7 names by hand are all of this shape:
//         GL_Query.cpp:481 / :558 / :785  BeginOcclusionQuery   -> kCapOcclusionQuery
//         GL_Query.cpp:534                BeginXfbPrimitivesQuery -> kCapXfbPrimitivesQuery
//         PipeFill.cpp                    SubDataResident        -> kCapResidentSubData
//       (the third is not a GLFunctionsTable slot but an op-table one, so it reads the bit
//        directly in PipeFill.cpp rather than through this header).
//
//   MGL_BACKEND_SLOT_LOCAL(Slot)
//       The capability has NO published bit and the slot is CONTRACT-P5.md §7 class C -
//       Fatal{UnmigratedVerb} in the client's table. Under split the honest answer is
//       "absent", which is precisely what the monolith nullptr meant, so every fallback the
//       site already has survives instead of being replaced by an abort. Under monolith it is
//       again today's null check.
//
// WHY "ABSENT" AND NOT "LET IT FATAL". A Fatal is loud, and for the 28 UNGUARDED slots it is
// strictly better than today (a null there is already an immediate crash with no diagnostic).
// But a guarded site is guarded because the frontend has a real answer for the absent case -
// always-signaled syncs, a CPU readback, CPU primitive accounting - and turning that answer
// into an abort is a behaviour change nobody asked for, in the direction that stops a lane
// dead. The class-C slot IS absent from this client; saying so is the accurate answer, not a
// weakening of R-4.
//
// THE TEST THAT DECIDES WHETHER A GUARDED SITE IS CONVERTED AT ALL, and it is the half of the
// walk the contract leaves to whoever does it: A PROBE MOVES ONLY WHERE "ABSENT" IS A CORRECT
// AND SUFFICIENT ANSWER. Where the fallback behind the probe produces a RIGHT result, "absent"
// is the accurate description of a client that does not have the slot, and converting keeps
// the lane running. Where the fallback produces a SILENTLY WRONG result, converting would
// manufacture exactly the defect R-4 exists to prevent, and the probe is left alone so the
// class-C slot Fatals by name instead.
//
// Converted, because the fallback is right:
//   GL_Sync.cpp:59    FenceSync              -> always-signaled syncs, which the table's own
//                                               header documents as the fallback and which GL
//                                               permits; every other sync site is already
//                                               guarded on syncObject->backendHandle, so this
//                                               one gate carries the whole family.
//   GL_Texture.cpp:6537 GetTextureImage      -> the frontend's own CPU readback, which is exact
//   GL_Texture.cpp:6799 GetTexImage          -> the same
//   GL_Getter.cpp x2  GetGpuTimestampNs      -> 0, which BackendObject.h:192 already names as
//                                               the unsupported answer
//   GL_Query.cpp      the four query probes   -> CPU primitive accounting / target rejection /
//                                               COUNTER_BITS 0, all of them spec answers
//
// NOT converted, deliberately, because "absent" would be wrong rather than quiet:
//   GL_Drawing.cpp:1274/:1371/:1420/:1435/:1641/:1673  the transform-feedback span family. The
//       frontend reads a null EndTransformFeedback as "this backend does NOT own the capture,
//       so reorder the captured records for it" (FixupGsStripCaptureOrder, :1290). Under split
//       the server's backend DOES own it, so answering "absent" would reorder a capture that
//       was already in GL order - a silently corrupt buffer. Transform feedback is off P5's
//       reduced path (BRIEF §4's exclusion list) and its slots are class C, so the first XFB
//       call aborts by name, which is the outcome R-4 asks for.
//   GL_Drawing.cpp:844  PatchParameteri. "Absent" means the patch size is never set and every
//       tessellation draw silently uses the previous one. Class C; it aborts by name.
//
// WHAT THIS HEADER DELIBERATELY DOES NOT DO. It does not touch the 28 unguarded slots: those
// have no probe to convert, and calling one reaches Fatal{UnmigratedVerb, "<slot>"} by name,
// which is R-4's intent. And it does not invent a cap bit - a new MGPCapBit is an
// MGPipeTypes.h edit and that file is c0's, so a family that needs one goes through the
// integrator (the XFB span family is the first that will).
//
// G1: in a build without MOBILEGL_BUILD_DISAGGREGATED both macros expand to the null check the
// site already had, so the pull build's code generation is unchanged.

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
