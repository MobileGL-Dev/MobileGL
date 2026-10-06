// MobileGL - MobileGL/MG_Pipe/PipeSessionFail.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P7 wave 0 (plan section 1.3's P7-marked refusal row, item (0); Ph slice (2)). HOW A BACKEND DEATH REACHES
// Session::Fail WITHOUT THE BACKEND KNOWING MG_Remote EXISTS.
//
// The three Magma wire funnels - MagmaWireFatal (Renderer/WireFramebuffer.inc),
// WireDescriptorFatal (Renderer/UniformManager.cpp) and WireBufferLegacyFatal
// (Renderer/WireDraw.inc) - each logged their line and raised their OWN std::abort(). Fifteen
// P7-marked refusals (the grep key is the at-sign form, not spelled here) and two more sites die through them, and every one of those deaths:
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
// census and in every recorded P7-refusal measurement, so they are byte-identical before and after.

#pragma once

#include <MG_Pipe/PipeFatalFamily.h>

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
#if MOBILEGL_BUILD_DISAGGREGATED
        // A malformed peer record rejected by the server-side applier.
        ProtocolCorruption,
        // The GPU device a session's backend renders with is lost (a GPU fault or hang the driver
        // reset). Raised only through MGPipeSessionLatch below: it ends THAT session.
        DeviceLost,
#endif
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

    // ---- P13 W5: THE RECORD ARM'S DEATHS, IN EVERY BUILD -------------------------------------
    //
    // The record arm's own code - the staged stores, the verb sink, the verb port - dies by
    // family word (MG_Pipe::MGFatalFamily, the whole FatalFamilies.def vocabulary) and, where a
    // session can be ended instead of the process, latches. With MG_Remote linked, FatalFunnel.cpp
    // registers SessionFail / SessionLatch / SessionLatched here at static init, so every DISAGG
    // process - unit cases with no role init included - counts, publishes and latches exactly as
    // a direct call did. Without MG_Remote (the FCL shape) the defaults are the monolith answer:
    // log the line, echo it to stderr (a death test reads the child's stderr), abort; latching is
    // dying; nothing is ever latched.
    //
    // `line` is the FULLY FORMATTED message, family word and all, as for MGPipeSessionFail.
    using MGPipeRecordFailHook = void (*)(MGFatalFamily family, const char* line);
    using MGPipeRecordLatchHook = bool (*)(MGFatalFamily family, const char* line);
    using MGPipeRecordLatchedHook = bool (*)();
    // Last writer wins; a null leaves that half on its default.
    void MGPipeInstallRecordFailHooks(MGPipeRecordFailHook fail, MGPipeRecordLatchHook latch,
                                      MGPipeRecordLatchedHook latched);
    // The installed hooks, or nullptr - so a test proves the registration happened (and restores
    // what it replaced) without having to die to find out.
    MGPipeRecordFailHook MGPipeRecordFailHookInstalled();
    MGPipeRecordLatchHook MGPipeRecordLatchHookInstalled();
    MGPipeRecordLatchedHook MGPipeRecordLatchedHookInstalled();
    [[noreturn]]
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void MGPipeRecordFail(MGFatalFamily family, const char* fmt, ...);
    // Returns false (a Bool site reads `return MGPipeRecordLatch(...);`) when the hook latched the
    // calling session; with no latch to raise it dies as MGPipeRecordFail does.
    bool
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
        MGPipeRecordLatch(MGFatalFamily family, const char* fmt, ...);
    // Whether the calling session already latched (false with no hook).
    bool MGPipeRecordLatched();

#if MOBILEGL_BUILD_DISAGGREGATED
    // ---- THE LATCH TWIN: A LOST GPU DEVICE ENDS ITS SESSION, NOT THE PROCESS ----------------
    //
    // MGPipeSessionFail is for a verb the backend cannot honour, and it does not return. A device
    // loss is different in kind: nothing about the peer's bytes was wrong, the GPU faulted or hung
    // and the driver reset it, and every session the process serves except the one whose device
    // went is still healthy. The display server runs every session in ONE process, so dying here
    // takes the compositor and every other client down with the one that lost its device.
    //
    // So a backend that sees its device lost calls this instead, and then RETURNS from the verb
    // it was applying without touching the GPU again. The hook MG_Remote installs latches the
    // calling thread's session (SessionLatch): the session declines everything after it, closes,
    // and its client reads a lost context. Where no latch is armed - the inproc client+server
    // shape, where the process IS the session - the hook dies exactly as MGPipeSessionFail does,
    // and with no hook at all this is MGPipeSessionFail. Returns false, so a Bool site can read
    // `return MGPipeSessionLatch(...);`.
    using MGPipeSessionLatchHook = bool (*)(MGPipeFatalFamily family, const char* line);
    // Whether the calling thread's session HAS a latch to raise. A backend that notices its
    // device lost outside a site that would die anyway (a submit whose failure it used to only
    // log) latches at once when this is true - so the session stops at its next record however
    // the current verb unwinds - and keeps its old behaviour when it is false.
    using MGPipeSessionLatchArmedHook = bool (*)();
    void MGPipeInstallSessionLatchHook(MGPipeSessionLatchHook hook, MGPipeSessionLatchArmedHook armed);
    MGPipeSessionLatchHook MGPipeSessionLatchHookInstalled();
    bool MGPipeSessionLatchArmed();
    bool
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
        MGPipeSessionLatch(MGPipeFatalFamily family, const char* fmt, ...);

    // A DEBUG KNOB THAT MAKES ONE DEVICE CHECK REPORT A LOSS, so the containment above can be
    // driven without a GPU that faults on demand. MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT=N (or, on
    // Android, the property debug.mobilegl.inject_device_lost_at) makes the N-th device check the
    // backends make after the knob was first seen nonzero answer "lost", once per value: setting
    // another value (or 0 and then a value again) re-arms it, so one server process can lose
    // several sessions in turn. The backends ask at their frame boundaries and readbacks; unset
    // (the default) it is one getenv.
    bool MGPipeDebugDeviceLossDue();
    // THE SAME KNOB, AIMED AT ONE CLIENT. MOBILEGL_DEBUG_INJECT_DEVICE_LOST_PID=P (Android: the
    // property debug.mobilegl.inject_device_lost_pid) loses the session whose client named pid P in
    // its Hello (its own pid, as it sees it - a container's own pid namespace included), once per
    // value, whatever the backend and whether or not that client ever presents or reads back: the
    // server's apply thread asks after every drain. It is how one application among several is
    // made to lose its device - a browser's GPU process, which renders into dma-bufs and never
    // presents, included. Asked only for a session with a context bound; asks the environment /
    // property at most every 100 ms per thread.
    bool MGPipeDebugSessionLossDue(unsigned clientPid);
#else
    // P13 W5: the device-loss knobs end a SESSION; a library without a transport has none to end.
    inline bool MGPipeDebugDeviceLossDue() { return false; }
#endif

} // namespace MobileGL::MG_Pipe
