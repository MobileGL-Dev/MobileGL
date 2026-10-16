// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersTextures.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    static Bool IsAngleLlvmpipeRenderer() {
        return g_GLESCapabilities.IsAngleLlvmpipeRenderer;
    }

    // P3b/P4b (wave 2-D, package D3). See the declaration for why a view's storage clauses are
    // asked of another record, why null means "ask again" and why the deletion order cannot
    // strand this walk.
    const MG_Pipe::MGPipeResourceRecord* PipeTextureStorageRecordForRecord(
        const MG_Pipe::MGPipeResourceRecord& record) {
        // ONE hop is the shape the client emits and the second is already generous; the bound is
        // here so a cyclic chain is a null answer rather than a hang, not because a real chain
        // is ever this long.
        constexpr Uint32 kViewChainBound = 4;
        const MG_Pipe::MGPipeResourceRecord* storage = &record;
        for (Uint32 hop = 0; hop < kViewChainBound; ++hop) {
            if (MG_Pipe::MGPipeHandleIsNull(storage->Desc.ViewOf)) return storage;
            storage = PipeTextureRecordForHandle(storage->Desc.ViewOf);
            if (storage == nullptr) return nullptr;
        }
        return nullptr;
    }

    namespace TextureImpl {
        BackendTextureObject::BackendTextureObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendTextureId == 0) {
                MGLOG_E_ONCE("Failed to generate texture object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated texture object with ID: %u.", m_backendTextureId);
            }
        }

        BackendTextureObject::~BackendTextureObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendTextureId == 0) {
                return;
            }
            // Scrub every driver-state shadow that could false-skip when the name
            // or this heap address is recycled - regardless of whether the id can
            // still be deleted.
            ScratchFBOImpl::NoteTextureIdDeleted(m_backendTextureId);
            for (auto& unitCache : g_boundTexturesCache) {
                for (auto& boundTexture : unitCache) {
                    if (boundTexture == this) {
                        boundTexture = nullptr;
                    }
                }
            }
            // TEMP-EXP (leak texture deletes): /sdcard/MG/exp_leak_texture_deletes.
            // Discriminator for the mali-mem-purge hiccup theory: never hand the
            // driver a texture free, so the purge daemon has nothing to reclaim.
            static const Bool s_expLeakTextureDeletes = [] {
                FILE* f = std::fopen("/sdcard/MG/exp_leak_texture_deletes", "rb");
                if (!f) return false;
                std::fclose(f);
                return true;
            }();
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteTextures &&
                !s_expLeakTextureDeletes) {
                g_GLESFuncs.glDeleteTextures(1, &m_backendTextureId);
                if (m_bufferImageSplitViewId != 0) {
                    g_GLESFuncs.glDeleteTextures(1, &m_bufferImageSplitViewId);
                }
            }
            m_backendTextureId = 0;
            m_bufferImageSplitViewId = 0;
        }

        void BackendTextureObject::Bind(GLenum target, Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_activeTextureUnit != unit) {
                ActivateTextureUnit(unit);
            }

            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (this == g_boundTexturesCache[unit][targetN]) return;

            g_GLESFuncs.glBindTexture(target, m_backendTextureId);
            g_boundTexturesCache[unit][targetN] = this;
        }

        Uint BackendTextureObject::GetBackendTextureId() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendTextureId;
        }

        // P5e (tx2), CONTRACT-P5E §5.2 / scout G-S2-5. See the declaration.
        void BackendTextureObject::RequireImageBindableStorageByHandle(
            MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPipeResourceRecord& record) {
            if (m_imageBindableStorageRequired) {
                return;
            }
            // P13 W4b (defect h): monolith's record arm takes the body too; its old arm only
            // cleared m_isInitialized and replayed the frontend shadow at the next sync.
            const auto format = static_cast<TextureInternalFormat>(record.Desc.InternalFormat);
            // ES image binding needs immutable storage, but an already
            // immutable core-format allocation needs no replacement. Keep
            // its GPU contents and native name; normal serial/shape checks
            // below still consume pending uploads and real redefinitions.
            if (m_isInitialized && m_backendStorageImmutable && !GetImageBindableStorageWidening(format)) {
                m_imageBindableStorageRequired = true;
                m_forceTextureParamsResync = true;
                return;
            }
            // P9 W2 (docs/archive/notes/p9/W2-REMINT.md): EVERY BYTE THIS RE-MINT REPLAYS IS ALREADY ON THE
            // SERVER. The GPU image holds what the GPU wrote (render targets, clears, copies,
            // generated mips, image stores) and the staged store holds what the client uploaded;
            // the client is never asked, so no pull protocol exists. Reading only the store would
            // be wrong: a GPU-written level's store run is stale or absent - both host corpus
            // re-mints (iris-photon, iris-derivative) are render targets with no staged bytes.
            const Bool hadBackendStorage = m_isInitialized;
            // The R-16 control for W2-b's fallback below: a readback the driver refuses cannot be
            // produced on the host (llvmpipe renders every image format), so the test lane forces
            // one. Read by the SERVER, like the MGITEST_MAGMA_FORCE_* knobs.
            static const Bool s_forceReadbackFailure =
                std::getenv("MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE") != nullptr;
            auto& store = MG_Record::ServerStagedTexture();
            const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(res);
            const auto target = BufferImpl::StagedTextureTargetForPipeTarget(record.Desc.Target);
            GLenum canonicalFormat = GL_NONE, canonicalType = GL_NONE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                MG_Util::ConvertTextureInternalFormatToGLEnum(format), {}, nullptr,
                &canonicalFormat, &canonicalType);
            for (const auto& uploadTarget :
                 BufferImpl::StagedUploadTargetsForPipeTarget(record.Desc.Target)) {
                for (Uint32 level = 0; level < record.Desc.Levels; ++level) {
                    const auto targetCode = static_cast<Uint16>(uploadTarget);
                    const auto levelCode = static_cast<Uint16>(level);
                    const IntVec3 extent = store.LevelExtentOrUndefined(key, targetCode, levelCode);
                    if (extent.x() <= 0 || extent.y() <= 0 || extent.z() <= 0) continue;
                    const auto* pending = FindPipeTextureUpload(record, targetCode, levelCode);
                    const Bool covered = store.IsCovered(key, targetCode, levelCode);
                    const MG_Pipe::MGPBox whole{0, 0, 0, static_cast<Uint32>(extent.x()),
                        static_cast<Uint32>(extent.y()), static_cast<Uint32>(extent.z())};
                    // A genuinely full pending write owns every byte. A
                    // union box covering the level is not sufficient when
                    // its explicit rectangles leave GPU-written gaps.
                    const auto coversWhole = [&](const auto& box) {
                        return box.X == 0 && box.Y == 0 && box.Z == 0 &&
                               box.W == whole.W && box.H == whole.H && box.D == whole.D;
                    };
                    const Bool pendingWhole = pending && (pending->Regions.empty()
                        ? coversWhole(pending->UnionBox)
                        : std::any_of(pending->Regions.begin(), pending->Regions.end(), coversWhole));
                    // W2-a: ONLY A LEVEL THE DRIVER ACTUALLY HOLDS IS READ BACK. The store's
                    // Defined-ness follows the client's respecifies, the driver's follows this
                    // twin's last sync, and the two differ for a level defined (null data, or null
                    // data plus a partial upload) after that sync: the driver has no such level, so
                    // the GPU cannot have written it. A covered level then replays its staged run
                    // (the client's whole-level shadow, pending boxes included) and an uncovered one
                    // is undefined content with nothing to preserve. Reading it anyway was
                    // Fatal{ResourceUnavailable, "image-promotion-readback"} on legal GL.
                    const Bool nativeLevel =
                        m_isInitialized &&
                        NativeTextureLevelHasExtent(m_backendTextureId, target, uploadTarget,
                                                    static_cast<GLint>(level), extent);
                    if (nativeLevel && !(covered && pendingWhole)) {
                        Vector<Uint8> snapshot;
                        const Bool readBack =
                            !s_forceReadbackFailure &&
                            ReadTextureLevelTight(m_backendTextureId, target, uploadTarget, format,
                                                  static_cast<GLint>(level), extent,
                                                  /*sourceUsesImageCarrier=*/false, canonicalFormat,
                                                  canonicalType, snapshot);
                        if (!readBack && !covered) {
                            // The GPU holds this level, the driver will not hand it back, and no
                            // CPU byte of it exists anywhere: the one case with no honest answer.
                            MGLOG_F("MGPipe: Fatal{ResourceUnavailable, \"image-promotion-readback\"} {%u,%u} level=%u",
                                    res.Slot, res.Gen, level);
                            std::abort();
                        }
                        if (!readBack) {
                            // W2-b: the driver refused the readback (a format it cannot attach
                            // to a framebuffer - SNORM without EXT_render_snorm, 16-bit norm
                            // without EXT_texture_norm16, float without EXT_color_buffer_float)
                            // but the store covers the level. Its staged run is exactly what the
                            // monolith re-mint replays; texels the GPU wrote since that upload
                            // are not preserved, which is monolith's answer too.
                            MGLOG_W_ONCE("MGPipe: image promotion of texture {%u,%u} level %u: the driver "
                                         "refused the readback (internal format %u); replaying the level's "
                                         "staged bytes instead (remint-readback-fallback)",
                                         res.Slot, res.Gen, level, record.Desc.InternalFormat);
                        }
                        if (readBack && pending) {
                            // Native contents include prior draws/image
                            // writes; the pending stage owns ONLY its boxes.
                            // Merge those boxes before adopting a whole-level
                            // snapshot, never overwrite untouched GPU texels
                            // with the rest of a stale client staging run.
                            const SizeT pixels = static_cast<SizeT>(extent.x()) * extent.y() * extent.z();
                            const SizeT stagedSize = store.LevelByteSize(key, targetCode, levelCode);
                            if (!covered || !pixels || snapshot.size() != stagedSize || stagedSize % pixels) {
                                MGLOG_F("MGPipe: Fatal{StageSnapshotTooNarrow, \"image-promotion-pending\"}");
                                std::abort();
                            }
                            const SizeT bpp = stagedSize / pixels;
                            const Uint8* staged = store.RequireLevelBytes(key, targetCode, levelCode, "image-promotion");
                            const auto merge = [&](Int32 x, Int32 y, Int32 z, Uint32 w, Uint32 h, Uint32 d) {
                                if (x < 0 || y < 0 || z < 0 || Uint64(x) + w > whole.W ||
                                    Uint64(y) + h > whole.H || Uint64(z) + d > whole.D) {
                                    MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"image-promotion-region\"}");
                                    std::abort();
                                }
                                for (Uint32 slice = 0; slice < d; ++slice) {
                                    for (Uint32 row = 0; row < h; ++row) {
                                        const SizeT offset = ((static_cast<SizeT>(z + slice) * whole.H + y + row) * whole.W + x) * bpp;
                                        Memcpy(snapshot.data() + offset, staged + offset, static_cast<SizeT>(w) * bpp);
                                    }
                                }
                            };
                            if (pending->Regions.empty()) {
                                const auto& box = pending->UnionBox;
                                merge(box.X, box.Y, box.Z, box.W, box.H, box.D);
                            } else {
                                for (const auto& box : pending->Regions)
                                    merge(box.X, box.Y, box.Z, box.W, box.H, box.D);
                            }
                        }
                        if (readBack) {
                            store.Adopt(key, targetCode, levelCode, extent, snapshot.data(), snapshot.size());
                        }
                    }
                    // Undefined contents have no byte run to preserve. A
                    // first allocation can still be image-bindable without
                    // inventing an upload or consulting the frontend.
                    if (!store.IsCovered(key, targetCode, levelCode)) continue;
                    if (!RearmPipeTextureLevelUpload(res,
                            MG_Pipe::MGPipePackSubDataTarget(record.Desc.Target, static_cast<Uint32>(uploadTarget)),
                            levelCode, whole)) {
                        MGLOG_F("MGPipe: Fatal{ResourceUnavailable, \"image-promotion-pending-capacity\"}");
                        std::abort();
                    }
                }
            }
            // W2-d: `trp=` counts a re-mint of storage the backend ALREADY held, on this arm as
            // on the frontend one (which counts in RequireImageBindableStorage). The keep path
            // above is not a re-mint and returns before this; a first allocation holds nothing.
            if (hadBackendStorage && MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::TextureRemintPulls, 1);
            }
            // Every defined byte now belongs to the server and every required
            // replay is in its pending set. The existing upload path builds and
            // fills the immutable/widened carrier from those records.
            m_imageBindableStorageRequired = true;
            m_isInitialized = false;
            // The widened carrier's swizzle override, exactly as the frontend overload's tail
            // sets it and for the same reason: the parameter sync is gated on a params version
            // this transition does not move.
            m_forceTextureParamsResync = true;
        }

        // Replaces one level's shadow bytes with what the driver texture holds for it, then copies
        // back the client writes the driver has not been sent yet - exactly the texels written
        // (the level's dirty footprint), not the union box or the rect list, which also span texels
        // the client never wrote and which only the driver's copy holds correctly. Returns false,
        // leaving the shadow untouched, when the driver's copy cannot stand in for the shadow: the
        // client wrote every texel of the level, the level is stored compressed, the driver has no
        // image at this level and extent (defined after the last sync), or the readback cannot be
        // served (a multisample image, a format the driver will not attach to a framebuffer).
        //
        // P11 M, `pipePending` (push builds): THE CLIENT WRITES THE DRIVER HAS NOT BEEN SENT MAY
        // LIVE IN THE APPLIER'S PENDING SET INSTEAD. On the texture handle arm the client clears its
        // own dirty flags when it emits a level (D-D5), so the frontend model below answers "clean"
        // for a write that is still only in the shadow; the entry the applier accumulated for this
        // (uploadTarget, level) names the same shadow-grid boxes. Both are merged over the
        // snapshot. Only a box-only entry whose box IS the level owns every byte of it (dev's rule:
        // a union box spanning the level with gaps in it is not a whole-level write); an entry with
        // regions never short-circuits, since accumulated regions may overlap and their texels do
        // not add up - the readback and the merge below give the same bytes when they do cover it.
        // Null on the pull build's arm and the legacy arm.
        static Bool AdoptDriverLevelIntoShadow(GLuint backendTextureId, BackendTextureObject& backendTexture,
                                               const MG_State::GLState::ITextureObject& stateTextureObject,
                                               MG_State::GLState::TextureObjectMipmap& mipmapObject,
                                               TextureUploadTarget uploadTarget, Uint level
                                               ,
                                               const MG_Pipe::MGPipeResourceRecord::PendingUpload* pipePending
        ) {
            const IntVec3 texelSize = mipmapObject.GetMipmapTexelSize(uploadTarget, level);
            const IntVec3 extent{texelSize.x(), texelSize.y(), std::max(texelSize.z(), 1)};
            const SizeT texelCount =
                static_cast<SizeT>(extent.x()) * static_cast<SizeT>(extent.y()) * static_cast<SizeT>(extent.z());
            Vector<MG_State::GLState::MipmapDirtyRegion> pending;
            mipmapObject.GetStorageDirtyFootprint(uploadTarget, level, pending);
            // Disjoint boxes, so their texels add up; a union box spanning the level is not enough.
            SizeT pendingTexels = 0;
            for (const auto& box : pending) {
                pendingTexels += box.TexelCount();
            }
            if (!pending.empty() && pendingTexels >= texelCount) return false;
            // Every box the entry names, checked against the level before anything is read: a box
            // outside it is a corrupt record, and the answer is the pre-readback one (keep the
            // shadow) rather than a copy out of bounds.
            const auto pipeBoxInLevel = [&](Int32 x, Int32 y, Int32 z, Uint32 w, Uint32 h, Uint32 d) {
                return x >= 0 && y >= 0 && z >= 0 && Uint64(x) + w <= Uint64(extent.x()) &&
                       Uint64(y) + h <= Uint64(extent.y()) && Uint64(z) + d <= Uint64(extent.z());
            };
            if (pipePending != nullptr) {
                const auto& box = pipePending->UnionBox;
                if (!pipeBoxInLevel(box.X, box.Y, box.Z, box.W, box.H, box.D)) {
                    MGLOG_E_ONCE("MGPipe: texture %u level %u: a pending upload's box lies outside the level; the "
                                 "image-bindable re-mint replays the shadow as it stands",
                                 stateTextureObject.GetExternalIndex(), level);
                    return false;
                }
                for (const auto& region : pipePending->Regions) {
                    if (!pipeBoxInLevel(region.X, region.Y, region.Z, region.W, region.H, region.D)) {
                        MGLOG_E_ONCE("MGPipe: texture %u level %u: a pending upload's region lies outside the "
                                     "level; the image-bindable re-mint replays the shadow as it stands",
                                     stateTextureObject.GetExternalIndex(), level);
                        return false;
                    }
                }
                if (pipePending->Regions.empty() && box.X == 0 && box.Y == 0 && box.Z == 0 &&
                    box.W == static_cast<Uint32>(extent.x()) && box.H == static_cast<Uint32>(extent.y()) &&
                    box.D == static_cast<Uint32>(extent.z())) {
                    return false;
                }
            }
            if (mipmapObject.GetMipmapCompressedFormat(uploadTarget, level) != GL_NONE) return false;
            if (!g_GLESFuncs.glGetTexLevelParameteriv) return false;

            const TextureTarget target = stateTextureObject.GetTarget();
            const GLenum nativeTarget = ConvertTextureTargetToBackendGLEnum(target);
            const GLenum faceTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
            const IntVec3 backendExtent = GetBackendUploadSize(target, texelSize);
            GLint driverWidth = 0, driverHeight = 0, driverDepth = 0;
            backendTexture.Bind(nativeTarget);
            g_GLESFuncs.glGetTexLevelParameteriv(faceTarget, static_cast<GLint>(level), GL_TEXTURE_WIDTH, &driverWidth);
            g_GLESFuncs.glGetTexLevelParameteriv(faceTarget, static_cast<GLint>(level), GL_TEXTURE_HEIGHT,
                                                 &driverHeight);
            g_GLESFuncs.glGetTexLevelParameteriv(faceTarget, static_cast<GLint>(level), GL_TEXTURE_DEPTH, &driverDepth);
            if (driverWidth != backendExtent.x() || driverHeight != backendExtent.y() ||
                std::max(driverDepth, 1) != std::max(backendExtent.z(), 1)) {
                return false;
            }

            const TextureInternalFormat format = stateTextureObject.GetFormat();
            GLenum shadowFormat = GL_NONE, shadowType = GL_NONE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(MG_Util::ConvertTextureInternalFormatToGLEnum(format),
                                                                  {}, nullptr, &shadowFormat, &shadowType);
            Vector<Uint8> snapshot;
            if (!ReadTextureLevelTight(backendTextureId, target, uploadTarget, format, static_cast<GLint>(level),
                                       texelSize, /*sourceUsesImageCarrier=*/false, shadowFormat, shadowType,
                                       snapshot)) {
                return false;
            }
            const SizeT shadowBytes = mipmapObject.GetMipmapByteSize(uploadTarget, level);
            auto* shadow = static_cast<Uint8*>(mipmapObject.MapMipmapData(uploadTarget, level));
            if (shadow == nullptr || snapshot.size() != shadowBytes || texelCount == 0 || shadowBytes % texelCount) {
                return false;
            }

            if (!pending.empty()) {
                // The boxes index the SHADOW's texel grid (a 1D array's layers are its rows), which
                // is also the snapshot's byte order.
                const SizeT bpp = shadowBytes / texelCount;
                const SizeT rowBytes = static_cast<SizeT>(extent.x()) * bpp;
                const SizeT sliceBytes = static_cast<SizeT>(extent.y()) * rowBytes;
                for (const auto& box : pending) {
                    if (box.Empty()) continue;
                    const SizeT spanBytes = static_cast<SizeT>(box.hi.x() - box.lo.x()) * bpp;
                    for (Int z = box.lo.z(); z < box.hi.z(); ++z) {
                        for (Int y = box.lo.y(); y < box.hi.y(); ++y) {
                            const SizeT offset = static_cast<SizeT>(z) * sliceBytes + static_cast<SizeT>(y) * rowBytes +
                                                 static_cast<SizeT>(box.lo.x()) * bpp;
                            Memcpy(snapshot.data() + offset, shadow + offset, spanBytes);
                        }
                    }
                }
            }
            if (pipePending != nullptr) {
                // The same merge for the applier's entry: its regions when it carries a list, its
                // union box when it does not (RegionCount 0 = "the box is the whole story", D-D3).
                const SizeT bpp = shadowBytes / texelCount;
                const SizeT rowBytes = static_cast<SizeT>(extent.x()) * bpp;
                const SizeT sliceBytes = static_cast<SizeT>(extent.y()) * rowBytes;
                const auto mergePipeBox = [&](Int32 x, Int32 y, Int32 z, Uint32 w, Uint32 h, Uint32 d) {
                    for (Uint32 slice = 0; slice < d; ++slice) {
                        for (Uint32 row = 0; row < h; ++row) {
                            const SizeT offset = static_cast<SizeT>(z + static_cast<Int32>(slice)) * sliceBytes +
                                                 static_cast<SizeT>(y + static_cast<Int32>(row)) * rowBytes +
                                                 static_cast<SizeT>(x) * bpp;
                            Memcpy(snapshot.data() + offset, shadow + offset, static_cast<SizeT>(w) * bpp);
                        }
                    }
                };
                if (pipePending->Regions.empty()) {
                    const auto& box = pipePending->UnionBox;
                    mergePipeBox(box.X, box.Y, box.Z, box.W, box.H, box.D);
                } else {
                    for (const auto& region : pipePending->Regions) {
                        mergePipeBox(region.X, region.Y, region.Z, region.W, region.H, region.D);
                    }
                }
            }
            Memcpy(shadow, snapshot.data(), shadowBytes);
            return true;
        }

        void BackendTextureObject::RecreateBackendTexture() {
            if (m_backendTextureId != 0) {
                ScratchFBOImpl::NoteTextureIdDeleted(m_backendTextureId);
                // Application FBO twins that attached the dying id memoize on FRONTEND
                // attachment versions, which this backend-side re-mint does not move;
                // without this bump their driver FBOs would keep the deleted name
                // attached forever (see g_attachmentBackendIdGeneration).
                ++FramebufferImpl::g_attachmentBackendIdGeneration;
                if (m_contextGeneration == g_backendContextGeneration) {
                    g_GLESFuncs.glDeleteTextures(1, &m_backendTextureId);
                }
                for (auto& unitCache : g_boundTexturesCache) {
                    for (auto& boundTexture : unitCache) {
                        if (boundTexture == this) {
                            boundTexture = nullptr;
                        }
                    }
                }
            }

#if MOBILEGL_BUILD_DISAGGREGATED
            // A shared image's EGLImage lives exactly as long as the name it was bound to.
            m_sharedImage = nullptr;
#endif
            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendTextureId == 0) {
                MGLOG_E_ONCE("Failed to regenerate texture object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Regenerated texture object with ID: %u.", m_backendTextureId);
            }
            m_isInitialized = false;
            m_backendStorageImmutable = false;
            m_prevTextureInfo = {};
            // The new ES texture starts at the ES defaults, so every parameter this object had
            // already pushed onto the old one is gone. The change-detection caches below would
            // otherwise still claim those values are in force and SyncTextureParamsToBackend
            // would skip the whole pass on the unchanged params version, leaving the driver
            // texture at defaults for the rest of its life. Latent for swizzle, LOD range and
            // border colour long before GL_DEPTH_STENCIL_TEXTURE_MODE joined them; the mode
            // makes it visible because falling back to the default silently samples the wrong
            // aspect rather than merely mis-filtering.
            m_cacheLodRange = {0, 1000};
            m_cacheBorderColor = {0.0f, 0.0f, 0.0f, 0.0f};
            m_cacheSwizzleParams = {TextureSwizzleParam::Red, TextureSwizzleParam::Green, TextureSwizzleParam::Blue,
                                    TextureSwizzleParam::Alpha};
            m_cacheDepthStencilTextureMode = GL_DEPTH_COMPONENT;
            m_forceTextureParamsResync = true;
            // The filter/wrap/LOD cache belongs to the name that just went away, and its gate is
            // the frontend SAMPLER's version, which a backend re-mint does not move - so without
            // this the new driver texture keeps the ES defaults for life. See m_forceSamplerResync.
            m_cacheSamplerParameters = SamplerParameters{};
            m_forceSamplerResync = true;
        }

        void ExerciseDefaultUnpackScopeForTesting() {
            const ScopedDefaultUnpackState scope;
        }

        // See Managers.h. One flag for every context, like the image-unit high-water mark: a
        // dispatch on another context can consume an upload's flag before the uploading
        // context's own dispatch sees it - a hole only a shared texture re-uploaded on one context
        // and image-accessed on another can fall into, and GL wants a fence between those anyway.
        static Bool g_inPlaceTextureUploadPending = false;

        void NoteInPlaceTextureUpload() { g_inPlaceTextureUploadPending = true; }

        GLbitfield TakeInPlaceTextureUploadBarrierBits() {
            if (!g_inPlaceTextureUploadPending) return 0;
            g_inPlaceTextureUploadPending = false;
            return GL_TEXTURE_UPDATE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        }

        // Components per texel the frontend format's client data carries. Only the three-channel
        // formats that can be widened to a four-channel render target need an answer (see
        // PrepareChannelWidenedUpload); everything else keeps its own layout and reports 0.
        Uint GetWidenableClientComponentCount(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB10: // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB12: // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
                return 3;
            default:
                return 0;
            }
        }

        // True when the widened format's client data is integer rather than normalized. The two
        // classes share every narrow component type - GL_RGB8I and GL_RGB8_SNORM are both uploaded
        // as GL_BYTE - but their "1.0" differs: an integer channel's one is the integer 1, a
        // normalized channel's is the saturated field. The type alone cannot tell them apart, so
        // the source format has to.
        Bool IsIntegerWidenableFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
                return true;
            default:
                return false;
            }
        }
