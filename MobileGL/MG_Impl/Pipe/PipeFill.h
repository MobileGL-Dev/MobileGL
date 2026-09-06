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

    // PipeFill.cpp. THE VALIDATE POINT (ARCHITECTURE.md 5.1, P2 brief D1). In order:
    //   1. bump the per-verb serial, record the verb and the context identity;
    //   2. run the tracker's DIRTY WALK for this verb's class (MG_Impl/Pipe/Tracker.h);
    //   3. EMIT, for each set dirty bit whose subsystem bit is on in the runtime
    //      MOBILEGL_PIPE_PUSH bitmask, the P2 call that carries it;
    //   4. run the P1 residual fill for every field an emitted call did NOT supply,
    //      stamping each with the new serial exactly as before;
    //   5. in a verify build, the entry compare against a second snapshot (P1 brief D8) -
    //      which stops being a tautology the moment step 3 supplies a field step 4 skips.
    //
    // It was MGPipeFillForVerb through P1, when steps 2 and 3 did not exist. The macro
    // spelling, the 83 call sites and the verb enum are unchanged: the dispatch is
    // kMGPipeVerbClass's nine classes, which is the same code as nine named ValidateFor*
    // entry points with one call site per verb instead of nine.
    void MGPipeValidateForVerb(MGPipeVerb verb);

    // Ends the verb in flight without starting another: bumps the serial, so every field the
    // verb stamped goes stale, and puts the current verb back to "none", so a read made after
    // it aborts as Fatal{UnmigratedPipeInput, "<Field>@<none>"} - which is what such a read
    // is - instead of naming whichever verb happened to be filled last. Nothing in the GL
    // entry points calls this: a real verb is always followed by the next verb's fill. It
    // exists for a caller that drives a backend helper directly and wants its declaration to
    // stop where it says it stops (MG_Test/ScopedPipeVerb.h).
    void MGPipeLeaveVerb();

    // PipeFill.cpp. Negative control B (P1 brief D6): the filler withholds the STAMP - never
    // the value - of `field` at `verb`, so that verb's read of it is
    // Fatal{UnmigratedPipeInput, "Field@Verb"} while every other verb is unaffected. The
    // MOBILEGL_PIPE_POISON_OMIT knob ("<Verb>:<FieldName>") calls this once, on the first
    // fill; tests call it directly. Both null clears the omission. An unknown name is
    // Fatal{PipeVerifyBadKnob}.
    void MGPipeSetPoisonOmission(const char* verb, const char* field);

    // PipeFill.cpp. How many times set_vertex_attrib_defaults' applier failed to reproduce
    // the value the call carried, so the client wrote the mirror itself
    // (EmitVertexAttribDefaults). It is the ONE observable of that repair: the window it
    // covers is a verb whose class does not read m_currentVertexAttribute, where reading the
    // storage to check it would be the poison violation the fill table exists to forbid. So
    // TrackerShippedEmitter asserts on this counter instead, and the day package A's applier
    // switches on MGPAttribValue::ValueClass the counter stops moving.
    //
    // Not hot-path instrumentation: it is incremented only inside the repair branch, which
    // runs only when the call actually went out, which is only when an attribute default
    // moved.
    Uint64 MGPipeVertexAttribDefaultRepairCount();

    // PipeFill.cpp. The header of the last set_vertex_attrib_defaults that actually went out
    // - Mask, and Count == 0 for "none ever did", since a call naming no attribute is not
    // emitted. Two properties of this call have no other observable, because reading
    // m_currentVertexAttribute back at a verb whose class does not carry it is the poison
    // violation the fill table exists to forbid: that a FRESH CONTEXT republishes all 32
    // (the server's mirror still holds the previous context's defaults), and that one moved
    // attribute publishes exactly one. Eight bytes, written only when a call goes out.
    MGPVertexAttribDefaults MGPipeVertexAttribDefaultsLastHeader();

#if MOBILEGL_PIPE_VERIFY
    // PipeFill.cpp. The second arm of the comparator (P1 brief D8, ARCHITECTURE.md 13.2-2):
    // fills `snapshot` from the live GLContext the old way, for every field in `mask`. This
    // is the branch that survives P13, which is why it is its own function rather than the
    // filler's loop.
    void SnapshotFromGLContext(PipeInputs& snapshot, const MGPipeFieldMask& mask);
#endif
} // namespace MobileGL::MG_Pipe
#define MGP_FILL(Verb) ::MobileGL::MG_Pipe::MGPipeValidateForVerb(::MobileGL::MG_Pipe::MGPipeVerb::Verb)
#else
#define MGP_FILL(Verb) ((void)0)
#endif
