// MobileGL - MobileGL/MG_Remote/Client/GpuWritePending.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE CLIENT-SIDE CONSERVATIVE GPU-WRITE SET (ARCHITECTURE.md:575 names this component;
// CONTRACT-P5.md section 3, table 2's first set). Owner: package b1.
//
// WHY IT HAS TO MOVE SIDES. `BufferObject::SyncGpuWrites` (BufferObject.cpp:369) runs
// synchronously, on the application's thread, the moment the application calls glMapBuffer or
// glGetBufferSubData. It has to answer "did the GPU write this buffer since I last read it?"
// - and today all six producers of that answer are BACKEND-side (DirectGLES.cpp:570, :618,
// :2603, UniformManager.cpp:1075, :1231, VulkanRenderer.cpp:11618), i.e. on the server's side
// of a split. Asking the server is a round trip the design forbids in steady state, so the
// client must build the set itself, from state it already owns: the SSBO and atomic-counter
// binding points, the image units whose Access is not GL_READ_ONLY, and the transform-feedback
// capture targets.
//
// CONSERVATIVE MEANS OVER-APPROXIMATE, AND THAT IS THE WHOLE DESIGN. The reverse channel's
// OnGpuWritten is a NARROWING channel (ResourceTracker.h:577-581): the client marks
// everything a shader COULD have written and the server only ever removes entries. So a row
// here that fires too often costs a readback; a row that fires too rarely reads a stale shadow
// and is silent. Every row below therefore mirrors its backend twin exactly, including the two
// places the twin is deliberately NARROW - a GL_READ_ONLY image binding is left alone, and the
// image walk runs from draw preparation rather than from glBindImageTexture's eager sync.
//
// NO NARROWING IN P5. ResourceTracker.h:587-592's `rangeCount == 1` assertion STAYS. Zero
// ranges will mean "a fully narrowed set - nothing is dirty" at P8/P9, and a package that
// reads ResourceTracker.h:577-581 alone will think it owns that already. It does not.
//
// THE TWO NEW PRODUCERS. Rows 4 and 5 have no backend twin: they are behaviour P5 ADDS
// (ARCHITECTURE.md:508), and both are strictly better than what monolith does.
//   * glReadPixels into a pack PBO becomes fire-and-forget plus a client-side mark. Monolith
//     maps the PBO and copies it into the shadow inside the call (DirectGLES.cpp:10983-10993),
//     which is an unconditional stall on every glReadPixels whether or not anyone reads the
//     shadow; marking instead defers the cost to the first read that actually wants it.
//   * glEndTransformFeedback drops its unbounded ClientWaitSync (GL_Drawing.cpp:1367, timeout
//     ~0ull) and marks the capture targets, for the same reason: the wait exists only so that
//     a later MapBuffer sees real results, which is precisely what the flag is for.
//
// WHAT NO ROW COVERS, WRITTEN DOWN SO THE NEXT READER DOES NOT HAVE TO ASK. The set is built
// from the APPLICATION's bindings, so it says nothing about a backend's own scratch buffers -
// Espryt's converted-vertex-stream and primitive-restart substitution buffers, Magma's UBO
// ring. None of those has a MarkGpuWritten today either, so the client set is no NARROWER than
// monolith's and this is not a regression; it is a standing hole in both, and it stays one
// until the phase that migrates the backend's own allocations.
//
// GATED ON THE TRANSPORT, like everything else in this package: on the monolith path the six
// backend sites still run and a second marker would be new behaviour (D-J), and rows 4 and 5
// would remove a stall monolith is entitled to keep.

#pragma once
#include <Includes.h>

#include <Config.h>

namespace MobileGL::MG_State::GLState {
    class BufferObject;
    struct ImageTextureBinding;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Remote::Client {

    // ONE ROW PER PRODUCER, and the enum is the inventory: six that mirror a backend
    // MarkGpuWritten site one-for-one, two that P5 adds. Each has exactly one unit case, and
    // the per-row counters below are what those cases assert on - a row that stops firing is
    // otherwise invisible, because an over-approximating set fails SILENTLY in the direction
    // that matters.
    enum class GpuWriteProducer : Uint8 {
        // DirectGLES.cpp:570 (MarkShaderStorageBuffersGpuWritten) and
        // UniformManager.cpp:1231 (ResolveStorageBufferDescriptor): every SSBO binding point,
        // unconditional, once the points are bound and the draw or dispatch is going out.
        ShaderStorageBinding = 0,
        // DirectGLES.cpp:618 (SyncAtomicCounterBuffers): every bound atomic counter. The
        // point of a counter is that the shader increments it and every conformance case
        // reads the increment back with glMapBufferRange or glGetBufferSubData.
        AtomicCounterBinding,
        // DirectGLES.cpp:2603 (MarkWritableImageBufferTexturesGpuWritten) and
        // UniformManager.cpp:1075 (ResolveStorageTexelBufferDescriptor): a buffer texture on
        // an image unit, ONLY when Access != GL_READ_ONLY. Marking a read-only binding would
        // make the next map wait on - and then re-read - a dispatch that could not have
        // changed a byte of it.
        WritableImageBufferTexture,
        // VulkanRenderer.cpp:11618 (BeginXfbCaptureForDraw): the capture targets, because
        // "the capture is a GPU write like any shader's".
        TransformFeedbackCapture,
        // P5, new: glReadPixels into a bound GL_PIXEL_PACK_BUFFER.
        ReadPixelsPackBuffer,
        // P5, new: glEndTransformFeedback, in place of the unbounded fence wait.
        EndTransformFeedbackCapture,
        Count
    };

