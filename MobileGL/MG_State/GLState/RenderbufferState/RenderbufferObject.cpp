// MobileGL - MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderbufferObject.h"
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/StateObjectDeathNotice.h>
// The contract's own door, exactly as the texture half takes it (c0b): the four renderbuffer
// hooks this file calls are declared in MG_Pipe/PipeMutation.h and defined in
// MG_Impl/Pipe/PipeFill.cpp, so no MG_State translation unit includes the client's emitter.
#include <MG_Pipe/PipeMutation.h>

#include <atomic>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            namespace {
                // Starts at 1 so a zero-initialized cache slot can never carry a live
                // renderbuffer's id.
                std::atomic<Uint64> g_nextRenderbufferLifetimeId{1};
            }

            Uint64 RenderbufferObject::AllocateLifetimeId() {
                return g_nextRenderbufferLifetimeId.fetch_add(1, std::memory_order_relaxed);
            }

            RenderbufferObject::RenderbufferObject(Uint externalIndex) : m_externalIndex(externalIndex) {
#if MOBILEGL_PIPE_PUSH
                // P4a D-D1: resource_create from the constructor, carrying no storage. A
                // renderbuffer is an INDEPENDENT class on the wire - it shares MGPResourceDesc's
                // shape with textures and buffers and nothing else - and its handle is minted
                // whatever the subsystem bitmask says, because MGPSurface::Res names it out of the
                // framebuffer subsystem.
                // TWO CALLS AND NOT ONE (c0b): the mint is unconditional in a push build
                // because a renderbuffer is named by handle out of the framebuffer subsystem
                // whether or not its own family is switched on; the create is what the gate in
                // PipeFill.cpp decides.
                MG_Pipe::MGPipeMintRenderbufferHandle(*this);
                MG_Pipe::MGPipeEmitRenderbufferResourceCreate(*this);
#endif
            }

#if MOBILEGL_PIPE_PUSH
            RenderbufferObject::~RenderbufferObject() {
                // P4a D-I1, the fixed three-step order: the wire delete first (published-gated,
                // because a slot is not evidence of a record), then the death notice - the P2
                // step-e2 announcement that used to stand here alone, raised while the handle
                // still resolves - and the slot last. Steps 2 and 3 are the contract's helper;
                // step 1 is this package's, one statement earlier, because the helper's file
                // belongs to the contract package for the whole phase.
                // All three steps are the contract's helper (c0b); v1's separate step-1 call
                // is deleted, not kept, for the reason ~TextureObjectBase states in full.
                MG_Pipe::MGPipeEmitRenderbufferDestroyAndFree(m_lifetimeId);
            }
#endif

            Uint RenderbufferObject::GetExternalIndex() const {
                return m_externalIndex;
            }

            Int RenderbufferObject::GetWidth() const {
                return m_width;
            }

            Int RenderbufferObject::GetHeight() const {
                return m_height;
            }

            TextureInternalFormat RenderbufferObject::GetInternalFormat() const {
                return m_internalFormat;
            }

            Bool RenderbufferObject::IsAllocated() const {
                return m_allocated;
            }

            Int RenderbufferObject::GetRedSize() const {
                return m_componentSizes.Red;
            }

            Int RenderbufferObject::GetGreenSize() const {
                return m_componentSizes.Green;
            }

            Int RenderbufferObject::GetBlueSize() const {
                return m_componentSizes.Blue;
            }

            Int RenderbufferObject::GetAlphaSize() const {
                return m_componentSizes.Alpha;
            }

            Int RenderbufferObject::GetDepthSize() const {
                return m_componentSizes.Depth;
            }

            Int RenderbufferObject::GetStencilSize() const {
                return m_componentSizes.Stencil;
            }

            Int RenderbufferObject::GetSamples() const {
                return m_samples;
            }

            const ComponentSizes& RenderbufferObject::GetComponentSizes() const {
                return m_componentSizes;
            }

            void RenderbufferObject::SetInternalFormat(TextureInternalFormat format) {
                m_internalFormat = format;
                m_componentSizes = MG_Util::GetComponentSizesForInternalFormat(format);
#if MOBILEGL_PIPE_PUSH
                PipePublishDescriptor();
#endif
            }

            void RenderbufferObject::AllocateStorage(IntVec2 size) {
                m_width = size.x();
                m_height = size.y();
                m_allocated = true;
#if MOBILEGL_PIPE_PUSH
                PipePublishDescriptor();
#endif
            }

            void RenderbufferObject::SetSamples(Int samples) {
                m_samples = samples;
#if MOBILEGL_PIPE_PUSH
                PipePublishDescriptor();
#endif
            }

#if MOBILEGL_PIPE_PUSH
            // D-D2: THE RENDERBUFFER PUBLICATION HOLE, CLOSED BY EMISSION AND NOT BY A NEW
            // VERSION. These three setters bump no version and raise no notice, so
            // `glBindRenderbuffer; glRenderbufferStorage(newSize)` on an attached renderbuffer
            // was invisible to everything downstream. Emitting from the storage entry point
            // closes the RESOURCE half; a version counter here would resize the pull build's
            // object and break G1.
            //
            // THE FRAMEBUFFER HALF IS THE AGGREGATE BUMP BELOW (P4a fable seam F-3), and the
            // sentence that used to end the paragraph above - "widening the shutter would fire
            // the framebuffer emission on an unrelated renderbuffer write" - was the seam:
            // set_framebuffer_state inlines an attachment's InternalFormat, extent and Samples at
            // emission (D-C1), so re-storaging an ATTACHED renderbuffer left the framebuffer
            // record - and the handle arm's four cross-object masks - describing the previous
            // storage while the resource record described the new one. The bump costs one
            // framebuffer re-emission per storage definition, whether or not the object is
            // attached, which the emitter's content hash suppresses when nothing it inlines
            // moved; it is not a counter on this object.
            //
            // The emitter dedupes on the built descriptor, so glRenderbufferStorage's three-setter
            // sequence publishes once rather than three times.
            void RenderbufferObject::PipePublishDescriptor() {
                MG_Pipe::MGPipeEmitRenderbufferResourceRespecify(*this);
                MGP_NOTE_AGGREGATE(FramebufferAttachment);
            }
#endif
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
