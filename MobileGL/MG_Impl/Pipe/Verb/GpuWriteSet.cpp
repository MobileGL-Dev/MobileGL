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
            // the client has no equivalent, so it sweeps. The sweep is NOT bounded by a device
            // limit read: MaxImageUnits would be a backend read, and a stale or absent backend
            // object would silently shorten the walk - which is the one direction this set may
            // not fail in.
            //
            // IT IS BOUNDED BY THE FRONTEND's OWN IMAGE-UNIT HIGH-WATER MARK (P5d round 3,
            // package C). The paragraph that stood here said the narrowing "belongs with P8's
            // binding-walk migration"; the 2026-09-17 inproc profile moved it forward -
            // MarkWritableImageBufferTextures was 2.15% self / 3.59% inclusive of the GL thread
            // (GetImageTextureBinding 1.37 of it) at ~852 draws/frame, in a workload that never
            // binds an image at all, purely because the walk was MAX_TEXTURE_IMAGE_UNITS (192)
            // units wide unconditionally - 192 GetImageTextureBinding reads per draw for a
            // context that has never bound one.
            //
            // AND THE MARK IS SAFE IN THE ONLY DIRECTION THAT MATTERS. It is written by
            // glBindImageTexture itself (GL_Texture.cpp, TextureState::NoteImageUnitTouched) and
            // it only ever grows - an unbind, a delete-unbind, a context that stops using an
            // image unit all leave it where it was - so `unit <= mark` can only be WIDER than
            // the set of units that hold a binding, never narrower. -1 means no image unit has
            // ever been bound in this context, and then this is one compare.
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
        return MG_Pipe::MGPipeResourceSubsystemEnabled();
    }

} // namespace MobileGL::MG_Record
