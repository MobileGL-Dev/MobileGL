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

    // WHICH SUBSYSTEM BIT THIS BUILD ACTUALLY EMITS FOR. PipeFill.cpp ORs the four per-family
    // constants into kMGPipeWiredSubsystems, so the bit is added by the commit that gives the
    // emitters their bodies, with no file touched twice - and a Coverage.def row can never
    // silently drop a field on the floor before the call that carries it exists.
    //
    // TURNING IT ON RETIRES NO PULL. GetFramebufferBindingSlot is the family's one
    // Coverage.def emitted row and PipeFill.cpp's EmittedCallSuppliesTheWholeField answers
    // FALSE for it, with the reason: the field's storage is a BindingSlot<FramebufferObject> -
    // a frontend heap reference - and the call that supplies it carries eight-byte {slot, gen}
    // handles and a fully resolved descriptor. So this bit switches the EMISSION on and the
    // residual fill keeps writing the mirror, which is what keeps the verify lane at zero
    // divergence.
    inline constexpr Uint64 kMGPipeWiredFramebufferSubsystem = kMGPipeSubsystemFramebuffer;

    inline Bool MGPipeFramebufferSubsystemEnabled() {
        return (kMGPipeWiredFramebufferSubsystem & kMGPipeSubsystemFramebuffer) != 0 &&
               (MG_Config::Features.PipePush & kMGPipeSubsystemFramebuffer) != 0;
    }

    // ---------------------------------------------------------------------------------
    // D-C1: the MGPSurface builder, one pure function, one statement per field
    // ---------------------------------------------------------------------------------

    // MGPSurface::Kind's three constants ARE THE CONTRACT'S (ID-12 DV-4, c0c):
    // kMGPipeSurfaceKindNone / ...Texture / ...Renderbuffer live in MG_Pipe/MGPipeTypes.h under
    // exactly these names with the same MGPipeKind derivation and the same static_assert. This
    // package's copies were a redefinition in the same namespace and are deleted.

    // The upload target an attachment names, RESOLVED: an attachment made through an entry
    // point that carries no face token stores TextureUploadTarget::Unknown, and the record goes
    // out fully resolved - nothing in it may require a lookup on the far side.
    //
    // THE FALLBACK IS ONLY LEGAL FOR A SINGLE-TARGET TEXTURE (m1), and v1's was not. The
    // precedent it copied - FramebufferAttachmentObject::GetSize - needs an EXTENT, which is
    // identical across a cube map's six faces; face IDENTITY is not, so
    // `glFramebufferTexture(GL_COLOR_ATTACHMENT0, cube, 0)` resolved to targets[0] and the
    // record ASSERTED CubeMapPositiveX for a layered attachment that names all six. A texture
    // with exactly one upload target has a [0] that IS the truth; anything else keeps Unknown,
    // which is the value the field already carries for "this attachment names no single face"
    // and which Layered = 1 tells the reader to ignore.
    inline MobileGL::TextureUploadTarget MGPipeResolveAttachmentUploadTarget(
        const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        MobileGL::TextureUploadTarget resolved = attachment.GetTextureUploadTarget();
        if (resolved != MobileGL::TextureUploadTarget::Unknown) return resolved;
        const auto& texture = attachment.GetTexture();
        if (!texture) return MobileGL::TextureUploadTarget::Unknown;
        const auto& targets = texture->GetUploadTargets();
        return targets.size() == 1 ? targets[0] : MobileGL::TextureUploadTarget::Unknown;
    }

    // ONE PURE FUNCTION, ONE STATEMENT PER FIELD, and that shape is a gate requirement rather
    // than taste: G7's scripted control stops this conversion copying exactly one member
    // (MGPSurface::Layered) and expects the framebuffer suite to go red NAMING that field. A
    // loop or a memcpy would make the control unanswerable.
    //
    // `res` is handed in because resolving it needs the slot allocator and this function stays
    // pure; `internalFormat` is INLINE in the record on purpose, so the four cross-object masks
    // fall out at push time with no lookup on the far side.
    // THE EMPTY POINT IS THE ZERO-INITIALISED RECORD EXCEPT FOR ITS TWO TARGET FIELDS. Both
    // are Uint16 enumerations whose zero is a REAL value - TextureTarget::Texture1D and
    // TextureUploadTarget::Texture1D - so a reader that forgot to gate on Kind would read a
    // plausible wrong answer rather than a nonsense one. Unknown (0xFFFF) is what the contract
    // spells for TextureTarget (kMGPipeSurfaceNoTextureTarget) and m6 applies the same rule to
    // UploadTarget, which shares the collision ID-12 DV-3 ruled on for MGPSubData::Target.
    inline MGPSurface MGPipeEmptySurface() {
        MGPSurface surface{};
        surface.UploadTarget = static_cast<Uint16>(MobileGL::TextureUploadTarget::Unknown);
        surface.TextureTarget = kMGPipeSurfaceNoTextureTarget;
        return surface;
    }

    inline MGPSurface MGPipeBuildSurface(const MG_State::GLState::FramebufferAttachmentObject& attachment,
                                         MGPipeHandle res) {
        MGPSurface surface = MGPipeEmptySurface();
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
            // ID-12 DV-5: the field that WAS Pad0, and the size did not move. The four
            // cross-object masks all reduce to (format, TEXTURE TARGET) -
            // ShouldUseCaveatTextureFormat / BackendTextureFormatAddsAlpha - and no
            // TextureUploadTarget -> TextureTarget inverse exists anywhere in the tree, so
            // without this the inline InternalFormat cannot make them fall out at push time and
            // the backend keeps reading the frontend attachment objects.
            surface.TextureTarget = static_cast<Uint16>(texture->GetTarget());
            return surface;
        }
        const auto& renderbuffer = attachment.GetRenderbuffer();
        surface.Kind = kMGPipeSurfaceKindRenderbuffer;
        surface.InternalFormat = static_cast<Uint32>(renderbuffer->GetInternalFormat());
        surface.Layered = 0;
        surface.Level = 0;
        surface.Layer = 0;
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

    // m2: A DRAW-BUFFER TOKEN CAN NAME A COLOUR POINT THE RECORD CANNOT CARRY, and D-C3's
    // refusal loop only ever scanned ATTACHMENTS. `glDrawBuffers(1, {GL_COLOR_ATTACHMENT10})`
    // with nothing attached at 10 is legal state - draw-incomplete, but legal - and the index
    // above would have written 10 into a record whose Color[] is 8 wide, so the server would
    // index out of its own storage or invent a bound the record does not carry. Truncating
    // silently is the bug class this phase is closing, so the record is refused exactly as an
    // over-wide attachment is.
    inline Bool MGPipeDrawBufferIsInsideTheWireWidth(MobileGL::FramebufferAttachmentType buffer) {
        using MobileGL::FramebufferAttachmentType;
        if (buffer < FramebufferAttachmentType::Color0 || buffer > FramebufferAttachmentType::ColorMax) {
            return true; // None and the four default-framebuffer tokens; neither indexes Color[]
        }
        return static_cast<Int>(buffer) - static_cast<Int>(FramebufferAttachmentType::Color0) <
               static_cast<Int>(kMGPipeMaxColorAttachments);
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
        // MANDATORY, not optional: TextureTarget is a PipeFields.def row now, so a
        // field-wise copy that skipped it would suppress a record whose only moved field is
        // the attachment's texture target - and that field decides three of the four
        // cross-object masks.
        dst.TextureTarget = src.TextureTarget;
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
                drawOk = BuildFramebufferState(*drawFbo, MGPipeFramebufferTarget::Both, drawState);
            } else {
                if (drawFbo) {
                    drawOk = BuildFramebufferState(*drawFbo, MGPipeFramebufferTarget::Draw, drawState);
                }
                if (readFbo) {
                    readOk = BuildFramebufferState(*readFbo, MGPipeFramebufferTarget::Read, readState);
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

        // ID-19(c): EVERY DSA ENTRY POINT THAT HANDS A FRAMEBUFFER TO THE SERVER BY NAME IS
        // PRECEDED BY A RECORD FOR IT, and that is the phase's main correction rather than a
        // nicety. With only the two BOUND-target records, glClearNamedFramebufferfv(fbo) on an
        // unbound fbo made the backend mint a fresh driver framebuffer with NO ATTACHMENTS,
        // find no record for it, decline, and issue the clear against it anyway -
        // GL_INVALID_FRAMEBUFFER_OPERATION and nothing cleared, where the legacy arm cleared
        // correctly (esprytobj C-1).
        //
        // THE TARGET IS Named ONLY WHEN THE OBJECT IS BOUND TO NEITHER BINDING. A record always
        // writes FramebufferRecords[Fbo.Slot]; Draw/Read/Both ADDITIONALLY set the bound
        // handle(s). So handing a currently-bound framebuffer a Named record would overwrite
        // the bound record's Target with one that says "no binding" while BoundFramebuffer
        // still names it, and the server would read a record whose Target contradicts the
        // binding it is resolved through. Re-asserting the binding the object already has is
        // free (the content hash suppresses it) and keeps the two consistent.
        //
        // Returns the bytes that went on the wire.
        Uint64 EmitFramebufferByName(const FramebufferObject& fbo) {
            if (!MGPipeFramebufferSubsystemEnabled()) return 0;
            MGPipeFramebufferTarget target = MGPipeFramebufferTarget::Named;
            const Bool boundToDraw = IsBoundTo(fbo, MobileGL::FramebufferTarget::Draw);
            const Bool boundToRead = IsBoundTo(fbo, MobileGL::FramebufferTarget::Read);
            if (boundToDraw && boundToRead) {
                target = MGPipeFramebufferTarget::Both;
            } else if (boundToDraw) {
                target = MGPipeFramebufferTarget::Draw;
            } else if (boundToRead) {
                target = MGPipeFramebufferTarget::Read;
            }

            MGPFramebufferState state{};
            if (!BuildFramebufferState(fbo, target, state)) return 0;

            // THE SUPPRESSOR IS KEYED BY THE FRAMEBUFFER THE RECORD NAMES, never by one global
            // slot (MGPipeTypes.h states the rule): two different objects' Named records in a
            // row must both go out, and a Named record must never be suppressed against the
            // same object's bound record or the reverse. Target is a ContentHash input, so the
            // second half holds by construction; the per-object table is what buys the first.
            // The two BOUND latches stay what they are - "does the server's draw/read binding
            // already hold this record" - and a bound-target emission from here consults them,
            // because a rebind of an unchanged object must still move the binding.
            if (target == MGPipeFramebufferTarget::Named) {
                NamedEntry& entry = NamedEntryFor(state.Fbo);
                if (entry.Has && entry.Gen == state.Fbo.Gen && entry.LastHash == state.ContentHash) {
                    return 0;
                }
                const Uint64 bytes = Emit(state);
                entry.Has = true;
                entry.Gen = state.Fbo.Gen;
                entry.LastHash = state.ContentHash;
                return bytes;
            }
            if (target == MGPipeFramebufferTarget::Both) {
                if (state.ContentHash == m_lastEmitted[kDraw] && state.ContentHash == m_lastEmitted[kRead]) {
                    return 0;
                }
                const Uint64 bytes = Emit(state);
                m_lastEmitted[kDraw] = state.ContentHash;
                m_lastEmitted[kRead] = state.ContentHash;
                return bytes;
            }
            const SizeT slot = target == MGPipeFramebufferTarget::Read ? kRead : kDraw;
            if (state.ContentHash == m_lastEmitted[slot]) return 0;
            const Uint64 bytes = Emit(state);
            m_lastEmitted[slot] = state.ContentHash;
            return bytes;
        }

        // ---- what a unit case reads. The emitter builds INTO these and hands the applier the
        // same objects, so "what was emitted" costs no copy. ----
        // ---- the death half (P4a final review C-2) ----
        //
        // Called by the contract's death helper before the slot is freed (there is no wire
        // delete for this kind, D-I2, so this is the only client-side thing a framebuffer's
        // death has to do). The per-object Named latch is the entry: a recycled handle's Gen
        // already refuses the stale latch, so this is hygiene rather than a fix - the rule
        // (ID-8) is that whatever mints a handle retires everything it keeps under it at the
        // death, and every P4a kind takes the same shape. Gen-keyed for a late notice.
        void NoteFramebufferDied(MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_named.size()) return;
            if (m_named[slot].Gen == handle.Gen) m_named[slot] = NamedEntry{};
        }
        // "Does this emitter hold a Named-record latch for this handle at its generation."
        Bool NamedRecordIsLatched(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_named.size()) return false;
            return m_named[slot].Has && m_named[slot].Gen == handle.Gen;
        }

        const MGPFramebufferState& LastDraw() const { return m_lastDraw; }
        const MGPFramebufferState& LastRead() const { return m_lastRead; }
        const MGPFramebufferState& LastNamed() const { return m_lastNamed; }
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
            // The per-object latch goes too, and the safe direction is why: MGPipeApplierReset
            // keeps FramebufferRecords standing (they are object state, ID-19(b)) but
            // ReleaseObjectRecords clears the whole table, and this emitter cannot tell the two
            // scopes apart from here. Keeping a latch across a table that may have been dropped
            // would suppress the one record that had to go out; dropping it costs one extra
            // 304-byte record per named framebuffer after a context switch.
            m_named.clear();
        }

        void ResetCounters() { m_emissions = m_refusals = 0; }

        void ResetForTest() {
            Reset();
            ResetCounters();
            m_lastDraw = MGPFramebufferState{};
            m_lastRead = MGPFramebufferState{};
            m_lastNamed = MGPFramebufferState{};
        }

    private:
        static constexpr SizeT kDraw = 0;
        static constexpr SizeT kRead = 1;

        Uint64 Emit(const MGPFramebufferState& state) {
            if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Named)) {
                m_lastNamed = state;
            } else if (state.Target == static_cast<Uint8>(MGPipeFramebufferTarget::Read)) {
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

        // ONE RECORD DESCRIBES ONE FRAMEBUFFER OBJECT - the one named by `fbo` - and every
        // field in it is a property of THAT object. Target is the only binding-specific one.
        //
        // ReadSurface IS RESOLVED FROM THIS FRAMEBUFFER'S OWN READ BUFFER UNDER EVERY TARGET,
        // Named included (c0e / MGPipeTypes.h). v1 resolved a Draw record's ReadSurface from
        // the READ-bound object, which was D-C2's letter and muddled in substance: the record
        // then described a surface that is not part of the framebuffer its own Fbo names, and a
        // glReadBuffer on the read FBO moved the DRAW record's ContentHash and forced a
        // redundant draw emission. Resolving it per object is what makes the
        // read-buffer-shared-FBO defect class unrepresentable rather than merely fixed - the
        // record carries a surface, not an index, and no field of it refers to "whatever is
        // bound".
        Bool BuildFramebufferState(const FramebufferObject& fbo, MGPipeFramebufferTarget target,
                                   MGPFramebufferState& out) {
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

            // m2, THE SAME REFUSAL ONE FIELD OVER. A draw-buffer token may name a colour point
            // at or above the wire width with nothing attached there, which the loop above
            // cannot see; MGPipeDrawBufferIndex would then write 8..31 into an 8-wide array.
            {
                const auto& tokens = fbo.GetDrawBuffers();
                for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
                    if (MGPipeDrawBufferIsInsideTheWireWidth(tokens[i])) continue;
                    MGLOG_E_ONCE("MGPipe: framebuffer %u names colour point %d in draw buffer %u, which is "
                                 "at or above the wire width of %u - set_framebuffer_state is refused "
                                 "rather than truncated and the legacy arm runs",
                                 fbo.GetExternalIndex(),
                                 static_cast<Int>(tokens[i]) -
                                     static_cast<Int>(FramebufferAttachmentType::Color0),
                                 static_cast<Uint>(i), static_cast<Uint>(kMGPipeMaxColorAttachments));
                    ++m_refusals;
                    return false;
                }
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
            out.ReadSurface = SurfaceOf(fbo, fbo.GetReadBuffer());

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

        static Bool IsBoundTo(const FramebufferObject& fbo, MobileGL::FramebufferTarget target) {
            if (MG_State::pGLContext == nullptr) return false;
            const auto& bound = MG_State::pGLContext->GetFramebufferBindingSlot(target).GetBoundObject();
            return bound && bound.get() == &fbo;
        }

        struct NamedEntry {
            Uint32 Gen = 0;
            Uint64 LastHash = 0;
            Bool Has = false;
        };

        NamedEntry& NamedEntryFor(MGPipeHandle fbo) {
            const SizeT slot = fbo.Slot;
            if (slot >= m_named.size()) m_named.resize(slot + 1);
            return m_named[slot];
        }

        MGPSurface SurfaceOf(const FramebufferObject& fbo, FramebufferAttachmentType type) {
            if (type == FramebufferAttachmentType::None || type == FramebufferAttachmentType::Unknown) {
                return MGPipeEmptySurface();
            }
            const auto& attachment = fbo.GetAttachment(type);
            if (attachment.IsEmpty()) return MGPipeEmptySurface();
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
        // The per-FRAMEBUFFER suppressor for Named records, slot-indexed with the generation
        // checked, exactly as the applier's own table is. A framebuffer has no wire lifetime
        // (D-I2), so a successor simply overwrites its predecessor's entry.
        Vector<NamedEntry> m_named;
        MGPFramebufferState m_lastDraw{};
        MGPFramebufferState m_lastRead{};
        MGPFramebufferState m_lastNamed{};
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
