// MobileGL - MobileGL/MG_Pipe/PipeInputsSwitch.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#ifndef MOBILEGL_MG_PIPE_INPUTS_SWITCH_H   // belt and braces: reachable as <MG_Pipe/..> and <..> (CMakeLists.txt:531,535)
#define MOBILEGL_MG_PIPE_INPUTS_SWITCH_H
// The strangler switch (ARCHITECTURE.md 9.2). Every backend read of frontend state is spelled
// MGB_CTX->Accessor(...). Pull arm: the live GLContext, so the pull build is the tree before P1
// token for token. Push arm: the PipeInputs block the frontend fills at every verb boundary.
// The pull arm is the ONLY place under MobileGL/ outside MG_State and MG_Impl that may spell
// pGLContext; purity gate C greps MG_Backend/ for that token.
#if MOBILEGL_PIPE_PUSH
#include <MG_Backend/MGPipe/PipeInputs.h>
#define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#define MGB_CTX_LIVE (::MobileGL::MG_Pipe::gPipeInputs.IsLive())
#define MGB_CTX_IDENTITY (::MobileGL::MG_Pipe::gPipeInputs.ContextIdentity())
#else
#include <MG_State/GLState/Core.h>
#define MGB_CTX (::MobileGL::MG_State::pGLContext)
#define MGB_CTX_LIVE (::MobileGL::MG_State::pGLContext != nullptr)
#define MGB_CTX_IDENTITY (static_cast<const void*>(::MobileGL::MG_State::pGLContext.get()))
#endif
#endif
