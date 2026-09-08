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
#if MOBILEGL_PIPE_PUSH
// The second and last MG_State translation unit that sees the client's texture emitter. A
// renderbuffer has no base class to hang protected helpers on and exactly one .cpp, so the
// include is the whole coupling; see MG_Impl/Pipe/TextureEmit.h's header comment for why the
// declaration cannot live in MG_Pipe/PipeMutation.h this phase.
#include <MG_Impl/Pipe/TextureEmit.h>
#endif

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
                MG_Pipe::MGPipeMintAndCreateRenderbuffer(*this);
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
                MG_Pipe::MGPipeEmitRenderbufferResourceDestroy(m_lifetimeId);
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
            // VERSION. These three setters bump no version and raise no notice, and the
            // framebuffer dirty bit's shutter does not move when an ALREADY-ATTACHED renderbuffer
            // is re-storaged - so `glBindRenderbuffer; glRenderbufferStorage(newSize)` on an
            // attached renderbuffer was invisible to everything downstream. Emitting from the
            // storage entry point closes it; a version counter here would resize the pull build's
            // object and break G1, and widening the shutter would fire the framebuffer emission on
            // an unrelated renderbuffer write.
            //
            // The emitter dedupes on the built descriptor, so glRenderbufferStorage's three-setter
            // sequence publishes once rather than three times.
            void RenderbufferObject::PipePublishDescriptor() {
                MG_Pipe::MGPipeEmitRenderbufferRespecify(*this);
            }
#endif
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
