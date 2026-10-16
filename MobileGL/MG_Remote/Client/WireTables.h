// MobileGL - MobileGL/MG_Remote/Client/WireTables.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client arm of MGPipe routing, the encode twin of gMGPipeWireRecordApply: thin emitters over
// ClientSession::EmitAndWait, installed into the generated tables and the escape table that
// MG_Pipe/PipeRoute.h declares, so every resource, CSO, texture and program record leaves the GL
// thread as a wire record instead of running against a context the apply thread owns.
// InstallClientWireTables() runs last in ClientSession::Start, on the GL thread: after the
// handshake (the peer has mapped the rings), after the first caps snapshot (consumer gates read
// it) and after the apply thread starts (EmitAndWait blocks on appliedSeq). Uninstall runs first
// in Stop, so a straggling routed call is refused by name instead of reaching freed rings.

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipeHandles.h>

#if MOBILEGL_BUILD_DISAGGREGATED

namespace MobileGL::MG_Remote::Client {

    // Installs the thirty-eight wire emitters over gMGPipeScreen / gMGPipeContext /
    // gMGPipeRouteEscapes and records the arm. Idempotent.
    void InstallClientWireTables();

    // Marks the routed tables uninstalled so a routed call refuses by name
    // (Fatal{ClientTablesUninstalled, "<row>"}) rather than running the applier on the caller
    // (codex 4). It does NOT restore the monolith adapters - that is ReinstallMonolithAfterTeardown
    // below, run only once the session's rings are freed. Idempotent.
    void UninstallClientWireTables();

    // Shared by routed rows and the five class-B emitters, before session/ring access.
    void RequireClientTablesInstalled(const char* row);

    // The LAST step of ClientSession::Stop: after the rings, segments and transport are gone,
    // puts the monolith adapters back and clears the refusal flag, so the at-exit ~BufferObject
    // deletes that reach a process with no session run the applier as they do under monolith.
    void ReinstallMonolithAfterTeardown();

    // How many records the thirty-eight emitters have published. It counts the ROUTED rows
    // only - a resource_create, a set_vertex_buffers, a create_shader_state - and never the
    // five class-B verbs, so it is the one number that says "the resource/CSO/state path really
    // ran" as opposed to "a Clear crossed".
    //
    // WHAT IT IS NOT (M7, corrected). This is NOT what arms the split lane. WireTables.h round 2
    // claimed it was "t1's FOURTH arming fact", and that was wrong: the harness reads the
    // encoder's EmitSeq (Harness/SplitRuntimePeek.cpp:50, Harness/ScenarioFixture.h:85), which
    // moves for the class-B verbs too. So an armed Clear-only lane is green on EmitSeq while this
    // counter stays 0. Making the harness read THIS instead is t1's file (SplitRuntimePeek), so
    // the honest statement is the one here: c1 counts the routed ordinal at the only place that
    // can, PipeFill's InitialBytesNotCarried self-check reads it (MGPipeEmitResourceRespecify), and the
    // lane's own arming remains EmitSeq until t1 re-points it. c1-v3.md M7 has the full note.
    Uint64 ClientWireRecordsEmitted();

    // How many of those were refused by the server, by acceptance row. Counted rather than
    // inferred, R-8's rule one level out.
    Uint64 ClientWireRecordsDeclined();

    // ---- P12: the CREATE WINDOW (MOBILEGL_IPC_CREATE_WINDOW) ---------------------------------
    //
    // The resource_create row defers its answer instead of dropping it: up to N creates go out
    // fire-and-forget, the caller reads a provisional accept, and the answers are taken in order.
    // The four numbers below are what a test can say about that without reaching into the row:
    // how many answers are outstanding, the ceiling the code actually applies (the config value
    // clamped to the array), the array's own size, and the two tallies - objects whose create came
    // back DECLINED and are therefore out of the window, and how many refusals there have been.
    Uint32 ClientCreateWindowPending();
    // How many deferred answers the window has read back. THE NUMBER THAT SAYS IT WORKED: a
    // drain that never collects leaves Pending at the ceiling and every other reading unchanged,
    // which is exactly what the device measured before the stream link learned to answer by seq.
    Uint64 ClientCreateWindowTaken();

    Uint32 ClientCreateWindowEffective();
    Uint32 ClientCreateWindowArraySize();
    Uint32 ClientCreateWindowSuspects();
    Uint64 ClientCreateWindowRefusals();

