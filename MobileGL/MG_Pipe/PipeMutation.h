// MobileGL - MobileGL/MG_Pipe/PipeMutation.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#ifndef MOBILEGL_MG_PIPE_MUTATION_H // belt and braces: reachable as <MG_Pipe/..> and <..>
#define MOBILEGL_MG_PIPE_MUTATION_H
// Push-on-mutation (P1 lane finding F2). MGP_FILL copies a verb's may-read set out of the
// live GLContext at the verb boundary; the backend then reads that copy for the whole verb.
// A backend that WRITES a frontend object inside its own verb - Magma synthesising a
// fallback texture for an unbound sampler, materialising a queued clear, or overriding a
// sampler's filter - moves a value the boundary already copied, and every read after that
// point sees a block that no longer equals the live context. That is a real divergence, not
// a harness artefact: the pull build reads the moved value and the push build does not.
//
// The frontend mutator that moves such a value spells MGP_NOTE_MUTATION(Field) right where
// it moves it. The notice refreshes that ONE field in the pushed block when the field
// belongs to the verb currently in flight, so "the pushed block equals the live context at
// every read" stays literally true and the push build keeps pull semantics. It refreshes
// the value only and never the poison stamp, so a withheld stamp (MOBILEGL_PIPE_POISON_OMIT,
// negative control B) stays withheld.
//
// In the pull build the macro is ((void)0) and this header includes nothing, so the pull
// build is byte-identical to a tree without it.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
namespace MobileGL::MG_Pipe {
    // MG_Impl/Pipe/PipeFill.cpp (the client side, the only place that may spell pGLContext).
    // A no-op unless a context is live, a verb has been filled, and `field` is in that verb
    // class's may-read mask; a forwarded (sticky) field has no storage and is never copied.
    void MGPipeNoteFrontendMutation(MGPipeInputField field);
} // namespace MobileGL::MG_Pipe
#define MGP_NOTE_MUTATION(Field)                                                                                       \
    ::MobileGL::MG_Pipe::MGPipeNoteFrontendMutation(::MobileGL::MG_Pipe::MGPipeInputField::Field)
#else
#define MGP_NOTE_MUTATION(Field) ((void)0)
#endif
#endif