// P5e (tx2), CONTRACT-P5E §5.2: THE TEXTURE'S TARGET, from the descriptor whenever a record was
// resolved. The two live `GetTarget()` reads left INSIDE the handle arm (scout G-S2-2, the
// parameter sync and the built-in sampler sync) read Desc.Target here, through the same
// StagedTextureTargetForPipeTarget mapping the storage sync already routes through - so all
// three bodies answer the question from one carrier rather than two authorities.
#define MGB_TEXPARAM_TARGET(rec, obj)                                                                                  \
    ((rec) != nullptr ? BufferImpl::StagedTextureTargetForPipeTarget((rec)->Desc.Target) : (obj)->GetTarget())

        // The ES entry point for EXT/OES_texture_view, whichever spelling this driver brought.
        // Callers must have checked g_GLESCapabilities.SupportsTextureView first - the capability
        // is the extension AND the pointer, because eglGetProcAddress hands back live-looking
        // stubs (see AcquireGLESFunctions).
        static MG_External::GLES::glTextureViewEXT_PTR ResolveTextureViewEntryPoint() {
            if (g_GLESFuncs.glTextureViewEXT != nullptr) {
                return g_GLESFuncs.glTextureViewEXT;
            }
            return reinterpret_cast<MG_External::GLES::glTextureViewEXT_PTR>(g_GLESFuncs.glTextureViewOES);
        }

        // P5e (tx2): see the declaration for the split between what the record answers and what
        // the wire still cannot say about a view.
        void BackendTextureObject::SyncTextureViewToBackendByRecord(
            MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPipeResourceRecord& record,
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (!g_GLESCapabilities.SupportsTextureView) {
                // Unreachable through the API - the frontend refuses glTextureView without
                // GL_ARB_texture_view - and kept for the reason the frontend arm keeps its twin.
                MGLOG_E_ONCE("Texture view %u reached the backend on a driver without "
                             "EXT/OES_texture_view.",
                             record.Desc.GlNameForDiag);
                return;
            }
            // The storage owner by handle, through the same by-handle entry every other draw-path
            // texture takes: no frontend object, no allocator probe.
            const SharedPtr<BackendTextureObject> storageBackendObject =
                SyncTextureToBackendByHandle(record.Desc.ViewOf, m_imageBindableStorageRequired);
            if (!storageBackendObject) {
                MGLOG_E_ONCE("Failed to sync the storage texture {%u, %u} of view %u on the handle arm.",
                             record.Desc.ViewOf.Slot, record.Desc.ViewOf.Gen, record.Desc.GlNameForDiag);
                return;
            }
            const Uint storageBackendTextureId = storageBackendObject->GetBackendTextureId();
            if (storageBackendTextureId == 0) {
                MGLOG_D("Storage texture of view %u has no ES name yet.", record.Desc.GlNameForDiag);
                return;
            }
            // P3b/P4b D3: THE SERIAL THIS TWIN STAMPS IS THE STORAGE'S, not this view's, because
            // the gate that reads it back (IsDrawSyncCleanByRecord) now asks the same record. It
            // is read AFTER the storage sync above - which has just consumed the owner's pending
            // uploads - and taken by VALUE, because the applier's vector may move under any later
            // call and a Uint64 cannot dangle. A storage record that has gone answers 0, which
            // the gate reads as "never clean": the direction that re-syncs.
            const MG_Pipe::MGPipeResourceRecord* storageRecord = PipeTextureStorageRecordForRecord(record);
            const Uint64 storageSerial = storageRecord != nullptr ? storageRecord->Serial : 0;
            if (m_isInitialized && m_viewSourceBackendTextureId == storageBackendTextureId) {
                // The steady case, and the only one this arm has to be fast at: one record read,
                // one integer compare and the serial stamp that keeps the aggregate clean gate
                // (IsDrawSyncCleanByRecord) answering true for the next draw.
                m_syncedResourceSerial = storageSerial;
                return;
            }
            // The view window is a sampler-view CSO, created even when this
            // texture is only image-bound or attached to an FBO. It is not part
            // of the resource descriptor, and needs no frontend fallback.
            (void)stateTextureObject;
            const auto& applier = MG_Pipe::MGPipeApplier();
            const auto handle = record.ViewCso;
            if (MG_Pipe::MGPipeHandleIsNull(handle) || handle.Slot >= applier.SamplerViewCsos.size()) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"texture-view-cso\"} {%u,%u}", res.Slot, res.Gen);
                std::abort();
            }
            const auto& cso = applier.SamplerViewCsos[handle.Slot];
            const auto& view = cso.View;
            if (!cso.Live || cso.Gen != handle.Gen || view.Texture != res || !view.NumLevels || !view.NumLayers) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"texture-view-window\"} {%u,%u}", res.Slot, res.Gen);
                std::abort();
            }
            const auto viewTarget = BufferImpl::StagedTextureTargetForPipeTarget(record.Desc.Target);
            const GLenum nativeTarget = ConvertTextureTargetToBackendGLEnum(viewTarget);
            GLenum internalFormat = 0, transferFormat = 0, transferType = 0;
            GenerateTextureFormatInfo(static_cast<TextureInternalFormat>(view.InternalFormat),
                &internalFormat, &transferFormat, &transferType, viewTarget);
            // A name already bound or used as a view cannot be redefined by
            // glTextureView. This also invalidates native binding/FBO caches.
            RecreateBackendTexture();
            DebugImpl::ErrorLopper::Clear();
            ResolveTextureViewEntryPoint()(m_backendTextureId, nativeTarget, storageBackendTextureId,
                internalFormat, view.MinLevel, view.NumLevels, view.MinLayer, view.NumLayers);
            const GLenum error = g_GLESFuncs.glGetError();
            if (error != GL_NO_ERROR) {
                MGLOG_F("MGPipe: Fatal{ResourceUnavailable, \"texture-view-native\"} {%u,%u} error=0x%x",
                        res.Slot, res.Gen, error);
                std::abort();
            }
            m_viewSourceBackendTextureId = storageBackendTextureId;
            m_isInitialized = true;
            m_backendStorageImmutable = true;
            m_prevTextureInfo = {static_cast<TextureInternalFormat>(record.Desc.InternalFormat),
                record.Desc.Width, record.Desc.Height, record.Desc.Depth, view.NumLevels, 0,
                record.Desc.Samples, record.Desc.FixedSampleLocations != 0};
            // The storage's, for the steady arm's reason above (P3b/P4b D3).
            m_syncedResourceSerial = storageSerial;
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // SHARED IMAGES (docs/archive/notes/anland/plan-ahb-dmabuf.md). The client defined
        // level 0 as WxH RGBA8 with no data and then attached the image; the record's descriptor
        // still says so, but the texels are the image's, so nothing below may allocate or upload.
        Bool BackendTextureObject::SyncSharedImageStorage(const MG_Pipe::MGPipeResourceRecord& record) {
            const Uint64 imageId = record.SharedImageId;
            if (imageId == 0) {
                // Respecified away from the image. The ES name is an EGLImage sibling, which must not
                // be redefined in place: the immutable flag sends the normal path through
                // RecreateBackendTexture, and the EGLImage goes with the name it was bound to.
                m_sharedImageId = 0;
                m_isInitialized = false;
                m_backendStorageImmutable = true;
                return false;
            }

            // Steady state: bound to this image already (the serial moves with sub-data and with a
            // metadata respecify, neither of which changes what the name samples). A failed bind is
            // retried only once the record moves. Pending sub-data is the caller's
            // (ReplaySharedImageUploads), after the image's sync state has been acquired.
            if (m_sharedImageId == imageId && m_isInitialized &&
                (m_sharedImage != nullptr || m_syncedResourceSerial == record.Serial)) {
                m_syncedResourceSerial = record.Serial;
                return true;
            }

            // A fresh mutable name: the current one may hold immutable storage (which no EGLImage
            // can replace) or another image's sibling. RecreateBackendTexture also moves the FBO
            // twins' attachment generation and forces the parameter resync the new name needs.
            RecreateBackendTexture();
            m_sharedImage = nullptr;
            m_sharedImageId = imageId;
            m_isInitialized = true;
            // Whatever comes next redefines storage through a re-mint (see the imageId == 0 arm).
            m_backendStorageImmutable = true;
            m_syncedResourceSerial = record.Serial;

            const TextureTarget target = BufferImpl::StagedTextureTargetForPipeTarget(record.Desc.Target);
            if (ConvertTextureTargetToBackendGLEnum(target) != GL_TEXTURE_2D) {
                MGLOG_E_ONCE("Shared image texture %u: target %s cannot take an EGLImage; left empty",
                             record.Desc.GlNameForDiag, MG_Util::ConvertTextureTargetToString(target).c_str());
                return true;
            }
            String why;
            SharedImageImpl::EglImageRef image = SharedImageImpl::CreateEglImage(imageId, why);
            if (image == nullptr) {
                MGLOG_E_ONCE("Shared image texture %u: image %llu cannot be bound (%s); left empty",
                             record.Desc.GlNameForDiag, static_cast<unsigned long long>(imageId), why.c_str());
                return true;
            }
            Bind(GL_TEXTURE_2D);
            if (SharedImageImpl::IsYuv(image)) {
                // A YUV image is CONVERTED into the name's own RGBA8 level at each frame's first use
                // (SharedImageImpl::ConvertYuvForSampling), never made its storage.
                while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                }
                g_GLESFuncs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(record.Desc.Width),
                                           static_cast<GLsizei>(record.Desc.Height));
                if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                    MGLOG_E_ONCE("Shared image texture %u: no %ux%u RGBA8 storage for YUV image %llu; left empty",
                                 record.Desc.GlNameForDiag, record.Desc.Width, record.Desc.Height,
                                 static_cast<unsigned long long>(imageId));
                    return true;
                }
            } else if (!SharedImageImpl::TargetBoundTexture2D(image)) {
                MGLOG_E_ONCE("Shared image texture %u: glEGLImageTargetTexture2DOES refused image %llu; left empty",
                             record.Desc.GlNameForDiag, static_cast<unsigned long long>(imageId));
                return true;
            }
            m_sharedImage = std::move(image);
            m_sharedImageUploadsRgb = false;
            // The shape the name now has: the descriptor's level 0, and that one level only.
            m_prevTextureInfo = {static_cast<TextureInternalFormat>(record.Desc.InternalFormat),
                                 record.Desc.Width,
                                 record.Desc.Height,
                                 record.Desc.Depth,
                                 1,
                                 0,
                                 record.Desc.Samples,
                                 record.Desc.FixedSampleLocations != 0};
            return true;
        }

        // SUB-DATA ON A SHARED IMAGE IS WRITTEN INTO IT. An X server drawing with glamor uploads
        // its clients' pixels (MIT-SHM / PutImage) into window pixmaps whose level 0 is a shared
        // image, with glTexSubImage2D: the texels are in the staged store (the client defined the
        // level without data, so only the uploaded boxes are covered), and they go into the
        // EGLImage-backed name box by box. Every pending entry leaves the set either way, or the
        // per-draw clean gate would never close. Called after AcquireForSampling, so the upload is
        // ordered after the image's pending reads and writes like any other use.
        void BackendTextureObject::ReplaySharedImageUploads(const MG_Pipe::MGPipeResourceRecord& record) {
            if (record.PendingUploads.empty()) return;
            Vector<std::pair<Uint16, Uint16>> keys;
            Vector<std::pair<Uint16, MG_Pipe::MGPBox>> boxes;
            keys.reserve(record.PendingUploads.size());
            for (const auto& pending : record.PendingUploads) {
                const Uint16 uploadHalf = MG_Pipe::MGPipeSubDataUploadTargetOf(pending.UploadTarget);
                keys.emplace_back(uploadHalf, pending.Level);
                if (pending.Level != 0) continue; // an image has one level
                if (pending.Regions.empty()) {
                    boxes.emplace_back(uploadHalf, pending.UnionBox);
                } else {
                    for (const auto& region : pending.Regions)
                        boxes.emplace_back(uploadHalf, MG_Pipe::MGPBox{region.X, region.Y, region.Z, region.W, region.H,
                                                                       region.D});
                }
            }
            if (m_sharedImage != nullptr && !boxes.empty()) {
                auto& store = MG_Record::ServerStagedTexture();
                const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(record.Desc.Resource);
                Bind(GL_TEXTURE_2D);
                ScopedDefaultUnpackState unpackState;
                BufferImpl::BindPixelUnpackBufferId(0);
                for (const auto& [uploadHalf, box] : boxes) {
                    Int rowTexels = 0;
                    // A shared image is RGBA8 to every client that names it (the level the client
                    // defined over it): four bytes a texel.
                    const Uint8* bytes =
                        store.CoveredBoxBytes(key, uploadHalf, 0, box.X, box.Y, box.W, box.H, 4, &rowTexels);
                    if (bytes == nullptr) {
                        MGLOG_E_ONCE("Shared image texture %u: an upload of %ux%u at (%d,%d) has no staged texels; "
                                     "not written",
                                     record.Desc.GlNameForDiag, box.W, box.H, box.X, box.Y);
                        continue;
                    }
                    const Uint8* origin =
                        bytes + (static_cast<SizeT>(box.Y) * static_cast<SizeT>(rowTexels) + static_cast<SizeT>(box.X)) * 4;
                    if (!m_sharedImageUploadsRgb) {
                        while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                        }
                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, rowTexels);
                        g_GLESFuncs.glTexSubImage2D(GL_TEXTURE_2D, 0, box.X, box.Y, static_cast<GLsizei>(box.W),
                                                    static_cast<GLsizei>(box.H), GL_RGBA, GL_UNSIGNED_BYTE, origin);
                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                        // An image of an X format (no alpha) is an RGB8 texture to the driver, which
                        // takes only RGB texels: this image's uploads are converted from now on.
                        if (g_GLESFuncs.glGetError() != GL_INVALID_OPERATION) continue;
                        m_sharedImageUploadsRgb = true;
                    }
                    Vector<Uint8> rgb(static_cast<SizeT>(box.W) * box.H * 3);
                    for (Uint32 row = 0; row < box.H; ++row) {
                        const Uint8* src = origin + static_cast<SizeT>(row) * static_cast<SizeT>(rowTexels) * 4;
                        Uint8* dst = rgb.data() + static_cast<SizeT>(row) * box.W * 3;
                        for (Uint32 x = 0; x < box.W; ++x) {
                            dst[x * 3 + 0] = src[x * 4 + 0];
                            dst[x * 3 + 1] = src[x * 4 + 1];
                            dst[x * 3 + 2] = src[x * 4 + 2];
                        }
                    }
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    g_GLESFuncs.glTexSubImage2D(GL_TEXTURE_2D, 0, box.X, box.Y, static_cast<GLsizei>(box.W),
                                                static_cast<GLsizei>(box.H), GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                }
                MGLOG_I_ONCE("Shared image texture %u: sub-data written into its image (%s texels)",
                             record.Desc.GlNameForDiag, m_sharedImageUploadsRgb ? "RGB" : "RGBA");
            }
            for (const auto& [uploadHalf, level] : keys) ConsumePipeTextureUpload(record.Desc.Resource, uploadHalf, level);
        }