    // TRUE ON THE SERVER ROLE's OWN THREAD (the apply thread), false everywhere else. It is
    // v1's ServerLoop::OnApplyThread(), exposed here because it is table 3's role split made
    // into one predicate and TWO packages read it: the wire emitters below (a routed call that
    // finds itself on the apply thread runs the monolith adapter, because on that thread this
    // process IS the server), and MG_Impl/Pipe/PipeFill.cpp's split-only respecify/flush
    // branches (M5: those branches emit CLIENT wire records and must not run on the server's
    // apply thread, which produces none - the InitialBytesNotCarried self-check would abort the
    // server otherwise). Declared here so PipeFill does not have to include a server header.
    Bool RunsAsTheServerRole();

    // ---- P5c (ct), CONTRACT-P5C.md §5: the two control records' client halves --------------
    //
    // NOT ROUTED ROWS - the two are appended to PipeCalls.def with no MGPipeApply* entry point
    // and no table slot at all, so their producers call these directly. PipeFill.cpp's
    // FreshlyPrimed arm calls the first; DirectGLES' OnFrontendStateObjectDestroyed calls the
    // second.

    // applier_reset (§5.1). Emitted on the GL thread at the tracker.FreshlyPrimed() edge,
    // BEFORE the block's client-side resets, so the record's barrier orders the server's
    // MGPipeApplierReset() against every verb that follows. ContextSerial is the client's own
    // count of applier_reset emissions (0 = the first make-current): nothing in the frontend
    // numbers a make-current today, and the sink asserts the value against its own count of
    // the same records rather than dispatching on it (P5c: one context per session; P6 reads
    // it for real). Returns false - nothing emitted - when no live session could carry the
    // record (the bring-up window before Start, a server-role-only fixture, teardown); the
    // caller then makes the direct call the record replaced.
    Bool EmitApplierResetRecord();

    // A fresh session replaces an ended one: the create window's deferred answers, its suspect
    // list and the applier-reset serial all belonged to the ended session (ClientSession's
    // ForgetEndedSession).
    void ForgetWireTablesOfEndedSession();

    // ---- the EGL context lifecycle's control-plane half -------------------------------------
    //
    // eglCreateContext / eglDestroyContext are not ring records: they cross as SurfaceOp frames
    // on the blocking control channel the surface forwarders use, sent by EGLImpl on the app
    // thread in EGL call order, which is the order the server's context table needs. NoWire
    // (before ClientSession::Start, or on the server role's own thread) means keep the context on
    // the client's books alone. Refused is a server answer: eglCreateContext then returns
    // EGL_NO_CONTEXT, because the server would refuse that context's first bind_context.
    enum class ContextFrameEmit : Uint8 { Sent, NoWire, Refused };
    ContextFrameEmit SendCreateContextFrame(Uint64 clientContextToken, Uint64 shareGroupToken, Uint32 flags);
    // A destroy has the same three answers; the client's EGL book is dropped either way (that is
    // the caller's own state machine), and what a refusal changes is only what the server holds.
    ContextFrameEmit SendDestroyContextFrame(Uint64 clientContextToken);

    // P14 S1 (docs/Disaggregated/design/11-state-ownership.md): bind_context, opcode 84.
    //
    // Emitted on the GL thread from EGLImpl::MakeCurrent, after the switch has succeeded and
    // before eglMakeCurrent returns - which puts it ahead of the applier_reset the NEXT GL call's
    // validate emits (PipeFill.cpp's FreshlyPrimed arm), so the reset is already attributed to
    // the context that primed. `clientContextToken` is the token
    // EGLState minted for the context the calling thread now has current (0 = none, the release
    // edge) - the caller reads it rather than this table owning a second copy, so the two planes
    // cannot disagree about which context is bound. Returns false - nothing emitted - in exactly
    // the windows EmitApplierResetRecord documents (no live session, the pre-Start bring-up, a
    // server-role-only fixture, teardown), where there is no wire for the binding to cross and
    // the session's single-context default is what the server already answers.
    Bool EmitBindContextRecord(Uint64 clientContextToken);

    // P5c (rv), CONTRACT-P5C.md §5.3: whether set_context_values can cross right now (a live,
    // started session, tables not being torn down). PipeFill.cpp gates the record's emission
    // AND the residual-fill skip for its eight fields on this one answer, so the two halves
    // can never disagree about who supplies them.
    Bool ContextValuesWireLive();

    // The three answers a death notice can produce (§5.2): the record crossed; the client's
    // own allocator cannot resolve the dying object (the server never saw it, so there is no
    // twin to kill and NOTHING crossed); or no live session could carry the record. The
    // third is NOT folded into the second: a server role with no wire at all - a ServerLoop
    // fixture's shape, never a real split's - still has a server loop the caller delivers to,
    // where a handle that never existed has nothing to deliver.
    enum class ObjectDeathEmit : Uint8 { Emitted, NoHandle, NoSession };
    ObjectDeathEmit EmitObjectDeathRecord(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId);

} // namespace MobileGL::MG_Remote::Client

#endif // MOBILEGL_BUILD_DISAGGREGATED