    // Transport != Monolith. False means every entry point below is a no-op and the six
    // backend sites are still the only producers, which is exactly today's behaviour.
    Bool GpuWriteSetIsClientSide();

    // ---- the walks -------------------------------------------------------------------
    //
    // Called from the client's own draw / dispatch emission point, BEFORE the verb record
    // goes out, for the same ordering reason the persistent-map push has: the set must
    // describe the work the record is about to start.

    // Rows 0, 1, 2 and 3.
    void MarkGpuWritesForDraw();
    // Rows 0, 1 and 2. A dispatch has no transform feedback.
    void MarkGpuWritesForDispatch();

    // ---- the two new producers -------------------------------------------------------

    // Row 4. Marks whatever is bound to GL_PIXEL_PACK_BUFFER, or nothing when the read goes
    // to client memory - which is the case the backend's map-and-copy never had to consider,
    // because it only ran when a PBO was bound in the first place.
    void MarkReadPixelsPackBuffer();
    // Row 5. Must be called while the capture state is still ACTIVE: GLContext's
    // EndTransformFeedback clears the live bindings, so a mark taken after it marks nothing.
    void MarkEndTransformFeedbackCaptureTargets();

    // ---- the row predicates, exposed so a unit case can drive one row at a time -------

    // Row 2's discriminator, verbatim from DirectGLES.cpp:2354-2357. It is a function rather
    // than three inline conditions at the call site because it is the one row whose backend
    // twin is narrow on purpose, and a client that re-derived it slightly wider would mark
    // read-only image bindings with nothing able to see that it had.
    Bool ImageUnitIsAWritableBufferTexture(const MG_State::GLState::ImageTextureBinding& binding);

    // The one place a row actually marks. Null and duplicate marks are absorbed here so the
    // walks stay readable, and the per-row counter moves only when a buffer really was
    // marked.
    void MarkBufferForProducer(const SharedPtr<MG_State::GLState::BufferObject>& buffer,
                               GpuWriteProducer producer);

    Uint64 ProducerMarkCount(GpuWriteProducer producer);
    void ResetProducerMarkCountsForTest();

    // ---- SyncGpuWrites' third state (CONTRACT-P5.md section 3) -----------------------

    // Blocks until this buffer's OnBufferWriteback has landed. With no session - a build-split
    // lane running monolith, and every unit case - the emission was synchronous and the answer
    // is already in, so this returns at once; that is why it is a call rather than a loop the
    // caller writes, because the loop would be a hang in exactly that configuration.
    void AwaitBufferWriteback(MG_State::GLState::BufferObject& buffer);

    // Is there a readback route at all? A buffer with no size, or one whose backend registered
    // no resource ops, can never catch up, and SyncGpuWrites must clear rather than block for
    // ever. It is the ONE case monolith's unconditional clear covers that a writeback cannot.
    Bool BufferWritebackIsReachable(const MG_State::GLState::BufferObject& buffer);

    // The slice a whole-buffer readback is cut into so one writeback event always fits
    // SEG_EVENT. The writeback's bytes travel INLINE in the event record (P5c ev, CONTRACT-P5C
    // §4.2), one record must fit the ring (RingProducer::MaxRecordBytes == capacity/2,
    // Ring.h:288), and a 24 MiB arena's single shot cannot (measured: Fatal{EventRingOverflow}
    // on LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack). 0 when no session is
    // active - the synchronous arm never slices.
    SizeT BufferWritebackSliceBytes();

    // The same question for CONTENT: how many bytes one resource_subdata record may stage
    // before the range has to be cut. SEG_STAGE is a linear arena, one record's blob is
    // allocated from it whole, and a blob larger than the arena is
    // Fatal{RingOverrun, "SEG_STAGE"} at the encoder rather than a split
    // (PipeWireCodec.cpp:856-864) - measured on the CI traces, where a 128 MiB arena's
    // whole-buffer follow-up against a 32 MiB segment aborted. 0 means "do not cut", i.e. the
    // record's own bound (MGPipeForEachSubDataRecordRange's default), which is the answer for
    // monolith, for the server role's own uploads (they run the monolith adapter) and for a
    // process with no session - every unit gate.
    SizeT MGPipeStageChunkBytes();

    // The clamp itself, a pure function of the segment's size so that a unit gate can drive the
    // splitter at exactly the value a live session produces. A quarter of the segment, never
    // below 4096 (a zero-width piece is a refusal, ResourceTracker.h:254) and never above the
    // segment (a larger cap would stage the very blob the arena refuses).
    SizeT MGPipeStageChunkBytesFor(Uint64 stageCapacityBytes);

} // namespace MobileGL::MG_Remote::Client