#endif

        // Stamps the same per-draw clean-gate keys a completed storage sync stamps, so a view
        // that needs no work costs the same nothing per draw that any other synced texture does.
        void BackendTextureObject::StampViewSyncKeys(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (MG_Pipe::gPipeInputs->IsLive()) {
                m_syncedShapeContextId = MG_Pipe::gPipeInputs->GetTextureContextId();
                m_syncedShapeGeneration = MG_Pipe::gPipeInputs->GetSamplingResolutionGeneration();
                m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
            }
            m_syncedContentVersion = stateTextureObject->GetContentVersion();
        }

        const SamplerParameters* BackendTextureObject::ResolvePushedBuiltinSampler(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject,
            const MG_Pipe::MGPipeResourceRecord* record) {
            if (record == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture %u has no applier record on the handle arm, so its "
                             "built-in sampler cannot be pushed (handle {%u, %u})",
                             TextureDiagName(stateTextureObject, record), m_pushedSyncHandle.Slot,
                             m_pushedSyncHandle.Gen);
                return nullptr;
            }
            // NO set_texture_params HAS BEEN APPLIED FOR THIS TEXTURE YET, which is not the same
            // thing as a malformed one and must not be read as one. ParamsSerial is the
            // applier's own "have I ever stored a params record here" (it is bumped on every
            // set_texture_params and starts at 0), and Params is value-initialised beside it -
            // so on a texture the application has never given a parameter to, BuiltinSampler is
            // null because the record was never written, not because a client wrote a null.
            // There is nothing to push in that state; SyncTextureParamsToBackend's own gate
            // reaches the same conclusion silently (m_syncedParamsSerial starts at 0 too and the
            // compare skips), and this decline is made silent for the same reason. Found by the
            // verification round's census: three cases were declining loudly here on textures
            // that had simply never been glTexParameter'd.
            if (record->ParamsSerial == 0) {
                MGLOG_D("Texture %u has no set_texture_params record yet, so it has no built-in "
                        "sampler to push.",
                        TextureDiagName(stateTextureObject, record));
                return nullptr;
            }
            // D-E1: kMGPipeNullHandle in a record that WAS written is Fatal{ProtocolCorruption}
            // territory - EVERY ITextureObject owns a SamplerObject, so a null is a malformed
            // record and not "no sampler". The applier is where that verdict is raised; this
            // side names it and declines, because a backend that sampled through whatever the
            // driver texture last held would be the silent half of the same bug.
            const MG_Pipe::MGPipeHandle builtin = record->Params.BuiltinSampler;
            if (MG_Pipe::MGPipeHandleIsNull(builtin)) {
                MGLOG_E_ONCE("MGPipe: texture %u's set_texture_params names the null handle as its "
                             "built-in sampler CSO; every texture owns a sampler object, so this "
                             "record is malformed - declining the built-in sampler push",
                             TextureDiagName(stateTextureObject, record));
                return nullptr;
            }
            const auto* cso = PipeSamplerCsoRecordForHandle(builtin);
            if (cso == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture %u's built-in sampler CSO {%u, %u} has no applier "
                             "record - declining the built-in sampler push",
                             TextureDiagName(stateTextureObject, record), builtin.Slot, builtin.Gen);
                return nullptr;
            }
            // THE HANDLE IS PART OF THE KEY, not just the serial. Sampler CSOs are
            // content-addressed at capacity 256 (D-F1), so two textures with identical sampling
            // share one CSO and a texture whose sampling diverges is re-pointed at a DIFFERENT
            // CSO whose serial may well be smaller than the one it left. Keying on the serial
            // alone would then skip the re-push and leave the driver texture filtering with the
            // old CSO's values for ever.
            //
            // SamplerResync is the second half of the gate and it is the one that matters more
            // than mis-filtering: ES makes a texture INCOMPLETE when its filters do not suit its
            // level set, and an incomplete texture samples (0,0,0,1) rather than its contents.
            // The server SETS it (RecreateBackendTexture) and the wire byte exists so the client
            // can force a resync it knows about; neither side clears the other's copy (D-E2), so
            // it is read here and never written back.
            if (m_syncedBuiltinSampler == builtin && m_syncedBuiltinSamplerSerial == cso->Serial &&
                record->Params.SamplerResync == 0 && !m_forceSamplerResync) {
                MGLOG_D("Sampler parameters have not changed for texture ID: %u, skipping sync.",
                        m_backendTextureId);
                return nullptr;
            }
            m_syncedBuiltinSampler = builtin;
            m_syncedBuiltinSamplerSerial = cso->Serial;
            m_forceSamplerResync = false;
            return &cso->Params;
        }

        void BackendTextureObject::SyncBuiltinSamplerToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            // P5e (tx2): NULL IS THE BY-HANDLE ARM, not an error, once a handle has been noted on
            // this twin - the record and the built-in sampler's CSO answer everything below and
            // the frontend object is not consulted at all. Without a handle it is still the
            // caller's bug it always was. P5e (fix1): and with a handle noted on an arm that
            // cannot serve it, it is Fatal{RoleViolation, "texture-handle-arm"} - see the
            // refusal's own comment beside MGB_TEXTURE_HANDLE_ARM_OFF.
            if (!stateTextureObject && MGB_TEXTURE_NULL_FRONTEND_REFUSED(*this, "SyncBuiltinSamplerToBackend")) {
                MGLOG_E_ONCE("State texture object is null, cannot sync to backend.");
                return;
            }

            // P4a (D-E1): the fifteen sampler values are SAMPLER state (GL 4.6 table 23.18) and
            // Espryt pushes them with glTexParameter* onto the TEXTURE rather than with
            // glBindSampler onto the unit - behaviour P4a preserves exactly. What changes is
            // where the values and the "did they move" answer come from.
            //
            // The whole arm choice is a preprocessor #if/#else rather than a runtime branch
            // around one shared prologue, and that is a G1 requirement (D-P): the PULL build's
            // text below the #else is the pre-P4a text token for token, so its object code
            // cannot move. Routing the pull path through a pointer it did not have before was
            // measured at -2 bytes on this function, which is a G1 failure.
            const SamplerParameters* pendingSamplerParams = nullptr;
            // P5e (tx2): resolved ONCE and shared with the target read below, which used to be
            // the frontend `GetTarget()` this arm was not allowed to make.
            const MG_Pipe::MGPipeResourceRecord* pushedDesc = nullptr;
            pushedDesc = ResolveOwnRecord(stateTextureObject);
            pendingSamplerParams = ResolvePushedBuiltinSampler(stateTextureObject, pushedDesc);
            if (pendingSamplerParams == nullptr) return;

            MGLOG_D("Syncing texture built-in sampler with backend ID %u to backend for state ID %u",
                    m_backendTextureId, TextureDiagName(stateTextureObject, pushedDesc));
            auto targetInternal = MGB_TEXPARAM_TARGET(pushedDesc, stateTextureObject);
            GLenum target = ConvertTextureTargetToBackendGLEnum(targetInternal);
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            const SamplerParameters& samplerParams = *pendingSamplerParams;
            if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                m_cacheSamplerParameters = samplerParams;
                return;
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update built-in sampler parameters
            MGLOG_D("Updating sampler parameters for texture with ID: %u", m_backendTextureId);

