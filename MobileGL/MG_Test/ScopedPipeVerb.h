// MobileGL - MobileGL/MG_Test/ScopedPipeVerb.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/PipeFill.h>
#endif

namespace MobileGL::MG_Test {
    // "This test is standing inside verb X."
    //
    // A unit test that constructs a GLContext by hand and then calls a BACKEND helper
    // directly enters through no GL entry point, so no MGP_FILL ever fires (P1 brief D7) and
    // in a push build the PipeInputs block the helper's MGB_CTX reads is empty and unstamped:
    // its first accessor read is Fatal{UnmigratedPipeInput, "Field@<none>"}. The test is
    // right and the poison is right - what was missing is the verb, and this object is how a
    // test states it. Constructing it runs the real filler for `verb`, exactly the call
    // MG_Impl makes before that verb reaches a backend; destroying it leaves the verb again.
    //
    // It is not an escape hatch and it weakens nothing:
    //   - it fills exactly kMGPipeClassFieldMask[class of verb] out of the live GLContext, so
    //     a read of a field that verb does not fill is still Fatal, naming the field and this
    //     verb - the fill table stays the only thing that says what a verb may read;
    //   - leaving the scope re-arms the poison. The exit fill is a kQuery verb, whose class
    //     mask is a single field, so every field this scope stamped goes stale the moment the
    //     scope ends. That matters when the suite runs as one process (a developer running
    //     the test binary directly, rather than one ctest entry per case): without it, one
    //     case's declaration would cover a later case that forgot to make one;
    //   - it is a no-op in the pull build, where MGB_CTX is the live context and there is
    //     nothing to fill, so the pull build stays byte-identical.
    //
    // Place it the way MGP_FILL is placed in production (P1 brief D7): immediately BEFORE the
    // backend call, after every frontend mutation that call is meant to see. A test that
    // drives the helper again with frontend state changed in between issues a second verb -
    // Renew() - because that is what a second GL entry point would have done. A test that
    // drives a helper of a DIFFERENT verb class opens a nested scope for it.
    class ScopedPipeVerb {
    public:
        explicit ScopedPipeVerb([[maybe_unused]] MG_Pipe::MGPipeVerb verb)
#if MOBILEGL_PIPE_PUSH
            : m_verb(verb) {
            MG_Pipe::MGPipeFillForVerb(m_verb);
        }
#else
        {
        }
#endif

        ScopedPipeVerb(const ScopedPipeVerb&) = delete;
        ScopedPipeVerb& operator=(const ScopedPipeVerb&) = delete;

        // A second verb of the same kind begins: re-fill and re-stamp, so a helper driven
        // again after the test moved frontend state sees the new values, the way the next GL
        // entry point's MGP_FILL would.
        void Renew() {
#if MOBILEGL_PIPE_PUSH
            MG_Pipe::MGPipeFillForVerb(m_verb);
#endif
        }

#if MOBILEGL_PIPE_PUSH
        ~ScopedPipeVerb() { MG_Pipe::MGPipeFillForVerb(kLeaveVerb); }

    private:
        // Leaving is a fill of the narrowest verb class there is: kQuery names one field, so
        // the serial bump lands and every field this scope stamped falls behind it. The one
        // field is a transform-feedback counter read - no side effect, and nothing to undo.
        static constexpr MG_Pipe::MGPipeVerb kLeaveVerb = MG_Pipe::MGPipeVerb::GetGpuTimestampNs;
        MG_Pipe::MGPipeVerb m_verb;
#endif
    };
} // namespace MobileGL::MG_Test
