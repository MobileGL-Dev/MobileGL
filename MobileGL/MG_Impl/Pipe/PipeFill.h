// MobileGL - MobileGL/MG_Impl/Pipe/PipeFill.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
// The fill point (ARCHITECTURE.md 9.2, P1 brief D7). MG_Impl spells MGP_FILL(Verb); as the
// statement immediately before every call through gBackendFunctionsTable.GL - after every
// early return the call is behind, inside the loop body for a call made in a loop - so the
// frontend fills the PipeInputs block for exactly the verbs that reach a backend. In the
// pull build the macro is ((void)0) and the pull build is byte-identical to a tree without
// it.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
namespace MobileGL::MG_Pipe {
    // PipeFill.cpp. Bumps the per-verb serial, records the verb, and (from c2 on) copies
    // every field in the verb class's may-read mask out of the live GLContext, stamping each
    // with the new serial.
    void MGPipeFillForVerb(MGPipeVerb verb);
} // namespace MobileGL::MG_Pipe
#define MGP_FILL(Verb) ::MobileGL::MG_Pipe::MGPipeFillForVerb(::MobileGL::MG_Pipe::MGPipeVerb::Verb)
#else
#define MGP_FILL(Verb) ((void)0)
#endif
