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

    // P15: the validate point again for a verb whose first validate answered UNBARRIERED for a
    // per-call reason that did not hold at emission (a pack-buffer readback that falls back to
    // the reply form). Runs the barriered fill: quiesce, then fill, exactly as a barriered verb.
    void MGPipeValidateForVerbBarriered(MGPipeVerb verb);

    // Ends the verb in flight without starting another: bumps the serial, so every field the
    // verb stamped goes stale, and puts the current verb back to "none", so a read made after
    // it aborts as Fatal{UnmigratedPipeInput, "<Field>@<none>"} - which is what such a read
    // is - instead of naming whichever verb happened to be filled last. Nothing in the GL
    // entry points calls this: a real verb is always followed by the next verb's fill. It
    // exists for a caller that drives a backend helper directly and wants its declaration to
    // stop where it says it stops (MG_Test/ScopedPipeVerb.h).
    void MGPipeLeaveVerb();

    // PipeFill.cpp. Whether this build, backend and MOBILEGL_PIPE_PUSH mask emit for `subsystem`:
    // the FamilyIsLive() expression every birth hook and `wants()` row uses, exported only for
    // unit tests. It needs the subsystem bit, the family's kMGPipeWired*Subsystem constant
    // (`wired`; this header cannot include the emit headers), a backend consumer for the family
    // and its dependency bits from MG_Pipe/SubsystemDeps.def. Without the last two the texture
    // emitters clear dirty levels on acceptance that nothing then uploads.
    // MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp checks this and Espryt's
    // Resolve<Family>SubsystemArm() refusals against the .def.
    Bool MGPipeP4aFamilyEmits(Uint64 subsystem, Uint64 wired);

    // PipeFill.cpp. Whether an emitted call supplies `field` in this environment, i.e. whether
    // the residual fill (step 4) skips it. Memoized on the last three arguments plus what it reads
    // itself (the consumer signal, the caps-mirror generation); uncached it cost 63 caps-mirror
    // reads per verb. Exported only for unit tests, the sole check that the memo key is complete,
    // since emissions write the same storage the fill would. A wrong answer is silent: step 4
    // stamps FilledGen whether or not it copied, so a skipped field reads as fresh with the
    // previous verb's value. Poison only catches unstamped reads; the verify comparison is the
    // other net.
    Bool MGPipeResidualFillSuppliesField(MGPipeInputField field, Uint64 pushMask, Bool applierDerives,
                                         Bool contextValuesWireLive);

    // PipeFill.cpp. The draw's raw vertex-fetch base instance for the next set_vertex_buffers,
    // which carries it explicitly; the server decides whether to emulate the fetch shift or use
    // GL_EXT_base_instance. It also feeds that record's content hash and the tracker's bit-9
    // shutter, so a draw that changes only its base instance still goes out. GL entry points
    // call it through MGP_SET_BASE_INSTANCE, one line above MGP_FILL in GL_Drawing.cpp's three
    // *BaseInstance draws. The validate point clears it on both exits; the tracker's Reset()
    // must not, because Update() resets on a context switch, after the setter and before use.
    void MGPipeSetPendingBaseInstance(Uint32 baseInstance);
    // What the next set_vertex_buffers will carry. The unit gate reads it to pin that a
    // make-current between the setter and the fill does not eat it
    // (TrackerWalk.ABaseInstanceSurvivesTheFirstWalkOnAFreshContext).
    Uint32 MGPipePendingBaseInstance();

    // PipeFill.cpp. How many times set_vertex_attrib_defaults' applier failed to reproduce
    // the value the call carried, so the client wrote the mirror itself
    // (EmitVertexAttribDefaults). It is the ONE observable of that repair: the window it
    // covers is a verb whose class does not read m_currentVertexAttribute, where reading the
    // storage to check it would be the poison violation the fill table exists to forbid. So
    // TrackerShippedEmitter asserts on this counter instead. Since P5c rv the record carries
    // all three views verbatim (CONTRACT-P5C.md §5.3) and the applier writes each from its own
    // array, so the counter is expected to stay at 0 - the check that increments it is the
    // trip wire that remains.
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

    // PipeFill.cpp. Negative control B (P1 brief D6): from now on the filler withholds the STAMP -
    // never the value - of `field` at `verb`, and so does the server's verb stamp, so that verb's
    // read of it is Fatal{UnmigratedPipeInput, "Field@Verb"} while every other verb is unaffected.
    // Names from kMGPipeVerbNames / kMGPipeInputFieldNames; false, arming nothing, for an unknown
    // one. Call it on the GL thread. The integration harness arms it (Harness/PoisonOmissionArm.h).
    Bool MGPipeSetPoisonOmission(const char* verb, const char* field);
#endif
} // namespace MobileGL::MG_Pipe
#define MGP_FILL(Verb) ::MobileGL::MG_Pipe::MGPipeValidateForVerb(::MobileGL::MG_Pipe::MGPipeVerb::Verb)
// P3a D-H2.1. One line immediately ABOVE the MGP_FILL of a draw entry point that takes a
// baseinstance, carrying the argument RAW. It has to be a macro for MGP_FILL's reason: the
// three call sites are compiled in the pull build too, where MGPipeSetPendingBaseInstance is
// neither declared nor defined.
#define MGP_SET_BASE_INSTANCE(BaseInstance)                                                                            \
    ::MobileGL::MG_Pipe::MGPipeSetPendingBaseInstance(static_cast<::MobileGL::Uint32>(BaseInstance))
