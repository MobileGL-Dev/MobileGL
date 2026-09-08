// MobileGL - MobileGL/MG_Impl/Pipe/FramebufferEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's framebuffer family: set_framebuffer_state, emitted at the validate
// point once per bound TARGET that moved, or once with Target = Both when the two bindings
// name the same object.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT, and the
// split is the whole reason it exists this early. MG_Impl/Pipe/PipeFill.cpp is the contract
// package's for the entire phase - it carries Coverage.def's enum-coupled block, the validate
// point and the death helpers - so the emitter package must not edit it. What it edits instead
// is this header: the emitter's BODY, and the value of kMGPipeWiredFramebufferSubsystem below.
// That is what makes "no file is touched twice by two packages" structural rather than a
// convention, and it is what the bb2a236d semantic-merge trap taught (two branches green
// separately, the integrated tree not compiling).
//
// HEADER-ONLY, for the ownership reason Tracker.h and ResourceTracker.h both state: the root
// CMakeLists.txt that would name a new .cpp is the contract package's and is frozen behind the
// tag. MG_Impl/Pipe/PipeFill.cpp is the one translation unit that includes it in the library.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <xxhash.h>

#include <algorithm>

namespace MobileGL::MG_Pipe {

    // WHICH SUBSYSTEM BIT THIS BUILD ACTUALLY EMITS FOR, and it is 0 until the emitter below
    // has a body. PipeFill.cpp ORs the four per-family constants into kMGPipeWiredSubsystems,
    // so the bit is added by the commit that gives the emitters their bodies, with no file
    // touched twice - and a Coverage.def row can never silently drop a field on the floor
    // before the call that carries it exists.
    inline constexpr Uint64 kMGPipeWiredFramebufferSubsystem = 0;

    inline Bool MGPipeFramebufferSubsystemEnabled() {
        return (kMGPipeWiredFramebufferSubsystem & kMGPipeSubsystemFramebuffer) != 0 &&
               (MG_Config::Features.PipePush & kMGPipeSubsystemFramebuffer) != 0;
    }

    // ---------------------------------------------------------------------------------
    // D-C1: the MGPSurface builder, one pure function, one statement per field
    // ---------------------------------------------------------------------------------

    // MGPSurface::Kind. MGPipeKind is REUSED rather than a second three-value enum minted
    // beside it: it already spells Texture and Renderbuffer, its None is 0, and a
    // zero-initialised MGPSurface therefore already IS the empty attachment point the contract
    // describes ({Res = kMGPipeNullHandle, Kind = None} and every other field zero). If the
    // contract package later wants a dedicated enumeration beside MGPSurface it is a rename,
    // not a re-encoding.
    inline constexpr Uint8 kMGPipeSurfaceKindNone = static_cast<Uint8>(MGPipeKind::None);
    inline constexpr Uint8 kMGPipeSurfaceKindTexture = static_cast<Uint8>(MGPipeKind::Texture);
    inline constexpr Uint8 kMGPipeSurfaceKindRenderbuffer = static_cast<Uint8>(MGPipeKind::Renderbuffer);
    static_assert(kMGPipeSurfaceKindNone == 0,
                  "a zero-initialised MGPSurface must already be the empty attachment point");

    // The upload target an attachment names, RESOLVED: an attachment made through an entry
    // point that carries no face token stores TextureUploadTarget::Unknown, and the record goes
    // out fully resolved - nothing in it may require a lookup on the far side. This is the same
    // fallback FramebufferAttachmentObject::GetSize already applies to answer its own question.
    inline MobileGL::TextureUploadTarget MGPipeResolveAttachmentUploadTarget(
        const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        MobileGL::TextureUploadTarget resolved = attachment.GetTextureUploadTarget();
        if (resolved != MobileGL::TextureUploadTarget::Unknown) return resolved;
        const auto& texture = attachment.GetTexture();
        if (!texture) return MobileGL::TextureUploadTarget::Unknown;
        const auto& targets = texture->GetUploadTargets();
        return targets.empty() ? MobileGL::TextureUploadTarget::Unknown : targets[0];
    }

