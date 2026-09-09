// MobileGL - MobileGL/MG_Impl/Pipe/TextureEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's texture and renderbuffer family: resource_create from the object's
// constructor, resource_respecify from every storage-defining entry point, set_texture_params
// from the parameter mutators, and resource_subdata from the DRAIN LIST at the validate point.
//
// THE THREE OBJECT CALLS ARE NOT EMITTED FROM HERE'S CALLER, they are emitted from MG_State's
// own mutators - a constructor, a storage definition, a glTexParameter - exactly as P3a's
// buffer family is, because that is where the event happens. Only the sub-data drain runs at
// the validate point, which is the explicit exception ARCHITECTURE.md 5.1 makes for texture
// upload: walking every live texture per verb is the cost the drain list exists to avoid.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full: PipeFill.cpp is the contract package's for the whole
// phase, so the emitter package edits this header and the value of
// kMGPipeWiredTextureSubsystem below, and never that file.
//
// HEADER-ONLY, for the ownership reason Tracker.h and ResourceTracker.h both state.
//
// ---------------------------------------------------------------------------------------
// HOW MG_State REACHES THIS FILE: IT DOES NOT, AND THAT IS THE POINT (c0b, ID-13).
//
// v1 of this package shipped a deviation - six free functions here, called from
// TextureObject.cpp and RenderbufferObject.cpp - because at the contract TAG
// MG_Pipe/PipeMutation.h declared only the six DEATH helpers and this package may not edit
// A's files. c0b landed the BIRTH half, so the deviation is retired rather than carried:
// MG_State now calls MGPipeMintTextureHandle / MGPipeEmitTextureResourceCreate /
// ...ResourceRespecify / MGPipeEmitTextureParams / MGPipeNoteTextureLevelDirty and the two
// renderbuffer twins, all DECLARED in MG_Pipe/PipeMutation.h and DEFINED in
// MG_Impl/Pipe/PipeFill.cpp, which forwards to the entry points below through
// ForwardWhenWired<kMGPipeWiredTextureSubsystem>. No MG_State translation unit includes this
// header any more, which is the property check_include_closure.py's mutation-header probe
// exists to keep.
//
// WHAT THIS FILE OWES THAT SEAM, and a mismatch is a compile error in this package's own
// commit rather than a surprise at the merge (that is what the wired constant buys):
//
//   EmitResourceCreate(ITextureObject&)      EmitResourceRespecify(ITextureObject&)
//   EmitTextureParams(ITextureObject&)       NoteLevelDirty(ITextureObject&, Uint32, Uint32)
//   EmitRenderbufferCreate(RenderbufferObject&)
//   EmitRenderbufferRespecify(RenderbufferObject&)
//
// and the PUBLICATION LATCH is A's too: MGPipeNoteHandlePublished is called where a create
// actually goes out and MGPipeHandleIsPublished is what the death helpers read, so this file
// keeps no Published flag of its own.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SamplerEmit.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/RenderbufferState/RenderbufferObject.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <Config.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace MobileGL::MG_Pipe {

    // WHICH SUBSYSTEM BIT THIS BUILD ACTUALLY EMITS FOR. PipeFill.cpp ORs the four per-family
    // constants into kMGPipeWiredSubsystems, so the bit is added by the commit that gives the
    // emitters their bodies, with no file touched twice - and a Coverage.def row can never
    // silently drop a field on the floor before the call that carries it exists.
    //
    // IT WAS 0 IN v1, AND THE THING THAT BLOCKED IT IS NOW IN THE TREE. Unlike the other three
    // P4a families this one gets no fresh apply entry points - the catalogue is closed and a
    // texture rides P3a's OWN resource_create / resource_respecify / resource_subdata /
    // resource_destroy rows - so on a base without the wire package's per-kind record vectors
    // and its texture branch, two properties made a texture record actively harmful rather than
    // merely ignored: MGPipeApplierState::Resources was ONE slot-indexed vector, so a texture
    // create at slot 12 overwrote the BUFFER record at slot 12; and SubDataBoxFault validated
    // every record as the buffer half of MGPSubData, so a texture sub-data record was
    // Fatal{ProtocolCorruption} on `record.Level != 0` alone. Both are closed on this base
    // (three per-kind vectors, ApplyTextureUpload, SubDataTextureFault, and C1's level-scoped
    // PendingUploads clear), so the constant is its own bit and the family is live.
    //
    // THE FLIP IS THE WHOLE SWITCH AND NOTHING ELSE MOVES: PipeFill.cpp ORs this constant into
    // kMGPipeWiredSubsystems, static_asserts it is 0-or-its-own-bit, gates every birth hook on
    // FamilyIsLive(kMGPipeSubsystemTextureResources, this), and gates DrainTextureSubData on
    // the same OR. Bit 9 (framebuffer) REQUIRES bit 10 (D-K2), because every MGPSurface::Res
    // names a texture or renderbuffer handle the applier must hold a record for - so this
    // package may never be integrated with only one of the two constants set.
    inline constexpr Uint64 kMGPipeWiredTextureSubsystem = kMGPipeSubsystemTextureResources;
    static_assert(kMGPipeWiredTextureSubsystem == 0 ||
                      kMGPipeWiredTextureSubsystem == kMGPipeSubsystemTextureResources,
                  "a family's wired constant is 0 or its own bit and nothing else");

    // BOTH HALVES MATTER, exactly as MGPipeResourceSubsystemEnabled()'s two do, and they are
    // the SAME PAIR PipeFill.cpp's FamilyIsLive applies - the operator's per-subsystem A/B bit
    // in MOBILEGL_PIPE_PUSH, and this build having wired the family at all. There is no third
    // half: v1 carried a runtime `m_armed` latch so a unit case could drive the conversion on a
    // base whose applier could not hold the record, and with the constant flipped that latch
    // would only be able to LIE (PipeFill.cpp's gate does not consult it, so an unarmed emitter
    // would still be driven by every frontend mutation). It is deleted; a case that wants the
    // family off clears MG_Config::Features.PipePush, which is the switch the shipped build has.
    inline Bool MGPipeTextureSubsystemEnabled();

    // DO THE RECORDS THIS EMITTER BUILDS REACH THE APPLIER IN THIS BUILD? It is the wired
    // constant asked as a COMPILE-TIME predicate, and with the constant set it is simply true -
    // which is the point: every `if constexpr` below is taken, so the acceptance answers the
    // emitter gates its own bookkeeping on are real answers rather than a default.
    //
    // IT IS KEPT RATHER THAN INLINED because it is what makes a build with the constant back at
    // 0 - the A/B arm an operator gets by editing one line, and the arm a bisect lands on -
    // compile with the applier calls discarded instead of half-wired. Where the difference
    // matters is stated at each site: the dirty-flag clear may never run on a discarded call
    // (D-D5 step 1), and the descriptor mirror may not advance past a call that never landed.
    //
    // set_texture_params is deliberately NOT behind it. It is addressed by RESOURCE and is the
    // one call that closes D-E3's READ-attachment gap, and its BuiltinSampler comes from the
    // sampler family rather than this one - so it rides bit 11's wiring, not bit 10's.
    inline constexpr Bool MGPipeTextureRecordsReachTheApplier() {
        return (kMGPipeWiredTextureSubsystem & kMGPipeSubsystemTextureResources) != 0;
    }

    // ---------------------------------------------------------------------------------
    // D-A3 / D-D1: the two discriminators a TEXTURE descriptor carries
    // ---------------------------------------------------------------------------------

    // MGPResourceDesc::StorageKind == TextureStorageType, named rather than open-coded, and
    // derived from the TARGET rather than from ITextureObject::GetStorageType().
    //
    // THE TARGET IS THE ONLY LEGAL SOURCE AT THE ONE MOMENT THIS MATTERS: resource_create is
    // emitted from TextureObjectBase's constructor (D-D1), where the derived object does not
    // exist yet and GetStorageType() is a PURE virtual - calling it there is undefined
    // behaviour, not merely a wrong answer. The mapping is exact and total: TextureObjectBuffer
    // is the only class that reports Buffer and TextureTarget::TextureBuffer is the only target
    // it is ever constructed with, which MGPipeTextureStorageKindAgreesWithObject below
    // re-checks at the first respecify, where the object IS complete.
    inline constexpr Uint8 MGPipeTextureStorageKindForTarget(MobileGL::TextureTarget target) {
        return static_cast<Uint8>(target == MobileGL::TextureTarget::TextureBuffer
                                      ? MobileGL::TextureStorageType::Buffer
                                      : MobileGL::TextureStorageType::Mipmap);
    }

    // MGPSubData::Target's PACKING AND THE TWO DEPTH-STENCIL NUMBERS ARE THE CONTRACT'S NOW
    // (ID-12 DV-2/DV-3, c0c): MGPipePackSubDataTarget / MGPipeSubDataResourceTargetOf /
    // MGPipeSubDataUploadTargetOf and kMGPipeDepthStencilModeDepth/Stencil live in
    // MG_Pipe/MGPipeTypes.h under exactly these names, with Uint32 arguments so the header
    // stays backend-neutral. This package's copies were the same spelling in the same
    // namespace - a redefinition - and are deleted; the packing argument they carried is
    // stated where the definitions now are.
    //
    // WHAT STAYS HERE is the GLenum -> byte translation, which is frontend knowledge: the
    // frontend keeps GL_DEPTH_COMPONENT / GL_STENCIL_INDEX (0x1902 / 0x1901) and the payload
    // byte cannot hold one.
    inline Uint8 MGPipeDepthStencilModeByte(GLenum mode) {
        return mode == GL_STENCIL_INDEX ? kMGPipeDepthStencilModeStencil : kMGPipeDepthStencilModeDepth;
    }

    // ---------------------------------------------------------------------------------
    // D-D1: the payload builders. Pure, so a unit case can assert field by field (G6), and
    // one EXPECT per field is what G7's scripted control needs - it stops the conversion
    // copying ONE member and expects the suite to go red NAMING it.
    // ---------------------------------------------------------------------------------

    // The extent trio and the layer count, which are one question the frontend answers in
    // three different axes depending on the target: a 1D array carries its layer count in the
    // state-side HEIGHT, every other layered target carries it in z, and a cube map keeps six
    // faces in six blobs and therefore reports none at all.
    struct MGPipeTextureExtent {
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Depth = 0;
        Uint16 ArrayLayers = 1;
    };

    inline MGPipeTextureExtent MGPipeTextureExtentOf(const MG_State::GLState::ITextureObject& texture) {
        MGPipeTextureExtent extent{};
        const IntVec3 base = texture.GetBaseSize();
        extent.Width = base.x() > 0 ? static_cast<Uint32>(base.x()) : 0;
        extent.Height = base.y() > 0 ? static_cast<Uint32>(base.y()) : 0;
        extent.Depth = base.z() > 0 ? static_cast<Uint32>(base.z()) : 0;
        switch (texture.GetTarget()) {
        case MobileGL::TextureTarget::Texture1DArray:
            extent.ArrayLayers = static_cast<Uint16>(std::max<Int>(base.y(), 1));
            break;
        case MobileGL::TextureTarget::Texture2DArray:
        case MobileGL::TextureTarget::TextureCubeMapArray:
        case MobileGL::TextureTarget::Texture2DMultisampleArray:
            extent.ArrayLayers = static_cast<Uint16>(std::max<Int>(base.z(), 1));
            break;
        case MobileGL::TextureTarget::TextureCubeMap:
            // Six blobs rather than six layers on this side of the boundary; the face rides in
            // the sub-data record's upload-target byte and in MGPSurface::UploadTarget.
            extent.ArrayLayers = 6;
            break;
        default:
            extent.ArrayLayers = 1;
            break;
        }
        return extent;
    }

    // The descriptor for a TEXTURE. `storageDefined` is false for the create the constructor
    // emits - storage is defined lazily by the first respecify and a backend tolerates a
    // resource that has none - and true for every respecify.
    //
    // `viewOf`, `bufferForTexBuffer` and the buffer window are handed in rather than resolved
    // here, because resolving them needs the slot allocator and this function must stay pure.
    inline MGPResourceDesc MGPipeBuildTextureResourceDesc(const MG_State::GLState::ITextureObject& texture,
                                                          MGPipeHandle handle, Uint16 bindMask,
                                                          Bool storageDefined, MGPipeHandle viewOf,
                                                          MGPipeHandle bufferForTexBuffer, Uint64 bufOffset,
                                                          Uint64 bufSize) {
        MGPResourceDesc desc{};
        desc.Resource = handle;
        desc.Target = static_cast<Uint8>(MGPipeResourceTargetForTextureTarget(texture.GetTarget()));
        desc.StorageKind = MGPipeTextureStorageKindForTarget(texture.GetTarget());
        desc.BindMask = bindMask;
        // STICKY AND FOREVER: everImageBound, the client-side answer MGPipeTypes.h asks for.
        // It is the PREVENTION half of the texture-remint stall class - a texture the server
        // knows may be image-bound is allocated image-bindable up front, so the re-mint that
        // would have to pull its texels back never happens.
        desc.ImageBindableHint = (bindMask & kMGPipeBindShaderImage) != 0 ? 1 : 0;
        if (storageDefined) {
            const MGPipeTextureExtent extent = MGPipeTextureExtentOf(texture);
            desc.InternalFormat = static_cast<Uint32>(texture.GetFormat());
            desc.Width = extent.Width;
            desc.Height = extent.Height;
            desc.Depth = extent.Depth;
            desc.ArrayLayers = extent.ArrayLayers;
            const auto* mipmap = MG_State::GLState::AsMipmapTexture(&texture);
            desc.Levels = mipmap != nullptr ? static_cast<Uint16>(mipmap->GetMipmapLevelCount()) : 0;
            desc.Samples = static_cast<Uint16>(std::max<Int>(texture.GetSamples(), 0));
            desc.FixedSampleLocations = texture.HasFixedSampleLocations() ? 1 : 0;
            // A real descriptor fact the backend reads (glTexStorage* / a texture view), and
            // it must NOT be read as "this respecify wants an acknowledgement":
            // MGPipeResourceRespecifyNeedsAck is narrowed to name the buffer target, because
            // texture allocation is lazy in monolith and stays lazy in split.
            desc.Immutable = texture.IsImmutable() ? 1 : 0;
            // "Storage exists and is not undefined". A mipmap texture answers with its level
            // count, a buffer texture with whether a buffer is attached; both are what the
            // backend's ensure path already tests before it uploads anything.
            desc.HasDefinedContent =
                (mipmap != nullptr ? mipmap->GetMipmapLevelCount() > 0 : !MGPipeHandleIsNull(bufferForTexBuffer))
                    ? 1
                    : 0;
        }
        desc.ViewOf = viewOf;
        desc.BufferForTexBuffer = bufferForTexBuffer;
        desc.BufOffset = bufOffset;
        desc.BufSize = bufSize;
        // Diagnostics only: a GL name is never an identity, never a memo key and never part of
        // a content hash (ARCHITECTURE.md 4.2.1).
        desc.GlNameForDiag = static_cast<Uint32>(texture.GetExternalIndex());
        return desc;
    }

    // The descriptor for a RENDERBUFFER. A renderbuffer is an independent class on the wire
    // (ARCHITECTURE.md 4.5.1) and shares nothing but the shape: no levels, no layers, no view,
    // no buffer window, and a storage definition that is always the whole object.
    inline MGPResourceDesc MGPipeBuildRenderbufferResourceDesc(
        const MG_State::GLState::RenderbufferObject& renderbuffer, MGPipeHandle handle, Uint16 bindMask,
        Bool storageDefined) {
        MGPResourceDesc desc{};
        desc.Resource = handle;
        desc.Target = static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer);
        // A renderbuffer is not a texture and has no TextureStorageType of its own; Mipmap is
        // the non-buffer answer and is what keeps the one discriminator that matters - "is this
        // a BUFFER store" - false for it.
        desc.StorageKind = static_cast<Uint8>(MobileGL::TextureStorageType::Mipmap);
        desc.BindMask = bindMask;
        desc.ImageBindableHint = 0;
        if (storageDefined) {
            desc.InternalFormat = static_cast<Uint32>(renderbuffer.GetInternalFormat());
            desc.Width = renderbuffer.GetWidth() > 0 ? static_cast<Uint32>(renderbuffer.GetWidth()) : 0;
            desc.Height = renderbuffer.GetHeight() > 0 ? static_cast<Uint32>(renderbuffer.GetHeight()) : 0;
            desc.Depth = 1;
            desc.ArrayLayers = 1;
            desc.Levels = 1;
            desc.Samples = static_cast<Uint16>(std::max<Int>(renderbuffer.GetSamples(), 0));
            desc.FixedSampleLocations = 1;
            desc.HasDefinedContent = renderbuffer.IsAllocated() ? 1 : 0;
        }
        desc.GlNameForDiag = static_cast<Uint32>(renderbuffer.GetExternalIndex());
        return desc;
    }

    // set_texture_params. Per texture OBJECT, independent of any view and of any binding -
    // which is exactly what closes the gap D10 names: a texture that is only an FBO
    // attachment, only an image-unit binding or only a glCopyImageSubData endpoint has a
    // record the moment its parameters move, and the server applies it wherever it meets it.
    //
    // `builtinSampler` is handed in for MGPipeBuildTextureResourceDesc's reason (resolving it
    // needs the allocator). It may NEVER be the null handle: every ITextureObject owns a
    // SamplerObject, so a null there is a protocol corruption rather than "no sampler".
    inline MGPTextureParams MGPipeBuildTextureParams(const MG_State::GLState::ITextureObject& texture,
                                                     MGPipeHandle handle, MGPipeHandle builtinSampler,
                                                     Bool forceResync) {
        MGPTextureParams params{};
        params.Res = handle;
        params.BuiltinSampler = builtinSampler;
        const UintVec2& levelRange = texture.GetLevelRange();
        params.BaseLevel = static_cast<Uint16>(std::min<Uint>(levelRange.x(), 0xFFFFu));
        params.MaxLevel = static_cast<Uint16>(std::min<Uint>(levelRange.y(), 0xFFFFu));
        const Vec4<MobileGL::TextureSwizzleParam>& swizzle = texture.GetAllSwizzleParams();
        params.Swizzle[0] = static_cast<Uint8>(swizzle.r());
        params.Swizzle[1] = static_cast<Uint8>(swizzle.g());
        params.Swizzle[2] = static_cast<Uint8>(swizzle.b());
        params.Swizzle[3] = static_cast<Uint8>(swizzle.a());
        params.DepthStencilMode = MGPipeDepthStencilModeByte(texture.GetDepthStencilTextureMode());
        // BOTH RESYNC BYTES ARE THE SERVER'S TO SET AND THE CLIENT'S ONLY TO REQUEST, and the
        // client has exactly one such request: the widened-channel swizzle override after an
        // ImageBindableHint transition, which the frontend params version does not move for.
        // The client never clears a server flag and the server never clears the client's.
        params.ForceResync = forceResync ? 1 : 0;
        params.SamplerResync = 0;
        const auto& sampler = texture.GetSamplerObject();
        if (sampler) {
            params.MinLod = sampler->GetMinLod();
            params.MaxLod = sampler->GetMaxLod();
            params.LodBias = sampler->GetLodBias();
        }
        return params;
    }

    // ---------------------------------------------------------------------------------
    // D-D3: the sub-data shape. The union box AND the region list, so the SERVER picks the
    // upload shape - the decision belongs on the side that pays the GPU cost, and Mali prices
    // texture upload by JOB COUNT (~100 sprite rects against one union box = +6 ms/frame).
    // ---------------------------------------------------------------------------------

    // The level's own pitches, in bytes, derived the way the frontend already sizes a level:
    // the stored byte size divided by the texel count. A zero-texel level answers zero, which
    // is what makes an unallocated level emit nothing rather than divide by zero.
    struct MGPipeLevelPitch {
        Uint32 BytesPerTexel = 0;
        Uint32 RowStride = 0;
        Uint32 SliceStride = 0;
    };

    inline MGPipeLevelPitch MGPipeLevelPitchOf(const IntVec3& levelSize, SizeT levelBytes) {
        MGPipeLevelPitch pitch{};
        const Int64 width = std::max<Int>(levelSize.x(), 0);
        const Int64 height = std::max<Int>(levelSize.y(), 0);
        const Int64 depth = std::max<Int>(levelSize.z(), 1);
        const Int64 texels = width * height * depth;
        if (texels <= 0 || levelBytes == 0) return pitch;
        pitch.BytesPerTexel = static_cast<Uint32>(static_cast<Int64>(levelBytes) / texels);
        pitch.RowStride = static_cast<Uint32>(pitch.BytesPerTexel * width);
        pitch.SliceStride = static_cast<Uint32>(static_cast<Int64>(pitch.RowStride) * height);
        return pitch;
    }

    inline MGPBox MGPipeBoxOfDirtyRegion(const MG_State::GLState::MipmapDirtyRegion& region) {
        MGPBox box{};
        box.X = region.lo.x();
        box.Y = region.lo.y();
        box.Z = region.lo.z();
        box.W = static_cast<Uint32>(std::max<Int>(region.hi.x() - region.lo.x(), 0));
        box.H = static_cast<Uint32>(std::max<Int>(region.hi.y() - region.lo.y(), 0));
        box.D = static_cast<Uint32>(std::max<Int>(region.hi.z() - region.lo.z(), 0));
        return box;
    }

    // ONE sub-region, with its strides CARRIED and never inferred (ARCHITECTURE.md 4.5.6): the
    // old `uploadData == mipData` pointer comparison cannot survive a split, where the client
    // neither ships the whole level nor keeps a server-side mirror of it.
    //
    // A WHOLE-LEVEL REGION LEAVES BOTH STRIDES 0 and that is not an omission: 0 means "tightly
    // packed", the level shadow IS tightly packed, and the staging planner on the far side
    // reads a 0 as `w * bpp`. A sub-rect's rows are not contiguous in the shadow, so it must
    // carry the LEVEL's pitches - not its own width - or the server would repack the wrong
    // bytes.
    inline MGPSubRegion MGPipeBuildSubRegion(const MGPBox& box, const IntVec3& levelSize,
                                             const MGPipeLevelPitch& pitch) {
        MGPSubRegion region{};
        region.X = box.X;
        region.Y = box.Y;
        region.Z = box.Z;
        region.W = box.W;
        region.H = box.H;
        region.D = box.D;
        const Bool wholeLevel = box.X == 0 && box.Y == 0 && box.Z == 0 &&
                                box.W >= static_cast<Uint32>(std::max<Int>(levelSize.x(), 0)) &&
                                box.H >= static_cast<Uint32>(std::max<Int>(levelSize.y(), 0)) &&
                                box.D >= static_cast<Uint32>(std::max<Int>(levelSize.z(), 1));
        region.SrcOffset = static_cast<Uint64>(box.Z) * pitch.SliceStride +
                           static_cast<Uint64>(box.Y) * pitch.RowStride +
                           static_cast<Uint64>(box.X) * pitch.BytesPerTexel;
        region.SrcRowStride = wholeLevel ? 0u : pitch.RowStride;
        region.SrcSliceStride = wholeLevel ? 0u : pitch.SliceStride;
        return region;
    }

    // ---------------------------------------------------------------------------------
    // The emitter: handles, the inverse, the sticky mask, the drain list
    // ---------------------------------------------------------------------------------

    class MGPipeTextureEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;
        using ITextureObject = MG_State::GLState::ITextureObject;
        using TextureObjectMipmap = MG_State::GLState::TextureObjectMipmap;
        using RenderbufferObject = MG_State::GLState::RenderbufferObject;

        // ---- handles ----
        //
        // Minting is NOT gated on the subsystem bit, for MGPipeMintResourceHandle's reason:
        // a handle is CLIENT state and set_framebuffer_state / set_sampler_views name a
        // texture by handle whether or not the texture-resource family is switched on, so
        // gating the mint would make the other subsystems emit null handles in exactly the
        // A/B arm that exists to isolate them. Only the CALLS are gated.
        MGPipeHandle AcquireTexture(Uint64 lifetimeId, ITextureObject* object) {
            const MGPipeHandle handle = MGPipeSlots().Acquire(MGPipeKind::Texture, lifetimeId);
            Entry& entry = EntryFor(m_textures, handle);
            RetireIfRecycled(entry, handle);
            entry.Texture = object;
            entry.Gen = handle.Gen;
            return handle;
        }
        MGPipeHandle FindTexture(const ITextureObject& texture) const {
            return MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, texture.GetLifetimeId());
        }
        MGPipeHandle AcquireRenderbuffer(Uint64 lifetimeId) {
            const MGPipeHandle handle = MGPipeSlots().Acquire(MGPipeKind::Renderbuffer, lifetimeId);
            Entry& entry = EntryFor(m_renderbuffers, handle);
            RetireIfRecycled(entry, handle);
            entry.Gen = handle.Gen;
            return handle;
        }
        MGPipeHandle FindRenderbuffer(const RenderbufferObject& renderbuffer) const {
            return MGPipeSlots().FindByLifetimeId(MGPipeKind::Renderbuffer, renderbuffer.GetLifetimeId());
        }

        // The texture a handle names, or null. A RAW pointer is exact here for
        // MGPipeResourceTracker::Resolve's reason: the entry exists only between the create the
        // constructor emits and the destroy the destructor emits - and since the final review's
        // C-2 that sentence is ESTABLISHED rather than assumed: the contract's death helper
        // forwards to NoteTextureDied below before it frees the slot, so a dead handle finds a
        // null pointer here. The Gen compare refuses a RECYCLED handle rather than resolving it
        // to whatever now occupies the slot.
        //
        // A DEAD SLOT IS REFUSED, LOUDLY. The allocator's generation moves only at the NEXT
        // hand-out, so between a death and a recycle a dead handle compares equal to the slot's
        // generation - which is why the guard is IsLive and not GenOfSlot (the review's C-2:
        // that compare guarded a recycled slot and never a dead one, and the drain then called
        // a virtual on the freed object once per verb). Reaching this arm at all means a death
        // path skipped the emitter, which is a seam defect and not traffic: it is counted and
        // logged once, and the answer is null.
        ITextureObject* ResolveTexture(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_textures.size()) return nullptr;
            const Entry& entry = m_textures[slot];
            if (entry.Texture == nullptr || entry.Gen != handle.Gen) return nullptr;
            if (!MGPipeSlots().IsLive(MGPipeKind::Texture, handle)) {
                ++m_deadResolves;
                MGLOG_E_ONCE("MGPipe: texture handle {slot=%u, gen=%u} is dead but the emitter still holds its "
                             "object - the death path did not retire the entry; refused rather than resolved",
                             handle.Slot, handle.Gen);
                return nullptr;
            }
            return entry.Texture;
        }

        // ---- the death half (P4a final review C-2) ----
        //
        // CALLED BY THE CONTRACT'S DEATH HELPER, after the wire delete went out and BEFORE the
        // slot is freed (ID-8's order: delete, notice, free - this sits between the first two).
        // v2 had no such door: the helper freed the slot, the emitter kept the freed
        // ITextureObject* and the level on the drain list, and `glTexImage2D; glDeleteTextures;
        // <any verb>` walked freed memory at the next validate point - a SIGABRT ("pure virtual
        // method called") at the shipping mask. Everything the entry owns goes here: the drain
        // entries (nothing is owed for a dead texture - its record is gone with the wire delete),
        // the built-in sampler's cache reference (ID-17: one per entry, released at the death
        // and no longer at the recycle), the latches and the sticky mask. RetireIfRecycled stays
        // as the belt for a slot whose death this emitter was never told about.
        //
        // Keyed on the GENERATION so a late notice for a slot that has already been handed out
        // again cannot retire the successor's entry.
        void NoteTextureDied(MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_textures.size()) return;
            Entry& entry = m_textures[slot];
            if (entry.Gen != handle.Gen) return;
            if (!entry.DrainKeys.empty()) {
                // A death inside the drain cannot happen (no SharedPtr drops there), but if one
                // ever did the loop below is iterating m_drain: the null pointer the reset
                // leaves is what makes EmitOneLevel answer "nothing owed" and drop the entry.
                if (!m_draining) {
                    SizeT kept = 0;
                    for (SizeT i = 0; i < m_drain.size(); ++i) {
                        if (m_drain[i].Handle == handle) continue;
                        m_drain[kept++] = m_drain[i];
                    }
                    m_drain.resize(kept);
                }
                entry.DrainKeys.clear();
            }
            MGPipeSamplerCsoCacheInstance().Release(entry.BuiltinSampler);
            entry = Entry{};
        }
        void NoteRenderbufferDied(MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_renderbuffers.size()) return;
            Entry& entry = m_renderbuffers[slot];
            if (entry.Gen != handle.Gen) return;
            entry = Entry{};
        }

        // ---- the sticky bind mask (D-A4) ----
        //
        // ORed, never cleared, and emitted on BOTH resource_create and every
        // resource_respecify, exactly as P3a's buffer mask is. The four bits nothing set before
        // P4a get their producers here and in the framebuffer emitter: RENDER_TARGET and
        // DEPTH_STENCIL from an attachment point (FramebufferEmit.h), SAMPLER from a resolved
        // sampler view (SamplerEmit.h) and SHADER_IMAGE from glBindImageTexture's state setter
        // and the resolved image unit (TextureState.h, ImageEmit.h) - the last two through the
        // contract's MGPipeNoteTextureBoundAs door, since neither may include this header
        // (final review M-A: before the fix round nothing produced them and the hint was dead).
        // A MASK CHANGE AFTER THE ALLOCATION IS A METADATA RESPECIFY (ID-18 M4), and without it
        // the sticky half of D-A4 is a no-op for exactly the textures it was written for. The
        // mask rides resource_create and every resource_respecify - and an IMMUTABLE texture has
        // no further respecify, that being what immutable means - so for the canonical order
        // `glTexStorage2D(...); glBindImageTexture(...)` the applier's record kept
        // ImageBindableHint = 0 for ever and the PREVENTION half of the texture-remint stall
        // class never fired. So a mask that actually MOVES re-emits the stored descriptor with
        // the new mask: every storage-defining field is byte-identical to what the applier
        // holds, which is exactly the shape wire applies as a METADATA UPDATE - the descriptor
        // is replaced, no reallocation is acked, and NO pending upload is dropped, so a mask
        // change arriving between a glTexSubImage2D and the sync that consumes it cannot eat
        // the texels.
        void NoteTextureBoundAs(MGPipeHandle handle, Uint16 bit) {
            if (MGPipeHandleIsNull(handle)) return;
            Entry& entry = EntryFor(m_textures, handle);
            // The entry is stamped with the generation it is written under, and a predecessor's
            // entry on a recycled slot is retired first (the same door AcquireTexture takes): a
            // texture born while the family bit was clear has no create to have done it.
            RetireIfRecycled(entry, handle);
            entry.Gen = handle.Gen;
            const Uint16 before = entry.BindMask;
            const Uint16 now = static_cast<Uint16>(before | bit);
            if (now == before) return;
            entry.BindMask = now;
            // AN ImageBindableHint TRANSITION IS THE ONE THING THE CLIENT ASKS A RESYNC FOR
            // (D-E2): the widened-channel carrier needs a swizzle override that the frontend's
            // own params version does not move for, so the transition arms ForceResync on the
            // next set_texture_params rather than being silently folded into the descriptor.
            // SamplerResync stays the SERVER's byte and is never set from here.
            if ((before & kMGPipeBindShaderImage) == 0 && (bit & kMGPipeBindShaderImage) != 0) {
                entry.ForceParamsResync = true;
            }
            RepublishMask(MGPipeKind::Texture, handle, entry);
        }
        void NoteRenderbufferBoundAs(MGPipeHandle handle, Uint16 bit) {
            if (MGPipeHandleIsNull(handle)) return;
            Entry& entry = EntryFor(m_renderbuffers, handle);
            RetireIfRecycled(entry, handle);
            entry.Gen = handle.Gen;
            const Uint16 before = entry.BindMask;
            const Uint16 now = static_cast<Uint16>(before | bit);
            if (now == before) return;
            entry.BindMask = now;
            RepublishMask(MGPipeKind::Renderbuffer, handle, entry);
        }
        Uint16 TextureBindMask(MGPipeHandle handle) const { return MaskOf(m_textures, handle); }
        Uint16 RenderbufferBindMask(MGPipeHandle handle) const { return MaskOf(m_renderbuffers, handle); }

        // ---- the three object calls (the entry points PipeFill.cpp forwards to) ----

        // resource_create, from TextureObjectBase's CONSTRUCTOR - so the DERIVED object does
        // not exist yet and only members TextureObjectBase itself implements may be read.
        // GetTarget(), GetExternalIndex() and GetLifetimeId() are all overridden ON THE BASE,
        // so they dispatch to the base's own bodies here and read members the mem-init list has
        // already written; GetStorageType() and GetUploadTargets() are NOT, so calling either
        // would be undefined behaviour and the storage kind is derived from the target instead
        // (exact, and re-checked at the first respecify where the object IS complete).
        void EmitResourceCreate(ITextureObject& texture) {
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            Entry& entry = EntryFor(m_textures, handle);
            MGPResourceDesc desc{};
            desc.Resource = handle;
            desc.Target = static_cast<Uint8>(MGPipeResourceTargetForTextureTarget(texture.GetTarget()));
            desc.StorageKind = MGPipeTextureStorageKindForTarget(texture.GetTarget());
            desc.BindMask = entry.BindMask;
            desc.ImageBindableHint = (entry.BindMask & kMGPipeBindShaderImage) != 0 ? 1 : 0;
            desc.GlNameForDiag = static_cast<Uint32>(texture.GetExternalIndex());
            NoteDesc(desc, /*isCreate=*/true);
            PublishCreate(MGPipeKind::Texture, handle, entry, desc);
        }

        // resource_respecify, from every storage-defining entry point, WITH THE SCOPE OF THE
        // STORAGE IT REPLACES (P4a final review C-1; the scopes are PipeMutation.h's).
        //
        // THE LEVEL IS PASSED, AND IT IS WIRE'S KEY. The applier keeps a pending-upload set per
        // (uploadTarget, level) - the client's dirty flags, inverted - and a respecify drops the
        // entries against the storage it REPLACES: with a null MGPRespecifiedLevel every entry,
        // with a level exactly that one. v2 passed null at every call, so a level the applier
        // had ACCEPTED at one verb (the client flag already clear, D-D5 step 1) and that the
        // next verb's glTexImage2D(level 1) or glGenerateMipmap grow defined AROUND was dropped
        // with nobody owing its texels: `L0; draw(other); L1; draw(T)` read a black level 0.
        // The key is built from the SAME packed MGPSubData::Target the drain puts in that
        // level's record (wire-v3 §5 item 5), so what this drops is what that emission made.
        //
        // THREE SCOPES, one call each for the first two and one call PER REMOVED LEVEL for the
        // chain cut: the applier's key is one (uploadTarget, level), so "every level from N"
        // is spelled as N.., each after the first landing on an unchanged descriptor - which
        // the applier classifies as a metadata update that drops nothing but the level it
        // names. That is the refinement wire's W11 clause takes this round.
        //
        // DEDUPED ON THE DESCRIPTOR ITSELF for the whole-resource form only: the entry points
        // that reach it move the SHAPE and several of them do not move the descriptor at all
        // (glTexParameter TEXTURE_BASE_LEVEL bumps the shape version and changes no field this
        // record carries), and a byte compare of an 88-byte POD is cheaper than the emission
        // it avoids. A PER-LEVEL form is never deduped: the level it redefines is not in the
        // descriptor (a non-base level's extent moves no field), so an unchanged descriptor
        // cannot say whether the applier still holds a box against the OLD level - and a box
        // kept across a shrink is uploaded past the end of the new one. One applier call per
        // level definition is the cost, and the sub-data that follows moves the serial anyway.
        void EmitResourceRespecify(ITextureObject& texture, MGPipeTextureRespecifyScope scope,
                                   Uint32 uploadTarget, Uint32 level) {
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            // THE VIEW'S OWNER IS ACQUIRED FIRST, and no Entry& is held across it (m3): the
            // owner's slot can be higher than this table's size, so AcquireTexture would
            // resize() the vector out from under a reference taken before it. Every Entry&
            // below is taken after the last call that can grow the table.
            MGPipeHandle viewOf = kMGPipeNullHandle;
            if (const auto& owner = texture.GetViewStorageOwner()) {
                // ONE HOP ALWAYS REACHES STORAGE: glTextureView composes a view-of-a-view onto
                // the ROOT at creation, which is what the spec's additive min-level rule means.
                viewOf = AcquireTexture(owner->GetLifetimeId(), owner.get());
            }
            MGPipeHandle bufferHandle = kMGPipeNullHandle;
            Uint64 bufOffset = 0;
            Uint64 bufSize = 0;
            if (texture.GetStorageType() == MobileGL::TextureStorageType::Buffer) {
                auto& bufferTexture = static_cast<MG_State::GLState::TextureObjectBuffer&>(texture);
                const auto& backing = bufferTexture.GetBufferBindingSlot().GetBoundObject();
                if (backing) {
                    // Bit 10 REQUIRES bit 7 for exactly this: only the resource subsystem puts
                    // a twin behind a Buffer handle, and a buffer texture's descriptor names
                    // one. The handle itself is minted whatever the bits say, because a mint is
                    // client state.
                    bufferHandle = MGPipeSlots().Acquire(MGPipeKind::Buffer, backing->GetLifetimeId());
                    bufOffset = static_cast<Uint64>(bufferTexture.GetBufferRangeOffset());
                    // RESOLVED LIVE, which is what ARCHITECTURE.md 4.5.1 asks for: glTexBuffer
                    // attaches the whole buffer and a stored size would freeze the texture at
                    // whatever size the buffer happened to have.
                    bufSize = bufferTexture.GetBufferRangeOffset() == 0 &&
                                      bufferTexture.GetBufferRangeSizeInBytes() == backing->GetSize()
                                  ? kMGPipeWholeBuffer
                                  : static_cast<Uint64>(bufferTexture.GetBufferRangeSizeInBytes());
                }
            }
            Entry& entry = EntryFor(m_textures, handle);
            const MGPResourceDesc desc = MGPipeBuildTextureResourceDesc(
                texture, handle, entry.BindMask, /*storageDefined=*/true, viewOf, bufferHandle, bufOffset,
                bufSize);
            const Bool unchanged = entry.HasLastDesc && std::memcmp(&entry.LastDesc, &desc, sizeof(desc)) == 0;

            // THE KEYS THIS CALL DROPS. `keyCount == 0` is the whole resource (a null level
            // pointer); otherwise `keyCount` keys from `firstLevel` up, all on `uploadTarget`.
            Uint32 firstLevel = 0;
            Uint32 keyCount = 0;
            switch (scope) {
            case MGPipeTextureRespecifyScope::OneLevel:
                firstLevel = level;
                keyCount = 1;
                break;
            case MGPipeTextureRespecifyScope::LevelsFrom: {
                // A cut at 0 leaves nothing: the whole resource. Otherwise the removed levels
                // are [level, the level count the applier last accepted): LastDesc mirrors
                // acceptance, and a sub-data for a level the accepted descriptor does not
                // describe is refused by the applier, so no key above that count can exist. A
                // cut that removes nothing the applier could hold is deduped like the
                // whole-resource form; if the descriptor moved anyway the first key carries it.
                if (level == 0) break;
                const Uint32 previous = entry.HasLastDesc ? static_cast<Uint32>(entry.LastDesc.Levels) : 0u;
                if (previous <= level && unchanged) return;
                firstLevel = level;
                keyCount = previous > level ? previous - level : 1u;
                break;
            }
            case MGPipeTextureRespecifyScope::WholeResource:
            default:
                if (unchanged) return;
                break;
            }

            // SELF-HEALING IN BOTH DIRECTIONS, the P3a m12 shape: a texture born while the
            // subsystem bit was clear has no applier record, and every later respecify would be
            // REFUSED. A create rather than a respecify, because that is what the record's
            // absence means and because the applier starts a record over on a create.
            if (!MGPipeHandleIsPublished(MGPipeKind::Texture, handle)) {
                const MGPResourceDesc createDesc = MGPipeBuildTextureResourceDesc(
                    texture, handle, entry.BindMask, /*storageDefined=*/false, viewOf, bufferHandle,
                    bufOffset, bufSize);
                NoteDesc(createDesc, /*isCreate=*/true);
                PublishCreate(MGPipeKind::Texture, handle, entry, createDesc);
            }
            NoteDesc(desc, /*isCreate=*/false);
            // NO initial bytes: a texture's texels travel as resource_subdata out of the drain
            // list, never inside its storage definition. This is what keeps glTexImage2D's
            // "define the level and upload it" one allocation and one upload rather than two.
            //
            // AND THE MIRROR ONLY ADVANCES ON ACCEPTANCE (ID-18 M3): the dedupe above is a claim
            // about what the APPLIER holds, so a refused respecify must leave LastDesc naming
            // the descriptor that actually landed, or the next identical call is suppressed
            // against a record that was never stored.
            //
            // THE PACKED TARGET IS THE DRAIN's (wire-v3 §5 item 5): the contract's packer takes
            // two Uint32s, low byte the resource target, high byte the upload target (a cube
            // face), and the applier matches the key against the sub-data records verbatim.
            const Uint16 packedTarget = MGPipePackSubDataTarget(
                static_cast<Uint32>(MGPipeResourceTargetForTextureTarget(texture.GetTarget())), uploadTarget);
            Bool accepted = false;
            if (keyCount == 0) {
                accepted = RespecifyOnce(texture, handle, entry, desc, nullptr, viewOf, bufferHandle, bufOffset,
                                         bufSize);
            } else {
                for (Uint32 i = 0; i < keyCount; ++i) {
                    MGPRespecifiedLevel key{};
                    key.UploadTarget = packedTarget;
                    key.Level = static_cast<Uint16>(firstLevel + i);
                    accepted = RespecifyOnce(texture, handle, entry, desc, &key, viewOf, bufferHandle, bufOffset,
                                             bufSize);
                    if (!accepted) break;
                }
            }
            NoteRespecified(entry, desc, accepted);
        }

        void EmitTextureParams(ITextureObject& texture) {
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            Entry& entry = EntryFor(m_textures, handle);
            const auto& sampler = texture.GetSamplerObject();
            if (!sampler) {
                // Structurally impossible - TextureObjectBase's constructor makes one - but a
                // null BuiltinSampler is Fatal{ProtocolCorruption} on the far side, so the
                // record is not sent rather than sent wrong.
                MGLOG_E_ONCE("MGPipe: texture %u has no sampler object; set_texture_params is dropped "
                             "rather than emitted with a null BuiltinSampler",
                             texture.GetExternalIndex());
                return;
            }
            // THE VERSION-FIRST SKIP, AND IT READS BOTH COUNTERS (clientsp-v2 rule 4, and it is
            // the M2 defect stated as a rule): glTexParameter* moves GetTextureParamsVersion()
            // AND lands on the built-in SamplerObject, but the three fields this record takes
            // off that object - MinLod, MaxLod, LodBias - are ALSO reachable through paths that
            // move only SamplerObject::GetVersion(). Latching on the texture's counter alone is
            // what let glTexParameterf(GL_TEXTURE_MIN_LOD) go stale. ForceParamsResync is the
            // third input because an ImageBindableHint transition moves neither counter.
            const Uint16 paramsVersion = texture.GetTextureParamsVersion();
            const Uint16 samplerVersion = sampler->GetVersion();
            if (entry.HasParamsLatch && entry.ParamsVersion == paramsVersion &&
                entry.SamplerVersion == samplerVersion && !entry.ForceParamsResync) {
                return;
            }
            // THE LATCH IS TAKEN BELOW, ON ACCEPTANCE (final review m-1, audit F-7) - like the
            // sub-data and respecify paths, and unlike v2, which advanced it here and left a
            // refused record (no applier record for the handle, the SD-1/SD-3 shape) unsent
            // until the next glTexParameter* moved a version.

            // ID-14 / ID-17: THE BUILT-IN SAMPLER COMES FROM C's CONTENT-ADDRESSED CACHE and is
            // never minted here. v1 took MGPipeSlots().Acquire(SamplerCso, the SamplerObject's
            // lifetime id), which is a slot no create_sampler_state ever names - so on the
            // integrated tree every texture's params record would have carried a handle the
            // applier holds nothing for. The cache mints and emits create_sampler_state on a
            // miss, so the texture's built-in sampler and a glBindSampler'd object with the
            // same value share ONE CSO and one server-side twin.
            //
            // EVERY Acquire TAKES A REFERENCE AND THIS ENTRY OWES EXACTLY ONE. The reference is
            // what stops the LRU pulling a handle out from under a standing MGPTextureParams
            // record: the applier deliberately does not resolve BuiltinSampler, and an eviction
            // is not a parameter change, so nothing would refuse and nothing would re-emit. The
            // previous handle is released when the content moves it, and the last one at the
            // texture's death (NoteTextureDied, reached from the contract's death helper) - or
            // at the recycle, as the belt, for a death this emitter was not told about.
            MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
            Uint64 samplerBytes = 0;
            const MGPipeHandle builtinSampler =
                cache.Acquire(sampler->GetAllSamplerParameters(), samplerBytes);
            m_samplerCsoPayloadBytes += samplerBytes;
            if (entry.BuiltinSampler == builtinSampler) {
                // The value did not move, so the cache handed back the handle this entry
                // already pins AND a second reference for it. Give that one straight back.
                cache.Release(builtinSampler);
            } else {
                cache.Release(entry.BuiltinSampler); // a no-op for the null handle
                entry.BuiltinSampler = builtinSampler;
            }

            const MGPTextureParams params =
                MGPipeBuildTextureParams(texture, handle, entry.BuiltinSampler, entry.ForceParamsResync);
            m_lastParams = params;
            ++m_paramSets;
            // Not behind MGPipeTextureRecordsReachTheApplier() (see its comment): the call is
            // dispatched whenever this emitter runs, so the answer is always a real one.
            Bool accepted = MGPipeApplySetTextureParams(params);
            if (!accepted) {
                // THE SELF-HEAL, the respecify path's shape, and the parameters are the one
                // publication that may be a texture's FIRST: the context's default textures are
                // constructed before the backend registers its consumer, so no create ever went
                // out for them, and the application's first glTexParameter* on texture 0 found
                // no record (the retrace census's residual once this refusal went loud). A
                // create with no storage gives the record its identity, the storage follows if
                // the texture has any (a respecify against the create's descriptor is never
                // deduped away), and the parameters land on the record that now exists. The
                // same repair covers the served context's teardown scope, where the records are
                // dropped while the objects live on. One retry, never a loop.
                const MGPResourceDesc healDesc = MGPipeBuildTextureResourceDesc(
                    texture, handle, entry.BindMask, /*storageDefined=*/false, kMGPipeNullHandle,
                    kMGPipeNullHandle, 0, 0);
                NoteDesc(healDesc, /*isCreate=*/true);
                PublishCreate(MGPipeKind::Texture, handle, entry, healDesc);
                const auto* mipmap = MG_State::GLState::AsMipmapTexture(&texture);
                const Bool hasStorage = mipmap != nullptr
                                            ? mipmap->GetMipmapLevelCount() > 0
                                            : texture.GetStorageType() == MobileGL::TextureStorageType::Buffer;
                if (hasStorage) {
                    // Can grow the table (a view's owner is acquired inside): no Entry& is held
                    // across it - `entry` is re-fetched below.
                    EmitResourceRespecify(texture, MGPipeTextureRespecifyScope::WholeResource, 0, 0);
                }
                accepted = MGPipeApplySetTextureParams(params);
            }
            Entry& latched = EntryFor(m_textures, handle);
            if (!accepted) {
                // Refused on its merits (a null built-in sampler, no consumer). Nothing latched:
                // the same versions re-send at the next call. Loud for the reason the sub-data
                // refusal is loud.
                ++m_refusedParamSets;
                MGLOG_E_ONCE("MGPipe: set_texture_params for texture %u {slot=%u, gen=%u} was refused; the "
                             "latch is not taken and the parameters are re-sent at the next call",
                             texture.GetExternalIndex(), handle.Slot, handle.Gen);
                return;
            }
            latched.HasParamsLatch = true;
            latched.ParamsVersion = paramsVersion;
            latched.SamplerVersion = samplerVersion;
            latched.ForceParamsResync = false;
        }

        void EmitRenderbufferCreate(RenderbufferObject& renderbuffer) {
            const MGPipeHandle handle = AcquireRenderbuffer(renderbuffer.GetLifetimeId());
            Entry& entry = EntryFor(m_renderbuffers, handle);
            const MGPResourceDesc desc = MGPipeBuildRenderbufferResourceDesc(renderbuffer, handle,
                                                                            entry.BindMask,
                                                                            /*storageDefined=*/false);
            NoteDesc(desc, /*isCreate=*/true);
            PublishCreate(MGPipeKind::Renderbuffer, handle, entry, desc);
        }

        // D-D2: THE RENDERBUFFER PUBLICATION HOLE, CLOSED BY EMISSION.
        //
        // RenderbufferObject::{SetInternalFormat, AllocateStorage, SetSamples} bump no version
        // and raise no notice, and the framebuffer dirty bit's shutter does not move when an
        // ALREADY-ATTACHED renderbuffer is re-storaged - so `glBindRenderbuffer;
        // glRenderbufferStorage(newSize)` on an attached renderbuffer was invisible. It is
        // closed HERE, from the storage entry point, and deliberately not by adding a version
        // counter to RenderbufferObject (a new member resizes the pull build's object and
        // breaks G1) nor by widening the shutter (which would fire the framebuffer emission on
        // an unrelated renderbuffer write).
        void EmitRenderbufferRespecify(RenderbufferObject& renderbuffer) {
            const MGPipeHandle handle = AcquireRenderbuffer(renderbuffer.GetLifetimeId());
            Entry& entry = EntryFor(m_renderbuffers, handle);
            const MGPResourceDesc desc = MGPipeBuildRenderbufferResourceDesc(renderbuffer, handle,
                                                                            entry.BindMask,
                                                                            /*storageDefined=*/true);
            if (entry.HasLastDesc && std::memcmp(&entry.LastDesc, &desc, sizeof(desc)) == 0) return;
            if (!MGPipeHandleIsPublished(MGPipeKind::Renderbuffer, handle)) {
                const MGPResourceDesc createDesc = MGPipeBuildRenderbufferResourceDesc(
                    renderbuffer, handle, entry.BindMask, /*storageDefined=*/false);
                NoteDesc(createDesc, /*isCreate=*/true);
                PublishCreate(MGPipeKind::Renderbuffer, handle, entry, createDesc);
            }
            NoteDesc(desc, /*isCreate=*/false);
            // A renderbuffer's storage is always the whole object: no levels, so no key.
            Bool accepted = ApplyRespecify(desc, nullptr);
            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                if (!accepted) {
                    // See the texture twin: the applier's refusal is the only thing that can
                    // say "I hold no record for this handle" once the latch has been set.
                    const MGPResourceDesc healDesc = MGPipeBuildRenderbufferResourceDesc(
                        renderbuffer, handle, entry.BindMask, /*storageDefined=*/false);
                    NoteDesc(healDesc, /*isCreate=*/true);
                    PublishCreate(MGPipeKind::Renderbuffer, handle, entry, healDesc);
                    accepted = ApplyRespecify(desc, nullptr);
                }
            }
            NoteRespecified(entry, desc, accepted);
        }

        // ---- the drain list (D-D4) ----
        //
        // Appended ONCE, on the first dirty mark of a level, and cleared at emission. Keyed on
        // the STORAGE OWNER from day one and for free: TextureObjectView forwards
        // MarkStorageDirty / MarkStorageDirtyRegion to the OWNER's methods after remapping the
        // level and the region, so an upload through a view and an upload through the owner
        // reach this function with the same object and the same owner-side coordinates.
        //
        // The per-slot key list is a short linear scan rather than a hash: a level count is
        // ~15, the cap on the rect list behind it is 96, and this runs on the glTexSubImage
        // path which has just memcpy'd texels.
        // THE PARAMETER TYPES ARE THE CONTRACT'S (PipeMutation.h): Uint32 rather than
        // MobileGL::TextureUploadTarget and Uint, because that declaration is the one door
        // MG_State has into the client and it may not name a frontend enumeration.
        //
        // THERE IS NO CLEAN ARM, and that is a DECLARED DEVIATION rather than a dropped half.
        // v1 carried a second entry point for MarkStorageDirty(..., false); the contract's hook
        // has no `dirty` parameter, and asking A to widen it would put a second signature in
        // MG_Pipe/PipeMutation.h for something the drain already collects. A level that goes
        // clean stays on the list until the NEXT drain walks it, where
        // `!mipmap->IsStorageDirty(...)` is the first test EmitOneLevel makes and returns
        // "nothing owed", so the entry is dropped from both lists there. The cost is one
        // IsStorageDirty call per cleaned level per drain, the list is bounded by the (texture,
        // level) pairs dirtied since the last validate point, and a re-dirty before that drain
        // is already covered by the entry still standing. What it must NOT be confused with is
        // dropping the level's TEXELS: nothing here clears a dirty flag.
        void NoteLevelDirty(ITextureObject& texture, Uint32 uploadTarget, Uint32 level) {
            if (m_draining) return;
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            Entry& entry = EntryFor(m_textures, handle);
            const Uint32 key = PackLevelKey(static_cast<MobileGL::TextureUploadTarget>(uploadTarget),
                                            static_cast<Uint>(level));
            for (const Uint32 present : entry.DrainKeys) {
                if (present == key) return;
            }
            entry.DrainKeys.push_back(key);
            m_drain.push_back(DrainEntry{handle, key});
        }

        // The DRAIN, at the validate point: one resource_subdata per dirty (storage owner,
        // upload target, level).
        //
        // WHO CLEARS THE FLAG, and why a bail cannot lose texels (D-D5): the client clears its
        // own m_isDirty / region / rects for a level ONLY when the record was actually
        // dispatched, and the applier accumulates the emitted shape into a per-record
        // pending-upload set that is server-side and survives every one of Espryt's bail arms.
        // A level whose record could not be built - no storage, no shadow, an empty box -
        // stays dirty and stays on the list, which is the safe direction.
        //
        // Returns the bytes that went on the wire, for the per-draw payload histogram.
        Uint64 DrainTextureSubData(GLContext& ctx) {
            (void)ctx;
            // THE EARLY-OUT, before anything is hashed or resolved: with nothing dirty this is
            // one integer test per verb, which is the whole reason the drain list exists
            // rather than a walk over every live texture.
            if (m_drain.empty()) return 0;
            m_draining = true;
            Uint64 bytes = 0;
            Vector<DrainEntry> retry;
            for (const DrainEntry& pending : m_drain) {
                if (EmitOneLevel(pending, bytes)) continue;
                retry.push_back(pending);
            }
            // Every slot's key list is rebuilt from what actually stayed behind, so a level
            // that was emitted is off both lists and a level that bailed is on both.
            for (const DrainEntry& pending : m_drain) {
                const SizeT slot = pending.Handle.Slot;
                if (slot < m_textures.size()) m_textures[slot].DrainKeys.clear();
            }
            for (const DrainEntry& pending : retry) {
                const SizeT slot = pending.Handle.Slot;
                if (slot < m_textures.size()) m_textures[slot].DrainKeys.push_back(pending.Key);
            }
            m_drain = Move(retry);
            m_draining = false;
            return bytes;
        }

        // ---- what a unit case reads. None of it costs a copy on the hot path: the two
        // descriptors are written by create and respecify, which run once per storage
        // definition rather than per upload, and the sub-data record is the emitter's own
        // staging buffer handed straight to the applier. ----
        const MGPResourceDesc& LastDesc() const { return m_lastDesc; }
        const MGPTextureParams& LastParams() const { return m_lastParams; }
        const MGPSubData& LastSubData() const { return m_lastSubData; }
        const Vector<MGPSubRegion>& LastRegions() const { return m_regions; }
        Uint64 CreateCount() const { return m_creates; }
        Uint64 RespecifyCount() const { return m_respecifies; }
        Uint64 ParamCount() const { return m_paramSets; }
        Uint64 SubDataCount() const { return m_subDatas; }
        // Records the applier REFUSED. The dirty flag survives one of these, which is the whole
        // of D-D5 step 1 - so a case that wants to prove the flag survived asserts on this.
        Uint64 RefusedSubDataCount() const { return m_refusedSubDatas; }
        // set_texture_params records the applier refused; the latch survives one of these (m-1).
        Uint64 RefusedParamCount() const { return m_refusedParamSets; }
        // What create_sampler_state put on the wire on this emitter's behalf, so the csob-blob
        // accounting does not under-report 100 bytes per built-in sampler mint. set_texture_params
        // itself returns no byte count - it is not emitted from the validate point's payload
        // histogram - so this is where the cache's answer lands.
        Uint64 SamplerCsoPayloadBytes() const { return m_samplerCsoPayloadBytes; }
        // Dead handles that still held an object when resolved: a death path that skipped the
        // emitter. 0 on a healthy tree; a case that drives every death path asserts it.
        Uint64 DeadResolveCount() const { return m_deadResolves; }
        MGPipeHandle BuiltinSamplerOf(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_textures.size()) return kMGPipeNullHandle;
            const Entry& entry = m_textures[slot];
            return entry.Gen == handle.Gen ? entry.BuiltinSampler : kMGPipeNullHandle;
        }
        SizeT DrainListSize() const { return m_drain.size(); }

        // A fresh context: what the server has is no longer what this emitter last sent. Only
        // LATCHES reset here - the applier's object records survive a make-current and
        // re-publishing them would move their serials for nothing.
        //
        // THE DRAIN LIST IS NOT A LATCH AND IS NOT CLEARED. It is a list of texels the client
        // still owes the server, and the server's pending-upload set is per RECORD, which
        // MGPipeApplierReset deliberately keeps. Clearing it here would drop exactly the
        // uploads a context switch has not flushed yet.
        void Reset() {}

        void ResetCounters() {
            m_creates = m_respecifies = m_paramSets = m_subDatas = 0;
            m_refusedSubDatas = 0;
            m_refusedParamSets = 0;
            m_samplerCsoPayloadBytes = 0;
            m_deadResolves = 0;
        }

        // A unit fixture's per-case reset; the library never calls it. See
        // MGPipeResourceTracker::ResetForTest for the rule this restates: a texture handle and
        // the applier record it names are SHARE-GROUP OBJECT STATE, so nothing here is
        // per-context and no re-publication path exists or may exist.
        void ResetForTest() {
            // EVERY REFERENCE THIS EMITTER OWES IS GIVEN BACK FIRST. A case that dropped the
            // table without releasing would pin cache entries for the rest of the process and
            // the next case's LRU would mint over capacity for reasons it cannot see.
            for (Entry& entry : m_textures) {
                MGPipeSamplerCsoCacheInstance().Release(entry.BuiltinSampler);
                entry.BuiltinSampler = kMGPipeNullHandle;
            }
            m_textures.clear();
            m_renderbuffers.clear();
            m_drain.clear();
            m_draining = false;
            m_regions.clear();
            m_lastDesc = MGPResourceDesc{};
            m_lastParams = MGPTextureParams{};
            m_lastSubData = MGPSubData{};
            ResetCounters();
        }

    private:
        struct Entry {
            ITextureObject* Texture = nullptr;
            Uint32 Gen = 0;
            Uint16 BindMask = 0;
            Bool ForceParamsResync = false;
            Bool HasLastDesc = false;
            // ID-14/ID-17: the CSO C's content-addressed cache handed this texture's BUILT-IN
            // sampler, and the ONE reference this emitter owes a Release for. Null until the
            // first set_texture_params. There is no Published flag beside it: c0b's
            // {kind, slot, gen} latch is the one answer both halves read.
            MGPipeHandle BuiltinSampler{};
            // The version-first skip for set_texture_params, and it reads BOTH counters
            // (clientsp-v2 rule 4): glTexParameter* moves GetTextureParamsVersion(), a write
            // that lands on the built-in SamplerObject moves only SamplerObject::GetVersion().
            Bool HasParamsLatch = false;
            Uint16 ParamsVersion = 0;
            Uint16 SamplerVersion = 0;
            MGPResourceDesc LastDesc{};
            Vector<Uint32> DrainKeys;
        };

        struct DrainEntry {
            MGPipeHandle Handle;
            Uint32 Key;
        };

        static constexpr Uint32 PackLevelKey(MobileGL::TextureUploadTarget uploadTarget, Uint level) {
            return (static_cast<Uint32>(uploadTarget) << 16) | (level & 0xFFFFu);
        }
        static constexpr MobileGL::TextureUploadTarget UnpackUploadTarget(Uint32 key) {
            return static_cast<MobileGL::TextureUploadTarget>(key >> 16);
        }
        static constexpr Uint UnpackLevel(Uint32 key) { return key & 0xFFFFu; }

        static Entry& EntryFor(Vector<Entry>& table, MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (slot >= table.size()) table.resize(slot + 1);
            return table[slot];
        }
        static Uint16 MaskOf(const Vector<Entry>& table, MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (slot >= table.size() || table[slot].Gen != handle.Gen) return Uint16{0};
            return table[slot].BindMask;
        }
        // A SLOT THE ALLOCATOR HAS HANDED OUT AGAIN CARRIES ITS PREDECESSOR'S ENTRY, and every
        // field in it is a lie about the new object (m4). The sticky BindMask is the one that
        // bites: the framebuffer emitter ORs RENDER_TARGET / DEPTH_STENCIL into these entries
        // whether or not the texture family is on, so a recycled slot's new texture inherited
        // the dead one's mask and its first descriptor said so. The generation is what
        // distinguishes them and the reset is here because AcquireTexture is the one door.
        //
        // IT IS THE BELT, NOT THE PATH (final review C-2): the death helper forwards to
        // NoteTextureDied, which retires the entry - drain entries, cache reference, latches,
        // mask - at the death itself. This stays for a slot whose death this emitter was never
        // told about, and drops the same reference if one is still standing.
        void RetireIfRecycled(Entry& entry, MGPipeHandle handle) {
            if (entry.Gen == handle.Gen) return;
            MGPipeSamplerCsoCacheInstance().Release(entry.BuiltinSampler);
            entry = Entry{};
        }

        // resource_create, and the LATCH IS TAKEN ONLY WHERE THE CREATE ACTUALLY WENT OUT
        // (D-I1, c0b): MGPipeHandleIsPublished is what the death helper reads, so latching on
        // a call the applier refused would emit a resource_destroy for a record that does not
        // exist - a refused call the applier asserts on in a verify build.
        void PublishCreate(MGPipeKind kind, MGPipeHandle handle, Entry& entry,
                           const MGPResourceDesc& desc) {
            Bool accepted = false;
            Bool dispatched = false;
            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                dispatched = true;
                accepted = MGPipeApplyResourceCreate(desc);
            }
            if (dispatched && !accepted) return;
            MGPipeNoteHandlePublished(kind, handle);
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
        }

        // `level` is null for the whole resource and a key for exactly one level; every caller
        // says which (final review C-1), and RepublishMask's null is deliberate - a mask move
        // replaces no storage at all.
        static Bool ApplyRespecify(const MGPResourceDesc& desc, const MGPRespecifiedLevel* level) {
            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                return MGPipeApplyResourceRespecify(desc, nullptr, level);
            }
            (void)level;
            return false;
        }

        // One respecify with one key, and the refusal self-heal beside it. THE SECOND HALF OF
        // THE SELF-HEAL, and the publication latch cannot give it: the latch answers "did a
        // create for this handle GO OUT", which stays true after
        // MGPipeApplierReleaseObjectRecords has dropped every object record - the scope a
        // served context's teardown takes while the frontend objects live on in the share
        // group. The applier's REFUSAL is the only signal that says "I hold nothing for this
        // handle", and the acceptance return is what makes it visible from here at all. One
        // retry, never a loop: a descriptor the applier refuses on its own merits (a target
        // that names no resource kind) is refused again and the flags stay set.
        Bool RespecifyOnce(ITextureObject& texture, MGPipeHandle handle, Entry& entry, const MGPResourceDesc& desc,
                           const MGPRespecifiedLevel* key, MGPipeHandle viewOf, MGPipeHandle bufferHandle,
                           Uint64 bufOffset, Uint64 bufSize) {
            Bool accepted = ApplyRespecify(desc, key);
            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                if (!accepted) {
                    const MGPResourceDesc healDesc = MGPipeBuildTextureResourceDesc(
                        texture, handle, entry.BindMask, /*storageDefined=*/false, viewOf, bufferHandle,
                        bufOffset, bufSize);
                    NoteDesc(healDesc, /*isCreate=*/true);
                    PublishCreate(MGPipeKind::Texture, handle, entry, healDesc);
                    accepted = ApplyRespecify(desc, key);
                }
            }
            return accepted;
        }

        static void NoteRespecified(Entry& entry, const MGPResourceDesc& desc, Bool accepted) {
            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                if (!accepted) return;
            }
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
        }

        // THE METADATA RESPECIFY (ID-18 M4). Every storage-defining field is the stored
        // descriptor's own byte for byte - the record IS entry.LastDesc with a new mask - which
        // is what makes the applier classify it as a metadata update: the descriptor is
        // replaced so the mask and the hint take their new values, the serial advances, and no
        // pending upload is dropped.
        void RepublishMask(MGPipeKind kind, MGPipeHandle handle, Entry& entry) {
            if (!MGPipeTextureSubsystemEnabled()) return;
            // Nothing has described this object to the applier yet, so the create or the first
            // respecify carries the new mask anyway - both read entry.BindMask.
            if (!entry.HasLastDesc || !MGPipeHandleIsPublished(kind, handle)) return;
            MGPResourceDesc desc = entry.LastDesc;
            desc.BindMask = entry.BindMask;
            desc.ImageBindableHint = (entry.BindMask & kMGPipeBindShaderImage) != 0 ? 1 : 0;
            if (std::memcmp(&entry.LastDesc, &desc, sizeof(desc)) == 0) return;
            NoteDesc(desc, /*isCreate=*/false);
            // A NULL LEVEL, DELIBERATELY (wire-v3 §5 item 6): a mask move replaces no storage,
            // and the applier classifies the identical storage fields as a metadata update
            // that drops nothing. A key here would name a level this call did not touch.
            NoteRespecified(entry, desc, ApplyRespecify(desc, nullptr));
        }

        void NoteDesc(const MGPResourceDesc& desc, Bool isCreate) {
            m_lastDesc = desc;
            if (isCreate) {
                ++m_creates;
            } else {
                ++m_respecifies;
            }
        }

        // True when the level's record went out and its flags may be cleared.
        Bool EmitOneLevel(const DrainEntry& pending, Uint64& bytes) {
            ITextureObject* texture = ResolveTexture(pending.Handle);
            if (texture == nullptr) return true; // the object is gone; nothing is owed
            auto* mipmap = MG_State::GLState::AsMipmapTexture(texture);
            if (mipmap == nullptr) return true; // a buffer texture has no level to upload
            const MobileGL::TextureUploadTarget uploadTarget = UnpackUploadTarget(pending.Key);
            const Uint level = UnpackLevel(pending.Key);
            if (!mipmap->IsStorageDirty(uploadTarget, level)) return true;

            const IntVec3 levelSize = mipmap->GetMipmapTexelSize(uploadTarget, level);
            const SizeT levelBytes = mipmap->GetMipmapByteSize(uploadTarget, level);
            const MGPipeLevelPitch pitch = MGPipeLevelPitchOf(levelSize, levelBytes);
            if (pitch.BytesPerTexel == 0) return false; // no storage yet; the texels are still owed
            const void* shadow = mipmap->MapMipmapData(uploadTarget, level);
            if (shadow == nullptr) return false;

            const MGPBox unionBox = MGPipeBoxOfDirtyRegion(mipmap->GetStorageDirtyRegion(uploadTarget, level));
            if (unionBox.W == 0 || unionBox.H == 0 || unionBox.D == 0) return false;

            // THE REGION LIST BEHIND THE BOX. 0 is legal and means "the union box is the whole
            // story" - it covers a single rect (identical to the box by construction), more
            // rects than the cap, and a summed area so close to the box's that one big upload
            // beats many small ones. The invariant that makes the server's choice safe is that
            // the two describe the SAME texels: every rect lies inside the box, and their union
            // is the box.
            MG_State::GLState::MipmapDirtyRegion rects[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
            const SizeT rectCount = mipmap->GetStorageDirtyRects(
                uploadTarget, level, rects, MG_State::GLState::MipmapStorage::kMaxDirtyRects);
            m_regions.clear();
            m_regions.reserve(rectCount);
            for (SizeT i = 0; i < rectCount; ++i) {
                m_regions.push_back(
                    MGPipeBuildSubRegion(MGPipeBoxOfDirtyRegion(rects[i]), levelSize, pitch));
            }

            m_lastSubData = MGPSubData{};
            m_lastSubData.Res = pending.Handle;
            // The contract's packer takes two Uint32s (c0c keeps MGPipeTypes.h backend-neutral),
            // so the frontend enumeration is widened here rather than there.
            m_lastSubData.Target = MGPipePackSubDataTarget(
                static_cast<Uint32>(MGPipeResourceTargetForTextureTarget(texture->GetTarget())),
                static_cast<Uint32>(uploadTarget));
            m_lastSubData.Level = static_cast<Uint16>(level);
            // ALWAYS 1 ON THE CLIENT SIDE. The conversion fallbacks (the packed-norm, widened
            // and fallback upload preparers) are the server's and run there, so the bytes this
            // record declares ARE the level shadow. The server clears the flag internally when
            // it converts, which is the `uploadData == mipData` pointer comparison turned into
            // a carried fact.
            m_lastSubData.SourceIsVerbatimLevelShadow = 1;
            m_lastSubData.UnionBox = unionBox;
            m_lastSubData.RegionCount = static_cast<Uint32>(m_regions.size());
            // "THIS RECORD DOES NOT DECLARE ITS BLOB", which is what a monolith emission is:
            // Seg is kMGHostSpanSegNone, Offset IS the address of the level shadow base, and a
            // zero Size means the destination box is what bounds the write. A non-zero Size
            // that did not match the record's own byte count would be Fatal{ProtocolCorruption}
            // on the far side, and a texture record's byte count is the server's to compute
            // once it has picked box-or-rects.
            m_lastSubData.Blob.Seg = kMGHostSpanSegNone;
            m_lastSubData.Blob.Offset = static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(shadow));
            m_lastSubData.Blob.Size = 0;

            // THE REGION LIST IS THE CALL'S VARIABLE TAIL AND IT IS HANDED OVER (M1). v1 built
            // m_regions, wrote its size into RegionCount and passed nothing, so on this base -
            // where the applier's tail exists - every scattered upload would have declared N
            // regions and supplied none (the applier faults on exactly that). A null tail is
            // correct ONLY for the whole-level shape, where RegionCount is 0.
            Bool accepted = false;
            Bool dispatched = false;
            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                dispatched = true;
                accepted = MGPipeApplyResourceSubData(m_lastSubData, shadow,
                                                      m_regions.empty() ? nullptr : m_regions.data());
            }
            ++m_subDatas;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::ClientTextureUploadEmissions, 1);
            }
            bytes += sizeof(MGPSubData) + m_regions.size() * sizeof(MGPSubRegion);
            // THE CLIENT CLEARS ITS OWN FLAG ONLY FOR A LEVEL THE APPLIER ACCEPTED (D-D5 step 1
            // read literally; ID-18 M3). v1 cleared on DISPATCH - and, with the wired constant
            // still 0, even on a call the `if constexpr` had discarded - so any refusal left the
            // server with nothing and the client with a clean flag, and since MG_Impl contains
            // no reader of a texture's dirty state the level simply stopped updating for the
            // life of the texture. The two refusal paths are invisible from here without this
            // answer: a dead or stale handle is a counted no-op and a corrupt record is a Fatal
            // that deliberately moves no counter.
            //
            // A REFUSED LEVEL STAYS DIRTY AND STAYS ON THE DRAIN LIST, which is the safe
            // direction and self-heals: the ordinary cause is a record the applier does not
            // hold, and the next respecify's self-healing create gives it one. It is LOUD
            // because a permanently refused level would otherwise re-emit once per verb for
            // ever with nothing to show for it.
            if (dispatched && !accepted) {
                ++m_refusedSubDatas;
                MGLOG_E_ONCE("MGPipe: resource_subdata for texture {slot=%u, gen=%u} level %u was refused; "
                             "the level stays dirty and is retried at the next validate point",
                             pending.Handle.Slot, pending.Handle.Gen, static_cast<Uint>(level));
                return false;
            }
            mipmap->MarkStorageDirty(uploadTarget, level, false);
            return true;
        }

        Vector<Entry> m_textures;
        Vector<Entry> m_renderbuffers;
        Vector<DrainEntry> m_drain;
        Vector<MGPSubRegion> m_regions;
        Bool m_draining = false;

        MGPResourceDesc m_lastDesc{};
        MGPTextureParams m_lastParams{};
        MGPSubData m_lastSubData{};

        Uint64 m_creates = 0;
        Uint64 m_respecifies = 0;
        Uint64 m_paramSets = 0;
        Uint64 m_subDatas = 0;
        Uint64 m_refusedSubDatas = 0;
        Uint64 m_refusedParamSets = 0;
        Uint64 m_samplerCsoPayloadBytes = 0;
        mutable Uint64 m_deadResolves = 0;
    };

    inline MGPipeTextureEmitter& MGPipeTextureEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason, and this one is not
        // hypothetical: ~TextureObjectBase reaches this emitter through the death helper's
        // RecordIsPublished / NoteRecordDestroyed pair, which read and WRITE its tables. A
        // destroyed emitter answers out of a freed Vector and the write grows it.
        static MGPipeTextureEmitter* emitter = new MGPipeTextureEmitter();
        return *emitter;
    }

    inline Bool MGPipeTextureSubsystemEnabled() {
        return (kMGPipeWiredTextureSubsystem & kMGPipeSubsystemTextureResources) != 0 &&
               (MG_Config::Features.PipePush & kMGPipeSubsystemTextureResources) != 0;
    }

    // ---------------------------------------------------------------------------------
    // WHAT USED TO BE HERE, AND WHY IT IS NOT (c0b, ID-13)
    // ---------------------------------------------------------------------------------
    //
    // v1 carried eight free functions - MGPipeMintAndCreateTexture, MGPipeEmitTextureRespecify,
    // MGPipeEmitTextureParams, MGPipeNoteTextureLevelDirty, MGPipeMintAndCreateRenderbuffer,
    // MGPipeEmitRenderbufferRespecify and the two ...ResourceDestroy halves - because at the
    // contract tag MG_Pipe/PipeMutation.h declared no texture birth hook and
    // MG_Impl/Pipe/PipeFill.cpp's death helpers hard-coded `published = false`. Every one of
    // them is now A's:
    //
    //   * the four MINTS and the nine EMISSIONS are declared in PipeMutation.h and defined in
    //     PipeFill.cpp, which gates them on FamilyIsLive(bit, kMGPipeWiredTextureSubsystem) and
    //     forwards to this class through ForwardWhenWired. MGPipeEmitTextureParams in
    //     particular was a NAME COLLISION - the contract declares that exact signature - so
    //     keeping the inline definition here would not have compiled at all;
    //   * step 1 of the death order is inside MGPipeEmitTextureDestroyAndFree /
    //     ...RenderbufferDestroyAndFree, which read MGPipeHandleIsPublished and emit the
    //     resource_destroy themselves, so the destructors call ONE helper and not two - and
    //     since the final review's C-2 the helper then forwards to NoteTextureDied /
    //     NoteRenderbufferDied above, so no ITextureObject* survives its object in this table
    //     and no dead level survives on the drain list;
    //   * the publication latch is PipeFill.cpp's {kind, slot, gen} table, written by
    //     PublishCreate above and read by those helpers.
    //
    // The self-healing create in EmitResourceRespecify stays this file's: c0b provides no such
    // path and it is what repairs a texture born while the subsystem bit was clear.
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
