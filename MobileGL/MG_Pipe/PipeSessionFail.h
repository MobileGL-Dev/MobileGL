// MobileGL - MobileGL/MG_Pipe/PipeSessionFail.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P7 wave 0 (plan section 1.3 row `@P7`, item (0); Ph slice (2)). HOW A BACKEND DEATH REACHES
// Session::Fail WITHOUT THE BACKEND KNOWING MG_Remote EXISTS.
//
// The three Magma wire funnels - MagmaWireFatal (Renderer/WireFramebuffer.inc),
// WireDescriptorFatal (Renderer/UniformManager.cpp) and WireBufferLegacyFatal
// (Renderer/WireDraw.inc) - each logged their line and raised their OWN std::abort(). Fifteen
// `@P7` refusals and two more sites die through them, and every one of those deaths:
//
//   * published NO SessionFault frame, so the peer read a bare EOF and could not name what
//     ended the session (CONTRACT-P6 5.2's whole point);
//   * bumped NO SessionFaultCount(), so exit gate S8's "zero faults over a good run" could
//     neither confirm nor falsify them;
//   * carried a HAND-TYPED family word with no path to FatalFamilies.def's projection, so
//     nothing checked that `UnmigratedVerb` in the string still meant UnmigratedVerb.
//
// Calling SessionFail() from the .inc files directly would be the obvious fix and is the wrong
// one: MG_Backend's renderer would then name an MG_Remote symbol on its own account rather than
// through the two staging headers it already borrows under `#if MOBILEGL_BUILD_DISAGGREGATED`,
// and the layering that P13's module boundaries have to establish would be one more edge worse.
//
// So the shape is MGP_TRIP_WIRE_REPORT's (PipeApply.cpp:66): MG_Pipe owns the entry point, the
// DEFAULT behaviour is exactly what the site did before (same log line, same abort), and the
// richer behaviour is a function pointer that a layer above installs. MG_Remote installs
// InstallPipeSessionFailHook() at server-role init (MG_Backend/Init.cpp's InitServerRoleCommon,
// which both the inproc server role and the spawn child run), and from then on these deaths go
// through SessionFail like every other one.
//
// THE MESSAGE STRING IS PASSED VERBATIM, family word and all - the same rule FatalFunnel.h
// states. The lines these three funnels write are already counted by name in the retrace refusal
// census and in every recorded `@P7` measurement, so they are byte-identical before and after.

#pragma once

namespace MobileGL::MG_Pipe {

    // THE FAMILIES THAT CROSS THIS BOUNDARY, AND ONLY THOSE. Not `MGFatalFamily` cast to an
    // integer: the .def's enumerators are positional, and a row inserted in the middle of it
    // would silently re-point every hook call at a different family. Two words is what the three
    // funnels use today, and a third one costs a row here and an arm in the adapter - which is
    // the point, because that arm is where the choice gets argued.
    enum class MGPipeFatalFamily : unsigned {
        // The Magma wire arms' refusal to honour a verb/shape the split path does not implement.
        // Deliberately the SAME word the sites already print: `Magma:` in the detail is what
        // separates them from the GLES `UnmigratedVerb` deaths, and renaming the family would
        // invalidate every census number recorded against it since P5b.
        UnmigratedVerb,
        // A call that reached an arm this build did not compile, or ran on the wrong role.
        RoleViolation,
    };

    // What the layer above installs. `line` is the FULLY FORMATTED message, so the hook forwards
    // it rather than re-formatting it - a second vsnprintf is a second chance to disagree about
    // what the death said. A hook is expected not to return; MGPipeSessionFail aborts anyway if
    // one does, because a death that keeps running is the one outcome nothing downstream handles.
    using MGPipeSessionFailHook = void (*)(MGPipeFatalFamily family, const char* line);

    // Idempotent and last-writer-wins. Installed once per process at server-role init; a unit
    // case installs its own and restores nullptr afterwards.
    void MGPipeInstallSessionFailHook(MGPipeSessionFailHook hook);

    // The installed hook, or nullptr. A test reads it to prove installation happened without
    // having to die to find out.
    MGPipeSessionFailHook MGPipeSessionFailHookInstalled();

    // Formats `fmt`, hands the line to the hook if one is installed, and otherwise does exactly
    // what the three funnels used to do on their own: MGLOG_F the line and std::abort().
    [[noreturn]]
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void MGPipeSessionFail(MGPipeFatalFamily family, const char* fmt, ...);

} // namespace MobileGL::MG_Pipe