#define SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                  \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glTexParameteri(target, glName,                                                                    \
                                    MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));              \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
        DebugImpl::ErrorLopper::Loop(                                                                                  \
            [file = __FILE__, line = __LINE__, func = __func__,                                                        \
             t = MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName)](GLenum err) {                    \
                MGLOG_D("%s(%s:%d) ES error %s, GL_TEXTURE_MIN_FILTER = %s", func, file, line,                         \
                        MG_Util::ConvertGLEnumToString(err).c_str(), MG_Util::ConvertGLEnumToString(t).c_str());       \
            });                                                                                                        \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                                            (GLint)ResolveBackendMinFilter(samplerParams, IsAngleLlvmpipeRenderer()));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glTexParameteri(
                    target, GL_TEXTURE_MAG_FILTER,
                    (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            if (SupportsWrapR(targetInternal)) {
                SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            } else {
                m_cacheSamplerParameters.wrapR = samplerParams.wrapR;
            }
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
            if (m_cacheSamplerParameters.maxAnisotropy != samplerParams.maxAnisotropy) {
                if (g_GLESCapabilities.SupportsTextureFilterAnisotropy) {
                    g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                                samplerParams.maxAnisotropy);
                }
                // Unsupported GLES backends intentionally treat anisotropy as a
                // frontend-only no-op; remember the observed value so the cache
                // remains coherent without issuing an illegal enum every sync.
                m_cacheSamplerParameters.maxAnisotropy = samplerParams.maxAnisotropy;
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
#undef SYNC_TEX_SAMPLER_PARAM_IF_CHANGED
        }

        const MG_Pipe::MGPipeResourceRecord* BackendTextureObject::ResolvePushedTextureParams(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            // P5e (tx2): the handle the caller adopted this twin with, and only failing that the
            // client allocator - ResolveOwnRecord holds the whole of that rule.
            const auto* record = ResolveOwnRecord(stateTextureObject);
            if (record == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture %u has no applier record on the handle arm, so its "
                             "parameters cannot be pushed (handle {%u, %u})",
                             TextureDiagName(stateTextureObject, record), m_pushedSyncHandle.Slot,
                             m_pushedSyncHandle.Gen);
                return nullptr;
            }
            // ParamsSerial + ForceResync replace m_syncedTextureParamsVersion +
            // m_forceTextureParamsResync. ForceResync exists because the widened-channel
            // carrier needs a swizzle override the frontend params version does not move for;
            // it is SET by the server (RequireImageBindableStorage, RecreateBackendTexture set
            // m_forceTextureParamsResync on the twin) and by the client on the wire, and
            // NEITHER SIDE CLEARS THE OTHER'S (D-E2) - so the wire byte is read and never
            // written back, while the twin's own flag is cleared here exactly as it was before.
            if (m_syncedParamsSerial == record->ParamsSerial && record->Params.ForceResync == 0 &&
                !m_forceTextureParamsResync) {
                MGLOG_D("Texture parameters have not changed for texture ID: %u, skipping sync.",
                        m_backendTextureId);
                return nullptr;
            }
            m_syncedParamsSerial = record->ParamsSerial;
            m_forceTextureParamsResync = false;
            return record;
        }

        // P4a (D-E1): the six values SyncTextureParamsToBackend pushes, spelled once per arm.
        // MACROS, for the reason MGB_LEVEL_NEEDS_UPLOAD gives one function up: in the PULL build
        // each expands to the pre-P4a expression at its original site, so that build's tokens -
        // and its object code, whose symbol sizes G1 pins to the byte - are unchanged. Reading
        // them into locals instead was measured at +9 bytes on this function.
        //
        // Four of the six come from MGPTextureParams, which is per texture OBJECT and
        // independent of any binding - that is what closes D-E3's gap, where a texture that is
        // only a READ-framebuffer attachment reaches SyncMipmapsToBackend and nothing else. The
        // FORMAT comes from the descriptor beside them, and the BORDER comes from the built-in
        // sampler's CSO (D-F4), because a border colour is sampler state.
