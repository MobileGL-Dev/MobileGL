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
    struct PipeInputs;

    // PipeFill.cpp. Bumps the per-verb serial, records the verb and the context identity,
    // and copies every field in the verb class's may-read mask (kMGPipeClassFieldMask) out
    // of the live GLContext, stamping each with the new serial. In a verify build it then
    // runs the entry compare against a second snapshot (P1 brief D8).
    void MGPipeFillForVerb(MGPipeVerb verb);

    // PipeFill.cpp. Negative control B (P1 brief D6): the filler withholds the STAMP - never
    // the value - of `field` at `verb`, so that verb's read of it is
    // Fatal{UnmigratedPipeInput, "Field@Verb"} while every other verb is unaffected. The
    // MOBILEGL_PIPE_POISON_OMIT knob ("<Verb>:<FieldName>") calls this once, on the first
    // fill; tests call it directly. Both null clears the omission. An unknown name is
    // Fatal{PipeVerifyBadKnob}.
    void MGPipeSetPoisonOmission(const char* verb, const char* field);

#if MOBILEGL_PIPE_VERIFY
    // PipeFill.cpp. The second arm of the comparator (P1 brief D8, ARCHITECTURE.md 13.2-2):
    // fills `snapshot` from the live GLContext the old way, for every field in `mask`. This
    // is the branch that survives P13, which is why it is its own function rather than the
    // filler's loop.
    void SnapshotFromGLContext(PipeInputs& snapshot, const MGPipeFieldMask& mask);
#endif
} // namespace MobileGL::MG_Pipe
#define MGP_FILL(Verb) ::MobileGL::MG_Pipe::MGPipeFillForVerb(::MobileGL::MG_Pipe::MGPipeVerb::Verb)
#else
#define MGP_FILL(Verb) ((void)0)
#endif
