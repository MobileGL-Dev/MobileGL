// MobileGL - MobileGL/MG_Remote/Client/GpuWritePending.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GpuWritePending.h"

#include "ClientSession.h"

#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Client {

    using MG_State::GLState::BufferObject;
    using MobileGL::BufferTarget;
    using MG_State::GLState::ImageTextureBinding;

    namespace {
        // Per-row tallies. Diagnostics, and the only thing a unit case can assert on: an
        // over-approximating set has no observable difference when a row fires too OFTEN, so
        // "did this row fire at all, for this buffer" has to be readable directly.
        Array<Uint64, static_cast<SizeT>(GpuWriteProducer::Count)> g_producerMarks{};

        // The transform-feedback rows, shared by the draw walk (row 3) and by
        // glEndTransformFeedback (row 5). Both mark the SAME set - the capture targets of the
        // capture program - and the split exists only so the two can be counted apart.
        void MarkTransformFeedbackTargets(GpuWriteProducer producer) {
            auto& context = MG_State::pGLContext;
            if (!context) return;
            if (!context->IsTransformFeedbackActive()) return;
            const auto& program = context->GetTransformFeedbackProgram();
            if (program == nullptr) return;
            const SizeT declared = program->GetTransformFeedbackBufferCount();
            const SizeT count =
                declared < MG_State::GLState::GLContext::MAX_TRANSFORM_FEEDBACK_BUFFERS
                    ? declared
                    : static_cast<SizeT>(MG_State::GLState::GLContext::MAX_TRANSFORM_FEEDBACK_BUFFERS);
            for (SizeT i = 0; i < count; ++i) {
                const auto& point =
                    context->GetBufferBindingPoint(BufferTarget::TransformFeedback, static_cast<Uint>(i));
                MarkBufferForProducer(point.GetBoundObject(), producer);
            }
        }

        void MarkShaderStorageBindings() {
            auto& context = MG_State::pGLContext;
            if (!context) return;
            // The TOUCHED count, exactly as the backend twin uses it
            // (DirectGLES.cpp:559-560): the binding-point array is 84 entries wide and
            // walking all of them on every draw is what the high-water mark exists to avoid.
            const SizeT points = context->GetTouchedBufferBindingPointCount(BufferTarget::ShaderStorage);
            for (SizeT i = 0; i < points; ++i) {
                const auto& point = context->GetBufferBindingPoint(BufferTarget::ShaderStorage, static_cast<Uint>(i));
                MarkBufferForProducer(point.GetBoundObject(), GpuWriteProducer::ShaderStorageBinding);
            }
        }

        void MarkAtomicCounterBindings() {
            auto& context = MG_State::pGLContext;
            if (!context) return;
            // WIDER THAN ITS BACKEND TWIN, ON PURPOSE AND IN THE SAFE DIRECTION.
            // SyncAtomicCounterBuffers (DirectGLES.cpp:578-583) walks the GL bindings the
            // TRANSPILED program declared, which is a subset of what is bound; the client has
            // the bindings but not that per-program list at this point, so it marks every
            // touched atomic-counter point. Over-approximating costs a readback the narrowing
            // channel then removes. Under-approximating reads a stale counter and says
            // nothing, which is the failure every atomic-counter conformance case is.
            const SizeT points = context->GetTouchedBufferBindingPointCount(BufferTarget::AtomicCounter);
            for (SizeT i = 0; i < points; ++i) {
                const auto& point = context->GetBufferBindingPoint(BufferTarget::AtomicCounter, static_cast<Uint>(i));
                MarkBufferForProducer(point.GetBoundObject(), GpuWriteProducer::AtomicCounterBinding);
            }
        }

        void MarkWritableImageBufferTextures() {
            auto& context = MG_State::pGLContext;
            if (!context) return;
            // The backend keeps a bitset of writable image-buffer units
            // (DirectGLES.cpp:2350-2352, maintained from its own SyncImageTextureBinding) and
            // the client has no equivalent, so it sweeps. The sweep is bounded by the array,
            // not by a device limit read: MaxImageUnits would be a backend read, and a stale
            // or absent backend object would silently shorten the walk - which is the one
            // direction this set may not fail in. The loop body is a null test on a
            // contiguous array until a unit is actually bound. P5 records cost and does not
            // gate on it (2026-09-08 rule); an image-unit high-water mark on GLContext is the
            // obvious narrowing and belongs with P8's binding-walk migration.
            for (Int unit = 0; unit < MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS; ++unit) {
                const auto& binding = context->GetImageTextureBinding(unit);
                if (!ImageUnitIsAWritableBufferTexture(binding)) continue;
                auto* textureBuffer = static_cast<MG_State::GLState::TextureObjectBuffer*>(binding.Texture.get());
                MarkBufferForProducer(textureBuffer->GetBufferBindingSlot().GetBoundObject(),
                                      GpuWriteProducer::WritableImageBufferTexture);
            }
        }
    } // namespace

    Bool GpuWriteSetIsClientSide() {
        return MG_Config::Transport != MG_Config::TransportMode::Monolith;
    }

    Bool ImageUnitIsAWritableBufferTexture(const ImageTextureBinding& binding) {
        // Verbatim from IsWritableImageBufferTexture (DirectGLES.cpp:2354-2357). All three
        // terms are client state; none of them is a driver question.
        return binding.Texture != nullptr && binding.Access != GL_READ_ONLY &&
               binding.Texture->GetStorageType() == TextureStorageType::Buffer;
    }

    void MarkBufferForProducer(const SharedPtr<BufferObject>& buffer, GpuWriteProducer producer) {
        if (!GpuWriteSetIsClientSide()) return;
        if (buffer == nullptr) return;
        if (producer >= GpuWriteProducer::Count) return;
        buffer->MarkGpuWritten();
        ++g_producerMarks[static_cast<SizeT>(producer)];
    }

    void MarkGpuWritesForDraw() {
        if (!GpuWriteSetIsClientSide()) return;
        MarkShaderStorageBindings();
        MarkAtomicCounterBindings();
        MarkWritableImageBufferTextures();
        MarkTransformFeedbackTargets(GpuWriteProducer::TransformFeedbackCapture);
    }

    void MarkGpuWritesForDispatch() {
        if (!GpuWriteSetIsClientSide()) return;
        MarkShaderStorageBindings();
        MarkAtomicCounterBindings();
        MarkWritableImageBufferTextures();
    }

    void MarkReadPixelsPackBuffer() {
        if (!GpuWriteSetIsClientSide()) return;
        auto& context = MG_State::pGLContext;
        if (!context) return;
        MarkBufferForProducer(context->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject(),
                              GpuWriteProducer::ReadPixelsPackBuffer);
    }

    void MarkEndTransformFeedbackCaptureTargets() {
        if (!GpuWriteSetIsClientSide()) return;
        MarkTransformFeedbackTargets(GpuWriteProducer::EndTransformFeedbackCapture);
    }

    Uint64 ProducerMarkCount(GpuWriteProducer producer) {
        if (producer >= GpuWriteProducer::Count) return 0;
        return g_producerMarks[static_cast<SizeT>(producer)];
    }

    void ResetProducerMarkCountsForTest() {
        g_producerMarks.fill(0);
    }

    Bool BufferWritebackIsReachable(const BufferObject& buffer) {
        if (buffer.GetSize() == 0) return false;
#if MOBILEGL_PIPE_PUSH
        return MG_Pipe::MGPipeResourceSubsystemEnabled();
#else
        return false;
#endif
    }

    SizeT BufferWritebackSliceBytes() {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return 0;
        // A quarter of the ring, not the MaxRecordBytes half: the record carries its own
        // header and the 24-byte EventBufferWritebackHead beside the payload, and the ring
        // may still hold a few small events (gpu-written, gl-error) posted earlier in the
        // same verb's apply. Each slice round-trips with its own barrier + drain, so the
        // ring never holds more than one slice's bytes.
        const Uint64 slice = session->EventRingCapacityBytes() / 4;
        // A floor so a pathologically small operator-supplied ring cannot make the slicing
        // loop in SyncGpuWrites spin at zero width; such a ring is broken anyway, and the
        // producer's Fatal{EventRingOverflow} names it on the first post.
        return static_cast<SizeT>(slice < 4096 ? 4096 : slice);
    }

    void AwaitBufferWriteback(BufferObject& buffer) {
        // THE WAIT IS THE BARRIER'S WAIT (R-3). The reply-slot id IS the record's seq, so
        // "appliedSeq reached my readback" and "my answer is back" are one condition, and
        // ClientSession::EmitAndWait is what pays for it. With no session - a build-split lane
        // running monolith, and every unit case - the emission WAS the application,
        // synchronously, so the writeback has already landed and there is nothing to wait for.
        // Spelling that as "return" rather than as a loop is deliberate: a loop here would be
        // a hang in exactly that configuration, which is the configuration every gate lane
        // runs.
        if (ClientSession::Active() == nullptr) return;

        // AND THE OTHER ARM IS A NAMED FATAL, NOT AN EMPTY BODY. A session exists, so the
        // apply side is no longer synchronous, and if the flag is still set the shadow this
        // caller is about to read is STALE - which is the whole failure the third state was
        // introduced to stop. An empty body here would make that failure silent and would let
        // s1/c1 land a session without noticing that nobody ever wrote the wait; a stub that
        // aborts by name is the house shape for exactly this (EmitTables.cpp's
        // UnmigratedVerbFatal), and it is what gives the hole a red spelling before the
        // transport arrives.
        if (!buffer.HasOutstandingGpuWrite()) return;
        MGLOG_F("MGPipe: Fatal{UnimplementedWritebackWait} - a ClientSession is active and buffer %u "
                "still has an outstanding GPU write after its readback was emitted. The wait is "
                "ClientSession::EmitAndWait's (R-3: the reply slot id IS the record seq); P5 package "
                "b1 landed the third state and s1/c1 own the wait itself.",
                buffer.GetExternalIndex());
        std::abort();
    }

} // namespace MobileGL::MG_Remote::Client