#define MGB_TEXPARAM_LEVEL_RANGE                                                                                       \
    (pushedRecord != nullptr ? UintVec2{static_cast<Uint>(pushedRecord->Params.BaseLevel),                              \
                                        static_cast<Uint>(pushedRecord->Params.MaxLevel)}                              \
                             : stateTextureObject->GetLevelRange())
#define MGB_TEXPARAM_FORMAT                                                                                            \
    (pushedRecord != nullptr ? static_cast<TextureInternalFormat>(pushedRecord->Desc.InternalFormat)                    \
                             : stateTextureObject->GetFormat())
#define MGB_TEXPARAM_SWIZZLE                                                                                           \
    (pushedRecord != nullptr                                                                                           \
         ? Vec4<TextureSwizzleParam>{static_cast<TextureSwizzleParam>(pushedRecord->Params.Swizzle[0]),                 \
                                     static_cast<TextureSwizzleParam>(pushedRecord->Params.Swizzle[1]),                 \
                                     static_cast<TextureSwizzleParam>(pushedRecord->Params.Swizzle[2]),                 \
                                     static_cast<TextureSwizzleParam>(pushedRecord->Params.Swizzle[3])}                 \
         : stateTextureObject->GetAllSwizzleParams())
// The contract now spells this encoding (c0c, ID-12 DV-2): kMGPipeDepthStencilModeDepth = 0 =
// GL_DEPTH_COMPONENT, kMGPipeDepthStencilModeStencil = 1 = GL_STENCIL_INDEX. v1 open-coded the
// direction as a bare `!= 0` and recorded it as a deviation; the constants are read here instead,
// so a future re-numbering moves both sides at once. ZERO MEANS THE DEFAULT is still what decides
// the direction: GL_DEPTH_COMPONENT is the GL and ES default and a texture that never asks for
// the stencil aspect never emits the call, so a zeroed record has to decode to exactly what such
// a texture already has.
#define MGB_TEXPARAM_DS_MODE                                                                                           \
    (pushedRecord != nullptr                                                                                           \
         ? (pushedRecord->Params.DepthStencilMode == MG_Pipe::kMGPipeDepthStencilModeStencil ? GL_STENCIL_INDEX        \
                                                                                            : GL_DEPTH_COMPONENT)      \
         : stateTextureObject->GetDepthStencilTextureMode())
