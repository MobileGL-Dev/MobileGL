// MobileGL - MobileGL/MG_Impl/Pipe/Verb/GpuWriteSet.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: the client GPU-write set (GpuWriteSet.h), moved from MG_Remote/Client/GpuWritePending.cpp.

#include "GpuWriteSet.h"

#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_State/GLState/TextureState/TextureState.h>

namespace MobileGL::MG_Record {

    using MG_State::GLState::BufferObject;
    using MG_State::GLState::ImageTextureBinding;
    using MobileGL::BufferTarget;

    namespace {
        // Per-row tallies. Diagnostics, and the only thing a unit case can assert on: an
        // over-approximating set has no observable difference when a row fires too OFTEN, so
        // "did this row fire at all, for this buffer" has to be readable directly.
        Array<Uint64, static_cast<SizeT>(GpuWriteProducer::Count)> g_producerMarks{};

        // The transform-feedback rows, shared by the draw walk (row 3) and by
        // glEndTransformFeedback (row 5). Both mark the SAME set - the capture targets of the
        // capture program - and the split exists only so the two can be counted apart.
        void MarkTransformFeedbackTargets(MG_State::GLState::GLContext* context, GpuWriteProducer producer) {
            if (!context) return;
            if (!context->IsTransformFeedbackActive()) return;
            const auto& program = context->GetTransformFeedbackProgram();
            if (program == nullptr) return;
            const SizeT declared = program->GetTransformFeedbackBufferCount();
            const SizeT count = declared < MG_State::GLState::GLContext::MAX_TRANSFORM_FEEDBACK_BUFFERS
                                    ? declared
                                    : static_cast<SizeT>(MG_State::GLState::GLContext::MAX_TRANSFORM_FEEDBACK_BUFFERS);
            for (SizeT i = 0; i < count; ++i) {
                const auto& point =
                    context->GetBufferBindingPoint(BufferTarget::TransformFeedback, static_cast<Uint>(i));
                MarkBufferForProducer(point.GetBoundObject(), producer);
            }
        }

        void MarkShaderStorageBindings(MG_State::GLState::GLContext* context) {
            if (!context) return;
            // The TOUCHED count, exactly as the backend twin uses it
            // (SyncBufferBindingPointsByRecord, DirectGLESBuffers.cpp): the binding-point array is 84 entries wide and
            // walking all of them on every draw is what the high-water mark exists to avoid.
            const SizeT points = context->GetTouchedBufferBindingPointCount(BufferTarget::ShaderStorage);
            for (SizeT i = 0; i < points; ++i) {
                const auto& point = context->GetBufferBindingPoint(BufferTarget::ShaderStorage, static_cast<Uint>(i));
                MarkBufferForProducer(point.GetBoundObject(), GpuWriteProducer::ShaderStorageBinding);
            }
        }

        void MarkAtomicCounterBindings(MG_State::GLState::GLContext* context) {
            if (!context) return;
            // WIDER THAN ITS BACKEND TWIN, ON PURPOSE AND IN THE SAFE DIRECTION.
            // SyncAtomicCounterBuffers (DirectGLESBuffers.cpp) walks the GL bindings the
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

        void MarkWritableImageBufferTextures(MG_State::GLState::GLContext* context) {
            if (!context) return;
            // The client has no bitset of writable image-buffer units, so it sweeps them, bounded
            // by the context's image-unit high-water mark rather than a backend limit, which a
            // stale or absent backend object could shorten. glBindImageTexture sets the mark and
            // it only grows (unbinding leaves it), so it never excludes a bound unit; -1 means no
            // image unit was ever bound and the walk is one compare.
            const Int highest = context->GetMaxTouchedImageUnit();
            for (Int unit = 0; unit <= highest && unit < MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS;
                 ++unit) {
                const auto& binding = context->GetImageTextureBinding(unit);
                if (!ImageUnitIsAWritableBufferTexture(binding)) continue;
                auto* textureBuffer = static_cast<MG_State::GLState::TextureObjectBuffer*>(binding.Texture.get());
                MarkBufferForProducer(textureBuffer->GetBufferBindingSlot().GetBoundObject(),
                                      GpuWriteProducer::WritableImageBufferTexture);
            }
        }
    } // namespace

    Bool GpuWriteSetIsClientSide() {
        // P13 W4a: wherever the backend reads records - every wire, and monolith's record arm -
        // the client owns the conservative set; the backend marks only its own twins.
        return MG_Config::DataArmIsRecord();
    }

    Bool ImageUnitIsAWritableBufferTexture(const ImageTextureBinding& binding) {
        // Verbatim from the backend's former IsWritableImageBufferTexture. All three
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
        // One read of the thread-local context for the four walks.
        MG_State::GLState::GLContext* const context = MG_State::pGLContext.get();
        MarkShaderStorageBindings(context);
        MarkAtomicCounterBindings(context);
        MarkWritableImageBufferTextures(context);
        MarkTransformFeedbackTargets(context, GpuWriteProducer::TransformFeedbackCapture);
    }

    void MarkGpuWritesForDispatch() {
        if (!GpuWriteSetIsClientSide()) return;
        MG_State::GLState::GLContext* const context = MG_State::pGLContext.get();
        MarkShaderStorageBindings(context);
        MarkAtomicCounterBindings(context);
        MarkWritableImageBufferTextures(context);
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
        MarkTransformFeedbackTargets(MG_State::pGLContext.get(), GpuWriteProducer::EndTransformFeedbackCapture);
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
        return MG_Pipe::MGPipeResourceSubsystemEnabled();
    }

} // namespace MobileGL::MG_Record
