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
// HOW MG_State REACHES THIS FILE, and it is a DEVIATION worth reading before the code.
//
// P3a's buffer family declares its emission points in MG_Pipe/PipeMutation.h and defines them
// in MG_Impl/Pipe/PipeFill.cpp, so BufferObject.cpp sees a declaration and never the client's
// tracker. P4a cannot copy that shape: BOTH of those files belong to the contract package for
// the whole phase (no file is touched twice by two packages), and they carry no texture
// emission declaration. The next-best arrangement, and the one used here, keeps the SAME
// property one level in:
//
//   * every texture emission point is a protected member of TextureObjectBase
//     (TextureState/TextureObject.h, guarded by MOBILEGL_PIPE_PUSH, non-virtual, so the pull
//     build's object layout and vtable are untouched) DECLARED there and DEFINED in
//     TextureState/TextureObject.cpp - the ONE MG_State translation unit that includes this
//     header. Every other texture .cpp - the cube's, the view's, the buffer texture's - calls
//     the inherited helper and still sees only a declaration.
//   * the renderbuffer half has no base class to hang helpers on and exactly one .cpp, so
//     RenderbufferState/RenderbufferObject.cpp is the second and last such translation unit.
//
// So the client's tracker reaches exactly two MG_State translation units instead of zero. The
// integrator can fold these six free functions into PipeMutation.h and PipeFill.cpp in one
// mechanical commit once the phase's file ownership relaxes; nothing else has to move.
#if MOBILEGL_PIPE_PUSH
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
    // IT IS STILL 0, AND THAT IS A BLOCKED FLIP RATHER THAN AN UNFINISHED ONE. Unlike the other
    // three P4a families, this one does not get four fresh apply entry points: the catalogue is
    // closed and a texture rides P3a's OWN resource_create / resource_respecify /
    // resource_subdata / resource_destroy rows. On a base without the wire package's `w1` those
    // four have P3a's BUFFER bodies, and two of their properties make a texture record actively
    // harmful rather than merely ignored:
    //
    //   * MGPipeApplierState::Resources is ONE vector indexed by SLOT (D-B2 makes it three, one
    //     per resource kind). Buffer, Texture and Renderbuffer slot spaces are independent, so
    //     a texture create at slot 12 OVERWRITES the buffer record at slot 12, and the next
    //     write to that buffer is refused against the texture's extent - a dropped content
    //     write with no diagnostic beyond the refusal counter;
    //   * SubDataBoxFault validates every record as the buffer half of MGPSubData, so a texture
    //     sub-data record is Fatal{ProtocolCorruption} on `record.Level != 0` alone (D-D4's
    //     drain cases and TextureTest.GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined
    //     abort in a verify build, which is how this was found rather than argued).
    //
    // So the flip is `w1`'s to unblock and the integrator's to make, in the rebase of this
    // branch onto the wire branch: change the 0 below to kMGPipeSubsystemTextureResources, and
    // nothing else. The whole conversion is already gated by TextureEmit's cases, which arm the
    // emitter directly (ArmForTest) and assert on the EMITTED records rather than on applier
    // state - so the flip cannot land untested, and until it lands nothing this file builds
    // reaches an applier that cannot hold it.
    inline constexpr Uint64 kMGPipeWiredTextureSubsystem = 0;

    // BOTH HALVES MATTER, exactly as MGPipeResourceSubsystemEnabled()'s two do. The bit is the
    // operator's per-subsystem A/B; the emitter's arm is "has this build's texture family been
    // switched on at all", and it is initialised from the constant above. There is no third
    // half - no MGPipeResourceOps member and no backend op table (D-B1: every P4a call is an
    // object record or working state the applier stores, and none of them dispatches to a
    // backend function pointer) - which is what makes the A/B a pure configuration question
    // rather than a bring-up-order one.
    inline Bool MGPipeTextureSubsystemEnabled();

    // DO THE RECORDS THIS EMITTER BUILDS REACH THE APPLIER ON THIS BASE? It is the wired
    // constant asked as a predicate, and it is a SECOND gate rather than the same one because
    // the two questions really are different while the flip is blocked:
    //
    //   * the emitter's ARM decides whether the family runs at all - the handles, the sticky
    //     bind mask, the descriptor dedupe, the drain list and the record construction;
    //   * this decides whether the four resource_* records are handed to P3a's apply bodies,
    //     which cannot hold them until the wire package's w1 gives them their Desc.Target
    //     branch and their three per-kind vectors.
    //
    // set_texture_params is deliberately NOT behind it: MGPipeApplySetTextureParams is one of
    // P4a's OWN fifteen entry points and is a stub at the contract tag, so a record handed to it
    // is stored by nobody and refused by nobody. Sending it is what keeps that seam exercised
    // rather than merely declared.
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

    // MGPSubData::Target, for a TEXTURE, and the packing is stated here because the payload has
    // exactly one Uint16 for two facts the server needs: WHICH KIND of storage the destination
    // is (the applier branches on it - a buffer target dispatches into MGPipeResourceOps, every
    // other target stores and returns) and WHICH CUBE FACE / upload target the level belongs
    // to, which is not derivable from the resource target at all.
    //
    //   low byte  = MGPipeResourceTarget          (0 == Buffer, and P3a's buffer records are
    //                                              unchanged: their upload byte is 0 too, so a
    //                                              buffer record still compares == 0 whole)
    //   high byte = MobileGL::TextureUploadTarget (26 enumerators; a byte is ample)
    //
    // The buffer half of the encoding is what keeps this backward-compatible with
    // MGPipeBuildSubDataRecord's `out.Target = kMGPipeResourceTargetBuffer`, so the applier's
    // buffer branch can keep testing the whole field or only the low byte and be right either
    // way.
    inline constexpr Uint16 MGPipePackSubDataTarget(Uint32 resourceTarget,
                                                    MobileGL::TextureUploadTarget uploadTarget) {
        return static_cast<Uint16>((resourceTarget & 0xFFu) |
                                   ((static_cast<Uint32>(uploadTarget) & 0xFFu) << 8));
    }
    inline constexpr Uint8 MGPipeSubDataResourceTargetOf(Uint16 packed) {
        return static_cast<Uint8>(packed & 0xFFu);
    }
    inline constexpr Uint8 MGPipeSubDataUploadTargetOf(Uint16 packed) {
        return static_cast<Uint8>((packed >> 8) & 0xFFu);
    }
    static_assert(MGPipePackSubDataTarget(kMGPipeResourceTargetBuffer,
                                          static_cast<MobileGL::TextureUploadTarget>(0)) ==
                      kMGPipeResourceTargetBuffer,
                  "a buffer sub-data record's Target must stay exactly kMGPipeResourceTargetBuffer");

    // MGPTextureParams::DepthStencilMode. The frontend keeps a GLenum (GL_DEPTH_COMPONENT /
    // GL_STENCIL_INDEX, 0x1902 / 0x1901) and the payload byte cannot hold one, so the two
    // legal values are numbered - 0 is DEPTH_COMPONENT, which is also the GL initial value and
    // therefore what a zero-initialised record already says.
    inline constexpr Uint8 kMGPipeDepthStencilModeDepth = 0;
    inline constexpr Uint8 kMGPipeDepthStencilModeStencil = 1;
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
            entry.Gen = handle.Gen;
            return handle;
        }
        MGPipeHandle FindRenderbuffer(const RenderbufferObject& renderbuffer) const {
            return MGPipeSlots().FindByLifetimeId(MGPipeKind::Renderbuffer, renderbuffer.GetLifetimeId());
        }

        // The texture a handle names, or null. A RAW pointer is exact here for
        // MGPipeResourceTracker::Resolve's reason: the entry exists only between the create the
        // constructor emits and the destroy the destructor emits, and the Gen compare is what
        // refuses a stale handle rather than resolving it to whatever now occupies the slot.
        ITextureObject* ResolveTexture(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_textures.size()) return nullptr;
            const Entry& entry = m_textures[slot];
            if (entry.Texture == nullptr || entry.Gen != handle.Gen) return nullptr;
            if (MGPipeSlots().GenOfSlot(MGPipeKind::Texture, handle.Slot) != handle.Gen) return nullptr;
            return entry.Texture;
        }

        // ---- the sticky bind mask (D-A4) ----
        //
        // ORed, never cleared, and emitted on BOTH resource_create and every
        // resource_respecify, exactly as P3a's buffer mask is. The four bits nothing set before
        // P4a get their producers here and in the framebuffer emitter: RENDER_TARGET and
        // DEPTH_STENCIL from an attachment point, SAMPLER from a resolved sampler view and
        // SHADER_IMAGE from a resolved image unit (the sampler package's two).
        void NoteTextureBoundAs(MGPipeHandle handle, Uint16 bit) {
            if (MGPipeHandleIsNull(handle)) return;
            Entry& entry = EntryFor(m_textures, handle);
            const Uint16 before = entry.BindMask;
            entry.BindMask = static_cast<Uint16>(before | bit);
            // AN ImageBindableHint TRANSITION IS THE ONE THING THE CLIENT ASKS A RESYNC FOR
            // (D-E2): the widened-channel carrier needs a swizzle override that the frontend's
            // own params version does not move for, so the transition arms ForceResync on the
            // next set_texture_params rather than being silently folded into the descriptor.
            if ((before & kMGPipeBindShaderImage) == 0 && (bit & kMGPipeBindShaderImage) != 0) {
                entry.ForceParamsResync = true;
            }
        }
        void NoteRenderbufferBoundAs(MGPipeHandle handle, Uint16 bit) {
            if (MGPipeHandleIsNull(handle)) return;
            Entry& entry = EntryFor(m_renderbuffers, handle);
            entry.BindMask = static_cast<Uint16>(entry.BindMask | bit);
        }
        Uint16 TextureBindMask(MGPipeHandle handle) const { return MaskOf(m_textures, handle); }
        Uint16 RenderbufferBindMask(MGPipeHandle handle) const { return MaskOf(m_renderbuffers, handle); }

        // ---- the publication latch (D-I1) ----
        //
        // The create is gated at its call site and the destroy inside the death helper, so the
        // two ask the SAME question at two different moments. An object born while the
        // subsystem bit was clear and destroyed after it was set would otherwise free its slot
        // with the applier's record still Live, on a slot about to be handed out again. So the
        // answer is LATCHED at the create and the destroy uses the latched one.
        Bool TextureRecordIsPublished(MGPipeHandle handle) const {
            return PublishedIn(m_textures, handle);
        }
        Bool RenderbufferRecordIsPublished(MGPipeHandle handle) const {
            return PublishedIn(m_renderbuffers, handle);
        }
        void NoteTextureRecordDestroyed(MGPipeHandle handle) { Retire(m_textures, handle); }
        void NoteRenderbufferRecordDestroyed(MGPipeHandle handle) { Retire(m_renderbuffers, handle); }

        // ---- the three object calls ----

        void EmitTextureCreate(ITextureObject& texture) {
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            Entry& entry = EntryFor(m_textures, handle);
            const MGPResourceDesc desc = MGPipeBuildTextureResourceDesc(
                texture, handle, entry.BindMask, /*storageDefined=*/false, kMGPipeNullHandle,
                kMGPipeNullHandle, 0, 0);
            entry.Published = true;
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
            NoteDesc(desc, /*isCreate=*/true);
            if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceCreate(desc);
        }

        // resource_create for a texture whose DERIVED object does not exist yet - the base
        // constructor. Nothing virtual is touched: the target and the GL name are the two
        // facts a create carries and both are plain members by then. See
        // MGPipeTextureStorageKindForTarget for why the storage kind may not be asked for here.
        void EmitTextureCreateFromBase(Uint64 lifetimeId, ITextureObject* object,
                                       MobileGL::TextureTarget target, Uint externalIndex) {
            const MGPipeHandle handle = AcquireTexture(lifetimeId, object);
            Entry& entry = EntryFor(m_textures, handle);
            MGPResourceDesc desc{};
            desc.Resource = handle;
            desc.Target = static_cast<Uint8>(MGPipeResourceTargetForTextureTarget(target));
            desc.StorageKind = MGPipeTextureStorageKindForTarget(target);
            desc.BindMask = entry.BindMask;
            desc.GlNameForDiag = static_cast<Uint32>(externalIndex);
            entry.Published = true;
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
            NoteDesc(desc, /*isCreate=*/true);
            if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceCreate(desc);
        }

        // resource_respecify, from every storage-defining entry point. DEDUPED ON THE
        // DESCRIPTOR ITSELF rather than on a version, because the entry points that reach here
        // are the ones that move the SHAPE and several of them do not move the descriptor at
        // all (glTexParameter TEXTURE_BASE_LEVEL bumps the shape version and changes no field
        // this record carries). A byte compare of an 88-byte POD is cheaper than the emission
        // it avoids, and it is the same "version-first skip before anything expensive" shape
        // every other P4a emission takes.
        void EmitTextureRespecify(ITextureObject& texture) {
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            Entry& entry = EntryFor(m_textures, handle);
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
            const MGPResourceDesc desc = MGPipeBuildTextureResourceDesc(
                texture, handle, entry.BindMask, /*storageDefined=*/true, viewOf, bufferHandle, bufOffset,
                bufSize);
            if (entry.HasLastDesc && std::memcmp(&entry.LastDesc, &desc, sizeof(desc)) == 0) return;
            // SELF-HEALING IN BOTH DIRECTIONS, the P3a m12 shape: a texture born while the
            // subsystem bit was clear has no applier record, and every later respecify would be
            // REFUSED. A create rather than a respecify, because that is what the record's
            // absence means and because the applier starts a record over on a create.
            if (!entry.Published) {
                const MGPResourceDesc createDesc = MGPipeBuildTextureResourceDesc(
                    texture, handle, entry.BindMask, /*storageDefined=*/false, viewOf, bufferHandle,
                    bufOffset, bufSize);
                entry.Published = true;
                NoteDesc(createDesc, /*isCreate=*/true);
                if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceCreate(createDesc);
            }
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
            NoteDesc(desc, /*isCreate=*/false);
            // NO initial bytes: a texture's texels travel as resource_subdata out of the drain
            // list, never inside its storage definition. This is what keeps glTexImage2D's
            // "define the level and upload it" one allocation and one upload rather than two.
            if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceRespecify(desc, nullptr);
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
            // THE BUILT-IN SAMPLER'S CSO SLOT IS KEYED ON THE SamplerObject's OWN LIFETIME ID,
            // which is the same key ~SamplerObject's death helper resolves through
            // (MGPipeEmitSamplerCsoDestroyAndFree). The CALL that fills the record -
            // create_sampler_state - is the sampler package's; minting the handle is client
            // state and is this record's to name.
            const MGPipeHandle builtinSampler =
                MGPipeSlots().Acquire(MGPipeKind::SamplerCso, sampler->GetLifetimeId());
            const MGPTextureParams params =
                MGPipeBuildTextureParams(texture, handle, builtinSampler, entry.ForceParamsResync);
            entry.ForceParamsResync = false;
            m_lastParams = params;
            ++m_paramSets;
            MGPipeApplySetTextureParams(params);
        }

        void EmitRenderbufferCreate(RenderbufferObject& renderbuffer) {
            const MGPipeHandle handle = AcquireRenderbuffer(renderbuffer.GetLifetimeId());
            Entry& entry = EntryFor(m_renderbuffers, handle);
            const MGPResourceDesc desc = MGPipeBuildRenderbufferResourceDesc(renderbuffer, handle,
                                                                            entry.BindMask,
                                                                            /*storageDefined=*/false);
            entry.Published = true;
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
            NoteDesc(desc, /*isCreate=*/true);
            if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceCreate(desc);
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
            if (!entry.Published) {
                const MGPResourceDesc createDesc = MGPipeBuildRenderbufferResourceDesc(
                    renderbuffer, handle, entry.BindMask, /*storageDefined=*/false);
                entry.Published = true;
                NoteDesc(createDesc, /*isCreate=*/true);
                if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceCreate(createDesc);
            }
            entry.LastDesc = desc;
            entry.HasLastDesc = true;
            NoteDesc(desc, /*isCreate=*/false);
            if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceRespecify(desc, nullptr);
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
        void NoteLevelDirty(ITextureObject& texture, MobileGL::TextureUploadTarget uploadTarget, Uint level) {
            if (m_draining) return;
            const MGPipeHandle handle = AcquireTexture(texture.GetLifetimeId(), &texture);
            Entry& entry = EntryFor(m_textures, handle);
            const Uint32 key = PackLevelKey(uploadTarget, level);
            for (const Uint32 present : entry.DrainKeys) {
                if (present == key) return;
            }
            entry.DrainKeys.push_back(key);
            m_drain.push_back(DrainEntry{handle, key});
        }

        // MarkStorageDirty(..., false) from outside the drain - a level respecified, truncated
        // or explicitly marked clean. The entry stops describing anything and is dropped from
        // the per-slot list; the global list is compacted at the next drain, which is where
        // walking it is already paid for.
        void NoteLevelClean(ITextureObject& texture, MobileGL::TextureUploadTarget uploadTarget, Uint level) {
            if (m_draining) return;
            const MGPipeHandle handle = FindTexture(texture);
            if (MGPipeHandleIsNull(handle)) return;
            const SizeT slot = handle.Slot;
            if (slot >= m_textures.size()) return;
            Entry& entry = m_textures[slot];
            const Uint32 key = PackLevelKey(uploadTarget, level);
            for (SizeT i = 0; i < entry.DrainKeys.size(); ++i) {
                if (entry.DrainKeys[i] != key) continue;
                entry.DrainKeys[i] = entry.DrainKeys.back();
                entry.DrainKeys.pop_back();
                return;
            }
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

        void ResetCounters() { m_creates = m_respecifies = m_paramSets = m_subDatas = 0; }

        // ---- the arm (see kMGPipeWiredTextureSubsystem) ----
        //
        // "Does this build's texture family emit at all", initialised from the wired constant.
        // It is a RUNTIME latch and not a constant only because the flip is blocked on the wire
        // package's `w1` while the conversion below is finished: a unit case arms it, drives a
        // frontend mutation and asserts on the record the emitter built, so the conversion is
        // gated by a test on a base whose applier could not yet hold that record. Once the
        // constant is flipped this stays true for the life of the process and ArmForTest is
        // redundant rather than wrong.
        Bool Armed() const { return m_armed; }
        void ArmForTest(Bool armed) { m_armed = armed; }

        // A unit fixture's per-case reset; the library never calls it. See
        // MGPipeResourceTracker::ResetForTest for the rule this restates: a texture handle and
        // the applier record it names are SHARE-GROUP OBJECT STATE, so nothing here is
        // per-context and no re-publication path exists or may exist.
        void ResetForTest() {
            m_textures.clear();
            m_renderbuffers.clear();
            m_drain.clear();
            m_draining = false;
            m_regions.clear();
            m_lastDesc = MGPResourceDesc{};
            m_lastParams = MGPTextureParams{};
            m_lastSubData = MGPSubData{};
            m_armed = (kMGPipeWiredTextureSubsystem & kMGPipeSubsystemTextureResources) != 0;
            ResetCounters();
        }

    private:
        struct Entry {
            ITextureObject* Texture = nullptr;
            Uint32 Gen = 0;
            Uint16 BindMask = 0;
            Bool Published = false;
            Bool ForceParamsResync = false;
            Bool HasLastDesc = false;
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
            return slot < table.size() ? table[slot].BindMask : Uint16{0};
        }
        static Bool PublishedIn(const Vector<Entry>& table, MGPipeHandle handle) {
            if (MGPipeHandleIsNull(handle)) return false;
            const SizeT slot = handle.Slot;
            return slot < table.size() && table[slot].Published && table[slot].Gen == handle.Gen;
        }
        static void Retire(Vector<Entry>& table, MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (slot >= table.size() || table[slot].Gen != handle.Gen) return;
            table[slot] = Entry{};
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
            m_lastSubData.Target = MGPipePackSubDataTarget(
                MGPipeResourceTargetForTextureTarget(texture->GetTarget()), uploadTarget);
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

            if constexpr (MGPipeTextureRecordsReachTheApplier()) {
                MGPipeApplyResourceSubData(m_lastSubData, shadow);
            }
            ++m_subDatas;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::ClientTextureUploadEmissions, 1);
            }
            bytes += sizeof(MGPSubData) + m_regions.size() * sizeof(MGPSubRegion);
            // THE CLIENT CLEARS ITS OWN FLAG, and only now (D-D5's inversion): the record was
            // accepted, the applier holds the shape, and MG_Impl contains no reader of this
            // texture's dirty state at all - the frontend never reads it back.
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
        Bool m_armed = (kMGPipeWiredTextureSubsystem & kMGPipeSubsystemTextureResources) != 0;
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
        return MGPipeTextureEmitterInstance().Armed() &&
               (MG_Config::Features.PipePush & kMGPipeSubsystemTextureResources) != 0;
    }

    // ---------------------------------------------------------------------------------
    // The six entry points MG_State calls. See this file's header comment for why they are
    // here rather than in MG_Pipe/PipeMutation.h.
    // ---------------------------------------------------------------------------------

    // From TextureObjectBase's constructor. The mint is unconditional in a push build; the
    // CALL is what the subsystem predicate gates.
    inline void MGPipeMintAndCreateTexture(MG_State::GLState::ITextureObject* object, Uint64 lifetimeId,
                                           MobileGL::TextureTarget target, Uint externalIndex) {
        MGPipeTextureEmitter& emitter = MGPipeTextureEmitterInstance();
        if (!MGPipeTextureSubsystemEnabled()) {
            emitter.AcquireTexture(lifetimeId, object);
            return;
        }
        emitter.EmitTextureCreateFromBase(lifetimeId, object, target, externalIndex);
    }

    inline void MGPipeEmitTextureRespecify(MG_State::GLState::ITextureObject& texture) {
        if (!MGPipeTextureSubsystemEnabled()) return;
        MGPipeTextureEmitterInstance().EmitTextureRespecify(texture);
    }

    inline void MGPipeEmitTextureParams(MG_State::GLState::ITextureObject& texture) {
        if (!MGPipeTextureSubsystemEnabled()) return;
        MGPipeTextureEmitterInstance().EmitTextureParams(texture);
    }

    inline void MGPipeNoteTextureLevelDirty(MG_State::GLState::ITextureObject& texture,
                                            MobileGL::TextureUploadTarget uploadTarget, Uint level, Bool dirty) {
        if (!MGPipeTextureSubsystemEnabled()) return;
        MGPipeTextureEmitter& emitter = MGPipeTextureEmitterInstance();
        if (dirty) {
            emitter.NoteLevelDirty(texture, uploadTarget, level);
        } else {
            emitter.NoteLevelClean(texture, uploadTarget, level);
        }
    }

    inline void MGPipeMintAndCreateRenderbuffer(MG_State::GLState::RenderbufferObject& renderbuffer) {
        MGPipeTextureEmitter& emitter = MGPipeTextureEmitterInstance();
        if (!MGPipeTextureSubsystemEnabled()) {
            emitter.AcquireRenderbuffer(renderbuffer.GetLifetimeId());
            return;
        }
        emitter.EmitRenderbufferCreate(renderbuffer);
    }

    inline void MGPipeEmitRenderbufferRespecify(MG_State::GLState::RenderbufferObject& renderbuffer) {
        if (!MGPipeTextureSubsystemEnabled()) return;
        MGPipeTextureEmitterInstance().EmitRenderbufferRespecify(renderbuffer);
    }

    // ---------------------------------------------------------------------------------
    // STEP 1 OF THE THREE-STEP DEATH ORDER, and it is here rather than inside the contract's
    // MGPipeEmitTextureDestroyAndFree for the same ownership reason the six entry points above
    // are: MG_Impl/Pipe/PipeFill.cpp, where that helper lives, is the contract package's for
    // the whole phase, and at the tag it hard-codes `published = false` with the note "the
    // texture emitter publishes nothing yet". This client package cannot edit that line, so it
    // supplies the answer from the destructor instead, one statement BEFORE the helper:
    //
    //     ~TextureObjectBase / ~RenderbufferObject
    //       -> MGPipeEmit<Kind>ResourceDestroy(lifetimeId)   // 1. the wire delete
    //       -> MGPipeEmit<Kind>DestroyAndFree(lifetimeId)    // 2. the notice, 3. the slot
    //
    // which is EXACTLY the order PipeMutation.h fixes and not a variation on it: the applier's
    // record is dropped while nothing can have re-handed the slot out, the death notice is
    // raised while the handle still resolves, and the slot goes back last. The integrator can
    // fold these two functions into the helpers' bodies in one mechanical commit once the
    // phase's file ownership relaxes.
    //
    // PUBLISHED-GATED RATHER THAN SLOT-GATED, for the reason PipeFill.cpp states in full: a
    // slot is not evidence of a record, because a backend twin table mints one through
    // MGPipeSlots().Acquire whether or not the subsystem ever asked this client to emit a
    // create - which is exactly what a MOBILEGL_PIPE_PUSH lane with P4a's bits clear runs - and
    // a resource_destroy on such a handle is a refused call the applier counts and asserts on.
    inline Bool MGPipeEmitTextureResourceDestroy(Uint64 lifetimeId) {
        const MGPipeHandle handle = MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, lifetimeId);
        MGPipeTextureEmitter& emitter = MGPipeTextureEmitterInstance();
        if (!emitter.TextureRecordIsPublished(handle)) return false;
        MGPHandleOnly only{};
        only.Handle = handle;
        only.Kind = static_cast<Uint32>(MGPipeKind::Texture);
        if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceDestroy(only);
        emitter.NoteTextureRecordDestroyed(handle);
        return true;
    }

    inline Bool MGPipeEmitRenderbufferResourceDestroy(Uint64 lifetimeId) {
        const MGPipeHandle handle = MGPipeSlots().FindByLifetimeId(MGPipeKind::Renderbuffer, lifetimeId);
        MGPipeTextureEmitter& emitter = MGPipeTextureEmitterInstance();
        if (!emitter.RenderbufferRecordIsPublished(handle)) return false;
        MGPHandleOnly only{};
        only.Handle = handle;
        only.Kind = static_cast<Uint32>(MGPipeKind::Renderbuffer);
        if constexpr (MGPipeTextureRecordsReachTheApplier()) MGPipeApplyResourceDestroy(only);
        emitter.NoteRenderbufferRecordDestroyed(handle);
        return true;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