#define MGB_TEXPARAM_BORDER_F (pushedBorder != nullptr ? pushedBorder->borderColor : stateTextureObject->GetBorderColor())
#define MGB_TEXPARAM_BORDER_I                                                                                          \
    (pushedBorder != nullptr ? pushedBorder->borderColorI : stateTextureObject->GetBorderColorI())
#define MGB_TEXPARAM_BORDER_UI                                                                                         \
    (pushedBorder != nullptr ? pushedBorder->borderColorUI : stateTextureObject->GetBorderColorUI())
#define MGB_TEXPARAM_BORDER_FORM                                                                                       \
    (pushedBorder != nullptr ? pushedBorder->borderColorForm : stateTextureObject->GetBorderColorForm())

        void BackendTextureObject::SyncTextureParamsToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            // P5e (tx2): null IS the by-handle arm once a handle has been noted on this twin;
            // see the twin note in SyncBuiltinSamplerToBackend.
            if (!stateTextureObject && MGB_TEXTURE_NULL_FRONTEND_REFUSED(*this, "SyncTextureParamsToBackend")) {
                MGLOG_E_ONCE("State texture object is null, cannot sync to backend.");
                return;
            }

            // P4a (D-E1/D-E3): where the parameter values come from, and what says they moved.
            // Null on the handle arm means "nothing to do" and is NEVER a fall-back to the
            // frontend; the legacy arm keeps the params-version pair verbatim, token for token,
            // because the PULL build's object code may not move (D-P).
            const MG_Pipe::MGPipeResourceRecord* pushedRecord = nullptr;
            const SamplerParameters* pushedBorder = nullptr;
            // Whether the four border comparands can be READ at all this sync. On the handle arm
            // a missing sampler CSO makes them unreadable and the border block below is skipped
            // with the refusal already named; on the legacy arm they always come from the
            // frontend object, so it is constant true and folds away.
            Bool borderColorReadable = true;
            pushedRecord = ResolvePushedTextureParams(stateTextureObject);
            if (pushedRecord == nullptr) return;
            // D-F4: the border colour is SAMPLER state (GL 4.6 table 23.18) and lives on the
            // built-in sampler's CSO, not on MGPTextureParams - P4a deliberately does not
            // put SamplerParameters on the wire twice. All FOUR comparands (float, int,
            // uint, form) still cross and are still all compared below, because two integer
            // borders differing above 2^24 share one float and a Float -> Int transition can
            // leave every number unchanged while needing a different driver entry point.
            const auto* borderCso = PipeSamplerCsoRecordForHandle(pushedRecord->Params.BuiltinSampler);
            if (borderCso != nullptr) {
                pushedBorder = &borderCso->Params;
            } else {
                borderColorReadable = false;
                MGLOG_E_ONCE("MGPipe: texture %u's built-in sampler CSO {%u, %u} has no applier "
                             "record, so its border colour cannot be pushed",
                             TextureDiagName(stateTextureObject, pushedRecord),
                             pushedRecord->Params.BuiltinSampler.Slot,
                             pushedRecord->Params.BuiltinSampler.Gen);
            }

            MGLOG_D("Syncing texture params with backend ID %u to backend for state ID %u", m_backendTextureId,
                    TextureDiagName(stateTextureObject, pushedRecord));

            auto targetInternal = MGB_TEXPARAM_TARGET(pushedRecord, stateTextureObject);
            GLenum target = ConvertTextureTargetToBackendGLEnum(targetInternal);
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // Multisample targets reject the *sampler* parameters (LOD range, border color) but
            // GL_TEXTURE_SWIZZLE_* is texture state, not sampler state, and ES accepts it on them.
            // Bailing out entirely used to drop every swizzle write on the floor, which is what the
            // frontend already assumes is legal (see GL_Texture.cpp's MS-invalid pname list, which
            // deliberately omits the swizzle enums). Note the caches for the skipped parameters are
            // still refreshed so they never look stale, but m_cacheSwizzleParams must NOT be, or the
            // change detection below would swallow the very writes we came here to emit.
            const Bool isMultisampleTarget = TextureImpl::IsMultisampleTextureTarget(targetInternal);
            if (isMultisampleTarget) {
                m_cacheLodRange = MGB_TEXPARAM_LEVEL_RANGE;
                if (borderColorReadable) m_cacheBorderColor = MGB_TEXPARAM_BORDER_F;
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update texture parameters
            MGLOG_D("Updating texture parameters for texture with ID: %u", m_backendTextureId);

            const auto& levelRange = MGB_TEXPARAM_LEVEL_RANGE;

            if (!isMultisampleTarget && m_cacheLodRange.x() != levelRange.x()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, static_cast<GLint>(levelRange.x()));
                m_cacheLodRange.x() = levelRange.x();
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            if (!isMultisampleTarget && m_cacheLodRange.y() != levelRange.y()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levelRange.y()));
                m_cacheLodRange.y() = levelRange.y();
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // A three-channel format widened to four to keep the image colour-renderable (see
            // NormalizePixelFormat) gains an alpha channel the frontend format does not have, and
            // whatever the draw that filled it wrote there is not what GL would report: a format
            // without alpha reads back as 1.0. Answer the ALPHA swizzle source with ONE so the
            // promotion stays invisible, composed with the swizzle the application asked for.
            Vec4<TextureSwizzleParam> swizzleParams = MGB_TEXPARAM_SWIZZLE;
            if (TextureImpl::BackendTextureFormatAddsAlpha(MGB_TEXPARAM_FORMAT, targetInternal)) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    if (swizzleParams[channel] == TextureSwizzleParam::Alpha) {
                        swizzleParams[channel] = TextureSwizzleParam::One;
                    }
                }
            }
            // The same composition for the image-format widening, which can add TWO or THREE
            // channels rather than one (GL_R8UI carried in a GL_RGBA8UI). GL reads a channel the
            // format does not have as 0, except alpha, which reads as 1 - so a sampler must see
            // those constants and not whatever the widened storage holds. The upload and the
            // shader's own store mask already keep them at exactly these values; this covers
            // storage nothing has written yet (glTexStorage with no upload), whose surplus
            // channels are undefined. Composed with the application's own swizzle for the same
            // reason as the alpha case above: GL_TEXTURE_SWIZZLE names a SOURCE channel of the
            // logical texel, so it is the source that is substituted, never the destination.
            if (const auto imageWidening =
                    m_imageBindableStorageRequired
                        ? TextureImpl::GetImageBindableStorageWidening(MGB_TEXPARAM_FORMAT)
                        : TextureImpl::ImageBindableStorageWidening{}) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    switch (swizzleParams[channel]) {
                    case TextureSwizzleParam::Green:
                        if (imageWidening.SourceChannels < 2) swizzleParams[channel] = TextureSwizzleParam::Zero;
                        break;
                    case TextureSwizzleParam::Blue:
                        if (imageWidening.SourceChannels < 3) swizzleParams[channel] = TextureSwizzleParam::Zero;
                        break;
                    case TextureSwizzleParam::Alpha:
                        if (imageWidening.SourceChannels < 4) swizzleParams[channel] = TextureSwizzleParam::One;
                        break;
                    default:
                        break;
                    }
                }
            }
            if (swizzleParams != m_cacheSwizzleParams) {
#define SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(func, glEnum)                                                                \
    if (m_cacheSwizzleParams.func != swizzleParams.func) {                                                             \
        g_GLESFuncs.glTexParameteri(target, glEnum, MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams.func));  \
        m_cacheSwizzleParams.func = swizzleParams.func;                                                                \
    }
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(r(), GL_TEXTURE_SWIZZLE_R);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(g(), GL_TEXTURE_SWIZZLE_G);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(b(), GL_TEXTURE_SWIZZLE_B);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(a(), GL_TEXTURE_SWIZZLE_A);
#undef SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED
                m_cacheSwizzleParams = swizzleParams;
                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }

            // GL_TEXTURE_BORDER_COLOR needs ES 3.2 or EXT/OES_texture_border_clamp; on a driver
            // without it every such call is INVALID_ENUM, so the parameter is simply not synced.
            //
            // The FORM has to be forwarded along with the value. A border colour set through
            // glTexParameterIiv/Iuiv is an integer one, and an isampler2D/usampler2D fetch of the
            // border returns whatever the driver's integer border register holds - so pushing it
            // through glTexParameterfv handed the driver float 255.0 and the shader read back
            // 1132396544, the bit pattern of that float. glTexParameterIiv/Iuiv are ES 3.2 core
            // beside GL_TEXTURE_BORDER_COLOR itself, so they sit behind the same capability gate;
            // the entry-point null check covers a driver that advertises the extension without them.
            // The redundancy filter has to look at the AUTHORITATIVE representation, not just the
            // float one: two integer borders that differ above 2^24 (16777216 and 16777217, say)
            // collapse onto the same float, so a float-only comparison would skip the second sync and
            // leave the driver holding the first value forever.
            // m-3: GATED, because on the handle arm an unreadable border means the CSO record is
            // missing and the block below is skipped with the refusal already named - so v1's
            // unconditional initialiser was a live frontend read on a switched-over path whose
            // value nothing then used. The value a skipped block sees is the cache's own, which
            // cannot make the condition true.
            const auto borderColorForm = borderColorReadable ? MGB_TEXPARAM_BORDER_FORM : m_cacheBorderColorForm;
            if (!isMultisampleTarget &&
                borderColorReadable &&
                g_GLESCapabilities.SupportsTextureBorderClamp &&
                (m_cacheBorderColor != MGB_TEXPARAM_BORDER_F ||
                 m_cacheBorderColorI != MGB_TEXPARAM_BORDER_I ||
                 m_cacheBorderColorUI != MGB_TEXPARAM_BORDER_UI ||
                 m_cacheBorderColorForm != borderColorForm)) {
                if (borderColorForm == BorderColorForm::Int && g_GLESFuncs.glTexParameterIiv) {
                    const auto& borderColorI = MGB_TEXPARAM_BORDER_I;
                    const GLint borderColorArray[4] = {borderColorI.x(), borderColorI.y(), borderColorI.z(),
                                                       borderColorI.w()};
                    g_GLESFuncs.glTexParameterIiv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                } else if (borderColorForm == BorderColorForm::Uint && g_GLESFuncs.glTexParameterIuiv) {
                    const auto& borderColorUI = MGB_TEXPARAM_BORDER_UI;
                    const GLuint borderColorArray[4] = {borderColorUI.x(), borderColorUI.y(), borderColorUI.z(),
                                                        borderColorUI.w()};
                    g_GLESFuncs.glTexParameterIuiv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                } else {
                    const auto& borderColor = MGB_TEXPARAM_BORDER_F;
                    const GLfloat borderColorArray[4] = {borderColor.x(), borderColor.y(), borderColor.z(),
                                                         borderColor.w()};
                    g_GLESFuncs.glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                }
                m_cacheBorderColor = MGB_TEXPARAM_BORDER_F;
                m_cacheBorderColorI = MGB_TEXPARAM_BORDER_I;
                m_cacheBorderColorUI = MGB_TEXPARAM_BORDER_UI;
                m_cacheBorderColorForm = borderColorForm;
                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }

            // GL_DEPTH_STENCIL_TEXTURE_MODE (GL_ARB_stencil_texturing / ES 3.1 core): which aspect
            // of a packed depth/stencil image a sampler reads. Until this was forwarded the
            // frontend kept the mode as a pure shadow - glGetTexParameter answered it, sampling
            // ignored it - so a usampler2D bound to a D24S8 texture in STENCIL_INDEX mode read the
            // depth aspect. Texture state rather than sampler state, so multisample targets take
            // it too (ES 3.1 8.10 lists it among the three pnames they accept). It is only sent
            // when it has moved, which for the overwhelming majority of textures is never.
            const Bool supportsStencilTextureMode =
                g_GLESCapabilities.GLESVersion.Major > 3 ||
                (g_GLESCapabilities.GLESVersion.Major == 3 && g_GLESCapabilities.GLESVersion.Minor >= 1);
            if (supportsStencilTextureMode) {
                const GLenum depthStencilTextureMode = MGB_TEXPARAM_DS_MODE;
                if (m_cacheDepthStencilTextureMode != depthStencilTextureMode) {
                    g_GLESFuncs.glTexParameteri(target, GL_DEPTH_STENCIL_TEXTURE_MODE,
                                                static_cast<GLint>(depthStencilTextureMode));
                    m_cacheDepthStencilTextureMode = depthStencilTextureMode;
                    DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                        MGLOG_D("%s(%s:%d) ES error %s", func, file, line,
                                MG_Util::ConvertGLEnumToString(err).c_str());
                    });
                }
            }
        }