    // ONE PURE FUNCTION, ONE STATEMENT PER FIELD, and that shape is a gate requirement rather
    // than taste: G7's scripted control stops this conversion copying exactly one member
    // (MGPSurface::Layered) and expects the framebuffer suite to go red NAMING that field. A
    // loop or a memcpy would make the control unanswerable.
    //
    // `res` is handed in because resolving it needs the slot allocator and this function stays
    // pure; `internalFormat` is INLINE in the record on purpose, so the four cross-object masks
    // fall out at push time with no lookup on the far side.
    inline MGPSurface MGPipeBuildSurface(const MG_State::GLState::FramebufferAttachmentObject& attachment,
                                         MGPipeHandle res) {
        MGPSurface surface{};
        if (attachment.IsEmpty()) return surface;
        surface.Res = res;
        if (attachment.IsTexture()) {
            const auto& texture = attachment.GetTexture();
            surface.Kind = kMGPipeSurfaceKindTexture;
            surface.InternalFormat = static_cast<Uint32>(texture->GetFormat());
            surface.Layered = attachment.IsLayered() ? 1 : 0;
            surface.Level = static_cast<Uint16>(std::max<Int>(attachment.GetTextureLevel(), 0));
            surface.Layer = static_cast<Uint32>(std::max<Int>(attachment.GetTextureLayer(), 0));
            surface.UploadTarget = static_cast<Uint16>(MGPipeResolveAttachmentUploadTarget(attachment));
            return surface;
        }
        const auto& renderbuffer = attachment.GetRenderbuffer();
        surface.Kind = kMGPipeSurfaceKindRenderbuffer;
        surface.InternalFormat = static_cast<Uint32>(renderbuffer->GetInternalFormat());
        surface.Layered = 0;
        surface.Level = 0;
        surface.Layer = 0;
        surface.UploadTarget = 0;
        return surface;
    }

    // MGPFramebufferState::DrawBuffers[i]: an index INTO THIS RECORD'S OWN Color[] array, and
    // -1 for NONE, which is the field's documented convention read literally.
    //
    // THE FOUR DEFAULT-FRAMEBUFFER TOKENS map to 0, and that is a deliberate narrowing rather
    // than an oversight: a default framebuffer has one colour surface, this record carries it
    // in Color[0] (see MGPipeBuildFramebufferState), and IsDefault is what tells the server
    // which framebuffer it is looking at. The distinction the narrowing loses is FRONT versus
    // BACK and LEFT versus RIGHT, which MobileGL's frontend never gives a default framebuffer
    // in the first place - FramebufferObject's constructor seeds BackLeft and nothing writes
    // another. A phase that needs stereo has to widen the field, not re-encode this one.
    inline Int8 MGPipeDrawBufferIndex(MobileGL::FramebufferAttachmentType buffer) {
        using MobileGL::FramebufferAttachmentType;
        if (buffer == FramebufferAttachmentType::None) return -1;
        if (buffer >= FramebufferAttachmentType::Color0 && buffer <= FramebufferAttachmentType::ColorMax) {
            return static_cast<Int8>(static_cast<Int>(buffer) - static_cast<Int>(FramebufferAttachmentType::Color0));
        }
        return 0;
    }

    // ---------------------------------------------------------------------------------
    // D-C4: ContentHash, and the one input it must not swallow
    // ---------------------------------------------------------------------------------
    //
    // XXH64 over the WHOLE record with ContentHash itself zeroed, computed field-wise into a
    // zero-initialised staging copy so that no padding byte can enter the hash. Two jobs: the
    // server's render-pass memo key, and this client's emission suppressor.
    //
    // IT MUST COVER Fbo. A recycled framebuffer handle whose successor happens to carry an
    // identical attachment set would otherwise be suppressed against its predecessor; Fbo
    // carries Gen, so it cannot be.
    //
    // IT MUST COVER DrawBuffers[8], and this is the trap worth naming. The backend derives the
    // fragColor BROADCAST COUNT from the draw-buffer array, and it does that at the verb, from
    // the framebuffer state it then holds, precisely so a program can relink inside the same
    // draw. A hash that did not cover the array would let a suppressed set_framebuffer_state
    // mean "the draw buffers did not move" when they had, and the shader would be specialised
    // for the previous output shape. With the array in the hash, a suppression provably means
    // the array did not move, which provably means the broadcast count did not move.
    inline void MGPipeCopySurfaceForHash(MGPSurface& dst, const MGPSurface& src) {
        dst.Res = src.Res;
        dst.InternalFormat = src.InternalFormat;
        dst.Kind = src.Kind;
        dst.Layered = src.Layered;
        dst.Level = src.Level;
        dst.Layer = src.Layer;
        dst.UploadTarget = src.UploadTarget;
    }

