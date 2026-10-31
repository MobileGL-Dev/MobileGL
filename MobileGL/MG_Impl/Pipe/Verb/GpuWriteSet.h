// MobileGL - MobileGL/MG_Impl/Pipe/Verb/GpuWriteSet.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Client-side conservative GPU-write set (ARCHITECTURE.md §13.1.2). glMapBuffer and
// glGetBufferSubData ask BufferObject::SyncGpuWrites, on the application thread, whether the GPU
// may have written a buffer; the backend's marking sites run on the server, so the client marks
// from its own bindings before each draw or dispatch record goes out. The set over-approximates:
// a missed mark reads a stale shadow silently, while an extra one costs a readback. Rows with a
// backend twin mirror it, including where the twin is deliberately narrow; the atomic-counter
// row is wider, in the safe direction. Backend-owned scratch buffers are not covered.

// P13 W5: moved from MG_Remote/Client/GpuWritePending.h - the record arm owns this set in a
// library without a transport too. The session's waits stay there.

#pragma once
#include <Includes.h>

#include <Config.h>

namespace MobileGL::MG_State::GLState {
    class BufferObject;
    struct ImageTextureBinding;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Record {

    // ONE ROW PER PRODUCER, and the enum is the inventory: six that mirror a backend
    // MarkGpuWritten site one-for-one, two that P5 adds. Each has exactly one unit case, and
    // the per-row counters below are what those cases assert on - a row that stops firing is
    // otherwise invisible, because an over-approximating set fails SILENTLY in the direction
    // that matters.
    enum class GpuWriteProducer : Uint8 {
        // DirectGLES.cpp's SyncBufferBindingPointsByRecord (shader-storage class) and
        // UniformManager.cpp's ResolveStorageBufferDescriptor: every SSBO binding point,
        // unconditional, once the points are bound and the draw or dispatch is going out.
        ShaderStorageBinding = 0,
        // DirectGLES.cpp's SyncAtomicCounterBuffers: every bound atomic counter. The
        // point of a counter is that the shader increments it and every conformance case
        // reads the increment back with glMapBufferRange or glGetBufferSubData.
        AtomicCounterBinding,
        // DirectGLES.cpp's MarkWritableImageBufferTexturesGpuWritten and
        // UniformManager.cpp's ResolveStorageTexelBufferDescriptor: a buffer texture on
        // an image unit, ONLY when Access != GL_READ_ONLY. Marking a read-only binding would
        // make the next map wait on - and then re-read - a dispatch that could not have
        // changed a byte of it.
        WritableImageBufferTexture,
        // WireDraw.cpp's BeginWireXfbCaptureForDraw, which VulkanRenderer::BeginXfbCaptureForDraw
        // calls: the capture targets, because the capture is a GPU write like any shader's.
        TransformFeedbackCapture,
        // P5, new: glReadPixels into a bound GL_PIXEL_PACK_BUFFER.
        ReadPixelsPackBuffer,
        // P5, new: glEndTransformFeedback, in place of the unbounded fence wait.
        EndTransformFeedbackCapture,
        Count
    };

    // The backend reads records (MG_Config::DataArmIsRecord). False means every entry point
    // below is a no-op and the backend sites are the only producers.
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

    // Row 2's discriminator, verbatim from the backend's former IsWritableImageBufferTexture (now
    // IsWritableImageBufferView, DirectGLESTextures.cpp). A function rather than three inline conditions, because
    // it is the one row whose backend twin is narrow on purpose, and a client that re-derived it slightly wider
    // would mark read-only image bindings with nothing able to see that it had.
    Bool ImageUnitIsAWritableBufferTexture(const MG_State::GLState::ImageTextureBinding& binding);

    // The one place a row actually marks. Null and duplicate marks are absorbed here so the
    // walks stay readable, and the per-row counter moves only when a buffer really was
    // marked.
    void MarkBufferForProducer(const SharedPtr<MG_State::GLState::BufferObject>& buffer, GpuWriteProducer producer);

    Uint64 ProducerMarkCount(GpuWriteProducer producer);
    void ResetProducerMarkCountsForTest();

    // Is there a readback route at all? A buffer with no size, or one whose backend registered
    // no resource ops, can never catch up, and SyncGpuWrites must clear rather than block for
    // ever. It is the ONE case monolith's unconditional clear covers that a writeback cannot.
    Bool BufferWritebackIsReachable(const MG_State::GLState::BufferObject& buffer);

} // namespace MobileGL::MG_Record