#undef MGB_TEXPARAM_LEVEL_RANGE
#undef MGB_TEXPARAM_FORMAT
#undef MGB_TEXPARAM_SWIZZLE
#undef MGB_TEXPARAM_DS_MODE
#undef MGB_TEXPARAM_BORDER_F
#undef MGB_TEXPARAM_BORDER_I
#undef MGB_TEXPARAM_BORDER_UI
#undef MGB_TEXPARAM_BORDER_FORM

        void ActivateTextureUnit(Uint unit) {
            if (unit == g_activeTextureUnit) {
                return;
            }
            g_GLESFuncs.glActiveTexture(GL_TEXTURE0 + unit);
            g_activeTextureUnit = unit;
        }

        void UnbindTexture(Uint unit, GLenum target) { // Activates `unit` when an unbind is issued
            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (g_boundTexturesCache[unit][targetN] == nullptr) return;

            ActivateTextureUnit(unit);
            g_GLESFuncs.glBindTexture(target, 0);
            g_boundTexturesCache[unit][targetN] = nullptr;
        }

        Uint g_activeTextureUnit = 0;
        Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
              MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;
        TwinRegistry<MG_State::GLState::ITextureObject, BackendTextureObject, MG_Pipe::MGPipeKind::Texture> g_backendTextureObjects;

#if MOBILEGL_BUILD_DISAGGREGATED
        void WritePendingSharedImageUploads() {
            auto& applier = MG_Pipe::MGPipeApplier();
            for (SizeT slot = 0; slot < applier.TextureResources.size(); ++slot) {
                const auto& record = applier.TextureResources[slot];
                if (!record.Live || record.SharedImageId == 0 || record.PendingUploads.empty()) continue;
                const MG_Pipe::MGPipeHandle handle{static_cast<Uint32>(slot), record.Gen};
                auto* twin = g_backendTextureObjects.FindByHandle(handle);
                if (twin == nullptr || !*twin) continue;
                (*twin)->SyncMipmapsToBackendByHandle(handle);
            }
        }