    inline Uint64 MGPipeFramebufferStateContentHash(const MGPFramebufferState& state) {
        MGPFramebufferState staging{};
        staging.Fbo = state.Fbo;
        for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
            MGPipeCopySurfaceForHash(staging.Color[i], state.Color[i]);
        }
        MGPipeCopySurfaceForHash(staging.Depth, state.Depth);
        MGPipeCopySurfaceForHash(staging.Stencil, state.Stencil);
        MGPipeCopySurfaceForHash(staging.ReadSurface, state.ReadSurface);
        for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
            staging.DrawBuffers[i] = state.DrawBuffers[i];
        }
        staging.Width = state.Width;
        staging.Height = state.Height;
        staging.Layers = state.Layers;
        staging.Samples = state.Samples;
        staging.FixedSampleLocations = state.FixedSampleLocations;
        staging.IsDefault = state.IsDefault;
        staging.Complete = state.Complete;
        staging.Target = state.Target;
        // staging.ContentHash stays 0 - that is the whole point.
        return XXH64(&staging, sizeof(staging), 0);
    }

    // ---------------------------------------------------------------------------------
    // The emitter
    // ---------------------------------------------------------------------------------

    class MGPipeFramebufferEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;
        using FramebufferObject = MG_State::GLState::FramebufferObject;
        using FramebufferAttachmentType = MobileGL::FramebufferAttachmentType;

        // The handle for `fbo`. kMGPipeDefaultFramebuffer ({0,1}) for the default framebuffer,
        // which is what retires the four pDefaultFramebufferInfo->defaultFBO identity
        // comparisons into an ordinary handle compare; a client-minted {slot, gen} otherwise.
        //
        // Minted, never gated: a framebuffer handle is CLIENT state and costs one free-list pop.
        static MGPipeHandle HandleFor(const FramebufferObject& fbo) {
            if (fbo.IsDefaultFramebuffer()) return kMGPipeDefaultFramebuffer;
            return MGPipeSlots().Acquire(MGPipeKind::Framebuffer, fbo.GetLifetimeId());
        }

        // Returns the bytes that went on the wire, for the per-draw payload histogram.
        Uint64 EmitFramebufferState(GLContext& ctx) {
            if (!MGPipeFramebufferSubsystemEnabled()) return 0;
            const auto& drawFbo = ctx.GetFramebufferBindingSlot(MobileGL::FramebufferTarget::Draw).GetBoundObject();
            const auto& readFbo = ctx.GetFramebufferBindingSlot(MobileGL::FramebufferTarget::Read).GetBoundObject();
            if (!drawFbo && !readFbo) return 0;

            // ONE OBJECT BOUND TO BOTH TARGETS IS ONE RECORD WITH Target = Both, and that is
            // not an optimisation: Espryt's "same FBO as draw" skip is the habitat of the
            // read-buffer defect class, and a record that says which target it describes turns
            // "apply the draw buffers only for the draw target" from call-site discipline into
            // a one-line test on the far side.
            const Bool shared = drawFbo && readFbo && drawFbo.get() == readFbo.get();

            MGPFramebufferState drawState{};
            MGPFramebufferState readState{};
            Bool drawOk = false;
            Bool readOk = false;
            if (shared) {
                drawOk = BuildFramebufferState(*drawFbo, *drawFbo, MGPipeFramebufferTarget::Both, drawState);
            } else {
                if (drawFbo) {
                    drawOk = BuildFramebufferState(*drawFbo, readFbo ? *readFbo : *drawFbo,
                                                   MGPipeFramebufferTarget::Draw, drawState);
                }
                if (readFbo) {
                    readOk = BuildFramebufferState(*readFbo, *readFbo, MGPipeFramebufferTarget::Read, readState);
                }
            }
            if (!drawOk && !readOk) return 0;

            // THE SUPPRESSOR SLOT IS FED THE COMBINED ANSWER and the per-target latches decide
            // which of the two records actually goes out. The slot exists so that
            // InvalidateAll() on a fresh context reaches this family like every other, and so
            // that "nothing moved" costs one compare rather than two.
            const Uint64 drawHash = drawOk ? drawState.ContentHash : 0;
            const Uint64 readHash = readOk ? readState.ContentHash : 0;
            const Uint64 combined =
                MGPipeMixShutter(MGPipeMixShutter(drawHash, readHash), shared ? 1u : 0u);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetFramebufferState,
                                                             combined)) {
                return 0;
            }

            Uint64 bytes = 0;
            if (shared) {
                if (drawOk && (drawHash != m_lastEmitted[kDraw] || drawHash != m_lastEmitted[kRead])) {
                    bytes += Emit(drawState);
                    m_lastEmitted[kDraw] = drawHash;
                    m_lastEmitted[kRead] = drawHash;
                }
                return bytes;
            }
            if (drawOk && drawHash != m_lastEmitted[kDraw]) {
                bytes += Emit(drawState);
                m_lastEmitted[kDraw] = drawHash;
            }
            if (readOk && readHash != m_lastEmitted[kRead]) {
                bytes += Emit(readState);
                m_lastEmitted[kRead] = readHash;
            }
            return bytes;
        }

        // ---- what a unit case reads. The emitter builds INTO these and hands the applier the
        // same objects, so "what was emitted" costs no copy. ----
        const MGPFramebufferState& LastDraw() const { return m_lastDraw; }
        const MGPFramebufferState& LastRead() const { return m_lastRead; }
        Uint64 EmissionCount() const { return m_emissions; }
        Uint64 RefusedCount() const { return m_refusals; }

        // A fresh context: what the server has is no longer what this emitter last sent. Only
        // LATCHES reset here - MGPipeApplierReset clears the applier's DrawFramebuffer and
        // ReadFramebuffer working state, so these mirrors have to go with them or the first
        // emission after a make-current would be suppressed as unchanged and the server would
        // draw into the previous context's framebuffer. The suppressor slot is invalidated by
        // the validate point's own InvalidateAll(), beside this call.
        void Reset() {
            m_lastEmitted[kDraw] = 0;
            m_lastEmitted[kRead] = 0;
        }

        void ResetCounters() { m_emissions = m_refusals = 0; }

        void ResetForTest() {
            Reset();
            ResetCounters();
            m_lastDraw = MGPFramebufferState{};
            m_lastRead = MGPFramebufferState{};
        }

    private:
        static constexpr SizeT kDraw = 0;
        static constexpr SizeT kRead = 1;

        Uint64 Emit(const MGPFramebufferState& state) {
            if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Read)) {
                m_lastRead = state;
            } else {
                m_lastDraw = state;
                if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Both)) m_lastRead = state;
            }
            MGPipeApplySetFramebufferState(state);
            ++m_emissions;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::FramebufferEmissions, 1);
            }
            return sizeof(MGPFramebufferState);
        }

        // `fbo` is the object this record describes; `readFbo` is the object whose OWN read
        // buffer resolves ReadSurface, and for a Draw record that is the read-bound object
        // rather than this one.
        //
        // THE RESOLVED READ SURFACE IS WHAT STRUCTURALLY CLOSES THE read-buffer-shared-FBO
        // DEFECT CLASS: the record carries the surface, not an index, and it is resolved from
        // the READ framebuffer's own read buffer, so the "same FBO as draw" skip that used to
        // lose it cannot be expressed.
        Bool BuildFramebufferState(const FramebufferObject& fbo, const FramebufferObject& readFbo,
                                   MGPipeFramebufferTarget target, MGPFramebufferState& out) {
            // D-C3, THE CLIENT HALF OF THE BRING-UP REFUSAL. The wire array is 8 wide and
            // GetDynamicParameters().MaxColorAttachments is the driver's raw ES cap, not
            // clamped to 8 on the GLES path. An attachment point at or above the wire width
            // cannot be carried at all, so the record is REFUSED and the legacy arm runs -
            // truncating it silently is exactly the bug class this phase is closing. The
            // backend half of the same refusal (bit 9 declined at its first lookup, with one
            // ERROR naming the cap) rides ResolveFramebufferSubsystemArm.
            for (Int point = static_cast<Int>(FramebufferAttachmentType::Color0) +
                             static_cast<Int>(kMGPipeMaxColorAttachments);
                 point <= static_cast<Int>(FramebufferAttachmentType::ColorMax); ++point) {
                if (fbo.GetAttachment(static_cast<FramebufferAttachmentType>(point)).IsEmpty()) continue;
                MGLOG_E_ONCE("MGPipe: framebuffer %u has an attachment at colour point %d, which is at or "
                             "above the wire width of %u - set_framebuffer_state is refused rather than "
                             "truncated and the legacy arm runs",
                             fbo.GetExternalIndex(),
                             point - static_cast<Int>(FramebufferAttachmentType::Color0),
                             static_cast<Uint>(kMGPipeMaxColorAttachments));
                ++m_refusals;
                return false;
            }

            out = MGPFramebufferState{};
            out.Fbo = HandleFor(fbo);
            out.Target = static_cast<Uint8>(target);
            out.IsDefault = fbo.IsDefaultFramebuffer() ? 1 : 0;

            // THE COLOUR POINTS. A default framebuffer keeps its one colour surface under
            // BackLeft rather than under Color0, and the record has exactly one place to put
            // it: Color[0], which is also the index MGPipeDrawBufferIndex maps that token to,
            // so the array and the draw-buffer indices agree by construction.
            if (out.IsDefault != 0) {
                out.Color[0] = SurfaceOf(fbo, FramebufferAttachmentType::BackLeft);
            } else {
                for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
                    out.Color[i] = SurfaceOf(fbo, static_cast<FramebufferAttachmentType>(
                                                      static_cast<Int>(FramebufferAttachmentType::Color0) +
                                                      static_cast<Int>(i)));
                }
            }
            out.Depth = SurfaceOf(fbo, FramebufferAttachmentType::Depth);
            out.Stencil = SurfaceOf(fbo, FramebufferAttachmentType::Stencil);
            out.ReadSurface = SurfaceOf(readFbo, readFbo.GetReadBuffer());

            const auto& drawBuffers = fbo.GetDrawBuffers();
            for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
                out.DrawBuffers[i] = MGPipeDrawBufferIndex(drawBuffers[i]);
            }

            FillGeometry(fbo, out);
            // Complete is FramebufferObject::CheckCompleteness(), the FRONTEND-ONLY answer, and
            // never glCheckFramebufferStatus's: that entry point additionally consults the
            // backend's probed format-capability cache, and a client emitting it would be
            // reading the backend from the client side - the exact coupling this boundary
            // exists to remove. glCheckFramebufferStatus keeps answering from the frontend
            // exactly as it does today.
            out.Complete = fbo.CheckCompleteness() ? 1 : 0;
            out.ContentHash = MGPipeFramebufferStateContentHash(out);
            return true;
        }

        MGPSurface SurfaceOf(const FramebufferObject& fbo, FramebufferAttachmentType type) {
            if (type == FramebufferAttachmentType::None || type == FramebufferAttachmentType::Unknown) {
                return MGPSurface{};
            }
            const auto& attachment = fbo.GetAttachment(type);
            if (attachment.IsEmpty()) return MGPSurface{};
            MGPipeTextureEmitter& textures = MGPipeTextureEmitterInstance();
            // D-A4's two producers: an attachment point is what sets RENDER_TARGET and
            // DEPTH_STENCIL, the two sticky bind bits nothing set before P4a. Sticky and ORed,
            // so a texture that was ever a colour attachment keeps saying so, and the mask is
            // republished on the resource's next respecify.
            const Uint16 bit = (type == FramebufferAttachmentType::Depth ||
                                type == FramebufferAttachmentType::Stencil)
                                   ? static_cast<Uint16>(kMGPipeBindDepthStencil)
                                   : static_cast<Uint16>(kMGPipeBindRenderTarget);
            MGPipeHandle res = kMGPipeNullHandle;
            if (attachment.IsTexture()) {
                const auto& texture = attachment.GetTexture();
                res = textures.AcquireTexture(texture->GetLifetimeId(), texture.get());
                textures.NoteTextureBoundAs(res, bit);
            } else if (attachment.IsRenderbuffer()) {
                const auto& renderbuffer = attachment.GetRenderbuffer();
                res = textures.AcquireRenderbuffer(renderbuffer->GetLifetimeId());
                textures.NoteRenderbufferBoundAs(res, bit);
            }
            return MGPipeBuildSurface(attachment, res);
        }

        // The attachments' common extent, and the ARB_framebuffer_no_attachments defaults when
        // there is no attachment at all (GL 4.6 core table 23.24 - the shape a framebuffer with
        // no attachments rasterizes at).
        static void FillGeometry(const FramebufferObject& fbo, MGPFramebufferState& out) {
            Bool found = false;
            for (const auto& attachment : fbo.GetAllAttachmentObjects()) {
                if (attachment.IsEmpty()) continue;
                const IntVec3 size = attachment.GetSize();
                if (!found) {
                    out.Width = static_cast<Uint16>(std::clamp<Int>(size.x(), 0, 0xFFFF));
                    out.Height = static_cast<Uint16>(std::clamp<Int>(size.y(), 0, 0xFFFF));
                    out.Layers = static_cast<Uint16>(
                        attachment.IsLayered() ? std::clamp<Int>(size.z(), 1, 0xFFFF) : 1);
                    if (attachment.IsTexture()) {
                        const auto& texture = attachment.GetTexture();
                        out.Samples = static_cast<Uint16>(std::max<Int>(texture->GetSamples(), 0));
                        out.FixedSampleLocations = texture->HasFixedSampleLocations() ? 1 : 0;
                    } else {
                        out.Samples = static_cast<Uint16>(
                            std::max<Int>(attachment.GetRenderbuffer()->GetSamples(), 0));
                        out.FixedSampleLocations = 1;
                    }
                    found = true;
                }
            }
            if (found) return;
            out.Width = static_cast<Uint16>(std::clamp<Int>(fbo.GetDefaultWidth(), 0, 0xFFFF));
            out.Height = static_cast<Uint16>(std::clamp<Int>(fbo.GetDefaultHeight(), 0, 0xFFFF));
            out.Layers = static_cast<Uint16>(std::clamp<Int>(fbo.GetDefaultLayers(), 0, 0xFFFF));
            out.Samples = static_cast<Uint16>(std::clamp<Int>(fbo.GetDefaultSamples(), 0, 0xFFFF));
            out.FixedSampleLocations = fbo.GetDefaultFixedSampleLocations() ? 1 : 0;
        }

        Array<Uint64, 2> m_lastEmitted{};
        MGPFramebufferState m_lastDraw{};
        MGPFramebufferState m_lastRead{};
        Uint64 m_emissions = 0;
        Uint64 m_refusals = 0;
    };

    inline MGPipeFramebufferEmitter& MGPipeFramebufferEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason (MG_Impl/Pipe/Tracker.h): the
        // rule covers every MGPipe process singleton, not only the ones a frontend destructor
        // reaches today, and it is what keeps exit() out of a torn-down pipe.
        static MGPipeFramebufferEmitter* emitter = new MGPipeFramebufferEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