#endif

        // P5e (id), CONTRACT-P5E §4.1. See Managers.h for the contract.
        BackendTextureObject* ResolveTextureTwin(MG_Pipe::MGPipeHandle res) {
            if (MG_Pipe::MGPipeHandleIsNull(res)) return nullptr;
            // THE RECORD FIRST: a texture handle with no resource record is a seam defect - the
            // client named a texture it never created, or one whose resource_destroy has
            // already applied while a binding still names it - and a twin adopted for it would
            // own a driver texture nothing ever uploads to.
            if (PipeTextureRecordForHandle(res) == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture {%u, %u} has no applier resource record on the handle "
                             "arm, so no driver texture can be built for it and the unit keeps what "
                             "it holds",
                             res.Slot, res.Gen);
                return nullptr;
            }
            auto* slot = AdoptTwinByHandle(g_backendTextureObjects, res, "texture");
            if (slot == nullptr) return nullptr;
            if (!*slot) *slot = MakeShared<BackendTextureObject>();
            return slot->get();
        }

        void NoteDriverSideTextureWriteByHandle(MG_Pipe::MGPipeHandle res, Bool storeFollowsTheWrite) {
            // The storage owner's handle, one hop per view like the frontend walk above (the
            // client composes a view of a view onto its root, so one hop is the real shape).
            MG_Pipe::MGPipeHandle storage = res;
            for (Uint hop = 0; hop < 4 && !MG_Pipe::MGPipeHandleIsNull(storage); ++hop) {
                const auto* record = PipeTextureRecordForHandle(storage);
                if (record == nullptr || MG_Pipe::MGPipeHandleIsNull(record->Desc.ViewOf)) break;
                storage = record->Desc.ViewOf;
            }
            if (auto* twin = ResolveTextureTwin(storage)) twin->NoteDriverSideWrite();
            // P8-E: the staged store stops standing for this texture's content (a no-op on a
            // monolith arm, where the store does not copy).
            if (!storeFollowsTheWrite && !MG_Pipe::MGPipeHandleIsNull(storage)) {
                MG_Record::ServerStagedTexture().MarkDriverWritten(
                    MG_Record::StagedTextureStore::KeyForHandle(storage));
            }
        }
        Bool FollowCopyImageInStagedStore(MG_Pipe::MGPipeHandle source, Uint32 sourceLevel, const IntVec3& sourceOrigin,
                                          MG_Pipe::MGPipeHandle destination, Uint32 destinationLevel,
                                          const IntVec3& destinationOrigin, const IntVec3& size) {
            auto& store = MG_Record::ServerStagedTexture();
            // P13 W4b: an aliasing store (monolith's record arm) follows too; see FollowCopy.
            if (!store.Holds()) return false;
            // The store's level key for an endpoint copy-image addresses as ONE image with its
            // layers on z: exactly one upload target (not a cube map, whose faces are six store
            // levels) and not a 1D array (the store keeps its layers as rows). A view's own levels
            // are byteless, so FollowCopy answers false for it by itself.
            const auto storeTarget = [](MG_Pipe::MGPipeHandle handle, Uint16& target) {
                const auto* record = PipeTextureRecordForHandle(handle);
                if (record == nullptr ||
                    record->Desc.Target == static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex1DArray)) {
                    return false;
                }
                const auto& targets = BufferImpl::StagedUploadTargetsForPipeTarget(record->Desc.Target);
                if (targets.size() != 1) return false;
                target = static_cast<Uint16>(targets[0]);
                return true;
            };
            Uint16 sourceTarget = 0, destinationTarget = 0;
            if (!storeTarget(source, sourceTarget) || !storeTarget(destination, destinationTarget)) return false;
            // ONE FORMAT BOTH ENDS. Copy-image moves the GL texel bits; the store holds each
            // format's canonical shadow, which for most formats IS those bits and for some is not
            // (the 16-bit packed formats are kept as UNorm8, RGB10 / RGB12 as UNorm16). Between two
            // levels of one format the canonical move and the GL move agree whatever the format;
            // across two compatible formats they need not, so that copy is not followed.
            if (PipeTextureRecordForHandle(source)->Desc.InternalFormat !=
                PipeTextureRecordForHandle(destination)->Desc.InternalFormat) {
                return false;
            }
            return store.FollowCopy(MG_Record::StagedTextureStore::KeyForHandle(source), sourceTarget,
                                    static_cast<Uint16>(sourceLevel), sourceOrigin,
                                    MG_Record::StagedTextureStore::KeyForHandle(destination),
                                    destinationTarget, static_cast<Uint16>(destinationLevel), destinationOrigin,
                                    size);
        }
    } // namespace TextureImpl

    // =====================================================================================
    // P5e SEAMS: DECLARED AND REFUSED HERE, BODIED BY THE FAMILY PACKAGES
    // (docs/archive/contracts/CONTRACT-P5E.md §4.2; BRIEF-P5E §1's "each such seam is a declared signature
    // in c0e so both sides compile from day one")
    // =====================================================================================
    //
    // WHY THEY ARE ALL IN ONE BLOCK AND WHY THE BODIES ABORT. P5e lands as eight packages in
    // eight parallel worktrees, and five of them are on opposite sides of these calls: tx2
    // writes SyncTextureToBackendByHandle and SyncMipmapsToBackendByHandle while fb's
    // attachment sync and image sweep CALL them; fb writes the two SyncToBackendByHandle
    // overloads while tx2's detach walk needs the reverse index behind them; id rekeys the
    // registries the three resolvers read. A package that had to add its own declaration would
    // collide with the package that added the other half, which is the merge trap P4a's
    // contract package was written to avoid.
    //
    // So the signature is fixed HERE, once, and each family's commit replaces a body. The body
    // ABORTS BY NAME rather than returning null or doing nothing: a resolver that answered null
    // would render a blank draw and a sync that did nothing would render stale pixels, and both
    // would be green lanes. Nothing calls any of them at this commit - every call site is still
    // on the frontend overload beside it - so the abort is a link-time seam, not a runtime one.
    //
    // A package REPLACES the body in place and deletes the matching comment; it does not add a
    // second definition elsewhere, or the linker's answer depends on link order.

    namespace TextureImpl {
        // ---- P5e (tx2): THE TEXTURE FAMILY'S BY-HANDLE ENTRY, CONTRACT-P5E §5.2 --------------
        //
        // SamplerImpl::ResolveSamplerCsoTwin is the shape (Managers.cpp, the sampler half was
        // already run-ahead clean on its record arm and this is that shape brought to the texture
        // half): RECORD FIRST, twin by GetOrCreateByHandle, three syncs driven from the record,
        // no frontend touch and no allocator probe anywhere on the path.
        //
        // WHAT THE FRONTEND ENTRY PAYS AND THIS ONE DOES NOT. SyncTextureObjectToBackend takes a
        // by-VALUE copy of the twin and does a second Find at the tail, because its registry's
        // Find returns a reference INTO an open-addressed map that a NESTED sync (a view syncing
        // its storage texture) can rehash out from under it. A slot-table entry is an ARRAY
        // ELEMENT: a nested GetOrCreateByHandle can only grow the vector, so the answer is
        // re-INDEXED at the tail rather than pinned by a refcount, and the copy is a refcount
        // this arm does not pay.
        //
        // A null return is a NAMED decline and never a mint: no record (the client named a
        // texture it never created, or one whose resource_destroy has already applied), or a slot
        // whose live generation is ahead of the handle's. A caller binds nothing and the unit
        // keeps what it holds.
        SharedPtr<BackendTextureObject>& SyncTextureToBackendByHandle(MG_Pipe::MGPipeHandle texture,
                                                                      Bool imageBindableStorageRequired) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // The one storage every decline arm can return a reference to. Never written.
            static SharedPtr<BackendTextureObject> s_noTwin{};
            s_noTwin = nullptr;

            const auto* record = PipeTextureRecordForHandle(texture);
            if (record == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture {%u, %u} has no applier resource record on the handle "
                             "arm, so no driver texture can be built for it and the caller keeps "
                             "what it holds",
                             texture.Slot, texture.Gen);
                return s_noTwin;
            }
            auto* slot = g_backendTextureObjects.GetOrCreateByHandle(texture);
            if (slot == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture {%u, %u} cannot be adopted on the handle arm (the slot's "
                             "live generation is %u), so no driver texture is built for it",
                             texture.Slot, texture.Gen, g_backendTextureObjects.LiveGenAt(texture.Slot));
                return s_noTwin;
            }
            if (!*slot) *slot = MakeShared<BackendTextureObject>();
            // THE HANDLE IS NOTED BEFORE ANY SYNC RUNS. It is what the three prologues read
            // instead of probing the client allocator, and what selects the by-handle arm inside
            // them, so it has to be in place before the first of them is called.
            (*slot)->NotePushedSyncHandle(texture);

            const SharedPtr<BackendTextureObject> twin = *slot;
            if (imageBindableStorageRequired) {
                twin->RequireImageBindableStorageByHandle(texture, *record);
            }
            static const SharedPtr<MG_State::GLState::ITextureObject> kNoFrontendTexture{};
            twin->SyncTextureParamsToBackend(kNoFrontendTexture);
            twin->SyncBuiltinSamplerToBackend(kNoFrontendTexture);
            twin->SyncMipmapsToBackend(kNoFrontendTexture);
            // The storage sync may RE-MINT the driver texture, which discards every parameter the
            // two calls above just pushed - the frontend entry's argument, unchanged.
            if (twin->NeedsParameterResync()) {
                twin->SyncTextureParamsToBackend(kNoFrontendTexture);
                twin->SyncBuiltinSamplerToBackend(kNoFrontendTexture);
            }
            // ONE re-index, and only because a nested adopt may have GROWN the vector; nothing on
            // this arm erases a live slot, so the entry itself cannot have gone.
            auto* refreshed = g_backendTextureObjects.FindByHandle(texture);
            if (refreshed != nullptr && *refreshed) {
                return *refreshed;
            }
            s_noTwin = twin;
            return s_noTwin;
        }

        void BackendTextureObject::SyncMipmapsToBackendByHandle(MG_Pipe::MGPipeHandle texture) {
            // The STORAGE half alone, for fb's attachment sync and the image sweep: an attachment
            // needs its levels on the driver, not its sampler parameters (§5.4). The handle note
            // is what routes the body below onto the record.
            NotePushedSyncHandle(texture);
            static const SharedPtr<MG_State::GLState::ITextureObject> kNoFrontendTexture{};
            SyncMipmapsToBackend(kNoFrontendTexture);
        }

        // P5e (tx2), CONTRACT-P5E §5.2. See the declaration for the clause-by-clause derivation
        // and for why BOTH halves of each resync pair are read here.
        Bool BackendTextureObject::IsDrawSyncCleanByRecord(MG_Pipe::MGPipeHandle res,
                                                           const MG_Pipe::MGPipeResourceRecord& record) const {
            (void)res;
            // P3b/P4b D3: THE TWO STORAGE CLAUSES ARE ASKED OF THE STORAGE RECORD, which is this
            // record for every texture that owns its texels and the record named by Desc.ViewOf
            // for a glTextureView. A view's own Serial is moved by nothing an upload does - the
            // client keys every resource_subdata on the storage owner, and the applier moves that
            // record's Serial and PendingUploads alone - so reading the view's here answered
            // "clean" for every owner write after the view's first sample. The three PARAMETER
            // clauses below stay this record's: a view has its own texture parameters and its own
            // built-in sampler, which is the whole point of the name existing. See
            // PipeTextureStorageRecordForRecord and CONTRACT-P5E.md §5.2.
            const MG_Pipe::MGPipeResourceRecord* storage = PipeTextureStorageRecordForRecord(record);
            if (storage == nullptr) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
            // Never clean while bound to a shared image: see IsDrawSyncClean.
            if (m_sharedImage != nullptr) return false;
#endif
            // SyncMipmapsToBackend's own early-out, verbatim.
            if (!m_isInitialized || m_syncedResourceSerial == 0 || m_syncedResourceSerial != storage->Serial ||
                !storage->PendingUploads.empty()) {
                return false;
            }
            // SyncTextureParamsToBackend's (ResolvePushedTextureParams' gate).
            if (m_syncedParamsSerial != record.ParamsSerial || record.Params.ForceResync != 0 ||
                m_forceTextureParamsResync) {
                return false;
            }
            // SyncBuiltinSamplerToBackend's (ResolvePushedBuiltinSampler's gate). A texture whose
            // params have never been set has no built-in sampler to push and that prologue
            // declines SILENTLY - so a zero ParamsSerial is clean here for the same reason, and
            // the CSO lookup below is only made when there is a CSO to look up.
            if (record.ParamsSerial != 0) {
                const MG_Pipe::MGPipeHandle builtin = record.Params.BuiltinSampler;
                if (m_syncedBuiltinSampler != builtin) return false;
                const auto* cso = PipeSamplerCsoRecordForHandle(builtin);
                if (cso == nullptr || m_syncedBuiltinSamplerSerial != cso->Serial) return false;
                if (record.Params.SamplerResync != 0 || m_forceSamplerResync) return false;
            } else if (m_forceSamplerResync) {
                return false;
            }
            // The storage-kind restriction the frontend gate carries, from the descriptor: a
            // buffer texture's backing store can move without any serial above noticing.
            return static_cast<TextureStorageType>(record.Desc.StorageKind) == TextureStorageType::Mipmap;
        }
    } // namespace TextureImpl

    // FramebufferImpl::BackendFramebufferObject::SyncToBackendByHandle and
    // RenderbufferImpl::BackendRenderbufferObject::SyncToBackendByHandle LANDED with package fb
    // (P5e); their bodies are beside the object forms they overload, where the attachment walk
    // and the storage allocation they share can be read side by side.

    // PrgramImpl's seam is GONE, not stubbed: package pg wrote
    // BackendProgramObjectImpl::SyncToBackendByHandle's real body beside the frontend overload
    // it is the twin of (above, with the two ProgramArchiveSource heads). A second definition
    // here would make the linker's answer depend on link order, which is the one failure this
    // block's own comment was written to prevent.
} // namespace MobileGL::MG_Backend::DirectGLES
