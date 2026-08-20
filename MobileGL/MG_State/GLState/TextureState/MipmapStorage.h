// MobileGL - MobileGL/MG_State/GLState/TextureState/MipmapStorage.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <algorithm>

#include "TextureEnum.h"
#include "MG_Util/Types.h"
#include "MG_Util/Math/VectorTypes.h"
#include "TextureTypes.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            // Texel-space bounding box of the shadow bytes a backend has not uploaded
            // yet, [lo, hi) per axis. Cleared (all zero) while the level is clean. A
            // box, not a range list: repeated sub-image writes union into one region,
            // which stays exact for the per-frame "small sub-rect of a big atlas"
            // pattern this exists for, and degrades to the old full-level upload as
            // the union grows.
            struct MipmapDirtyRegion {
                IntVec3 lo{0, 0, 0};
                IntVec3 hi{0, 0, 0};
                Bool Empty() const { return hi.x() <= lo.x() || hi.y() <= lo.y() || hi.z() <= lo.z(); }
                Bool CoversWholeLevel(const IntVec3& levelSize) const {
                    return lo.x() <= 0 && lo.y() <= 0 && lo.z() <= 0 && hi.x() >= levelSize.x() &&
                           hi.y() >= levelSize.y() && hi.z() >= std::max(levelSize.z(), 1);
                }
                SizeT TexelCount() const {
                    if (Empty()) return 0;
                    return static_cast<SizeT>(hi.x() - lo.x()) * static_cast<SizeT>(hi.y() - lo.y()) *
                           static_cast<SizeT>(hi.z() - lo.z());
                }
            };

            class MipmapStorage {
            public:
                SizeT GetLevelCount() const;
                void AllocateLevel(Uint level, MipmapInput input);
                // Discard every level at or above levelCount. AllocateLevel never shrinks, so this
                // is the only way a chain gets shorter - use it where the caller defines the whole
                // level set (glTexStorage*, mip regeneration, atlas respecification).
                void TruncateToLevelCount(SizeT levelCount);
                void UpdateSubData(Uint level, DataPtr input);
                void* MapData(Uint level);
                IntVec3 GetTexelSize(Uint level) const;
                SizeT GetByteSize(Uint level) const;
                void MarkDirty(Uint level, bool dirty);
                bool IsDirty(Uint level) const;
                // Union a sub-image write's box into the level's pending region and set the
                // dirty flag. MarkDirty keeps its meaning: true covers the whole level,
                // false clears the region along with the flag.
                void MarkDirtyRegion(Uint level, IntVec3 offset, IntVec3 size);
                // Meaningful only while IsDirty(level).
                MipmapDirtyRegion GetDirtyRegion(Uint level) const;

                // Behind the union box, the level keeps up to kMaxDirtyRects pairwise
                // disjoint rects recording WHERE the writes actually landed. A frame of
                // ~100 scattered sprite updates in a big atlas has a union box that
                // covers nearly the whole level while the touched texels are ~5% of it;
                // the union box stays the source of truth (every write funnels through
                // MarkDirty/MarkDirtyRegion into BOTH representations), backends OPT IN
                // to the list purely as an upload-size refinement. 96 slots because the
                // pattern this exists for is Minecraft's ~100 sprites/frame: a 16-slot
                // list forced into far-apart merges was measured at >90% of the union
                // box's area on exactly that pattern, i.e. worthless. Inserts merge any
                // touching/overlapping rect (cascading, so the list stays disjoint);
                // when full, the incoming rect folds into the neighbour whose box grows
                // least and the list degrades gracefully toward the union box.
                static constexpr SizeT kMaxDirtyRects = 96;
                // Copies the level's dirty rects into outRects and returns how many were
                // written. 0 means "upload the union box instead" and covers every
                // reason at once: tracking unavailable, a single rect (identical to the
                // union box by construction), more rects than maxRects, or a summed
                // area so close to the union box's that one big upload beats many small
                // ones (fewer driver calls wins when the bytes are nearly equal).
                SizeT GetDirtyRects(Uint level, MipmapDirtyRegion* outRects, SizeT maxRects) const;

                // The bytes an application handed to glCompressedTexImage*, kept verbatim beside the
                // (uncompressed) texel shadow rather than in place of it. GL 4.6 core 8.11 requires
                // glGetCompressedTexImage to return the image *as stored*, and no backend here has a
                // BC/ETC codec, so a re-encode could never be byte-exact; at the same time m_data has
                // to keep the "width * height * bytes-per-texel" layout that the backend upload
                // sizing, glGenerateMipmap's bytes-per-texel division and the pixel-store packer all
                // divide by. Two parallel vectors, one invariant preserved. Call order is
                // AllocateLevel then SetCompressedImage - AllocateLevel clears the tag, so a plain
                // glTexImage2D over the level un-compresses it.
                void SetCompressedImage(Uint level, GLenum internalFormat, const void* data, SizeT size);
                // GL_NONE when the level is not stored compressed.
                GLenum GetCompressedFormat(Uint level) const;
                SizeT GetCompressedByteSize(Uint level) const;
                const void* MapCompressedData(Uint level) const;

                // The compressed internalformat the application ASKED for, which is not the same
                // question as the one above: the six generic GL_COMPRESSED_* enums let the
                // implementation choose, MobileGL chooses uncompressed storage, and the level is
                // deliberately left untagged so GL_TEXTURE_COMPRESSED keeps answering false and
                // glGetCompressedTexImage is not handed a blob nothing ever compressed. The entry
                // points that must refuse a compressed image outright (glClearTexImage /
                // glClearTexSubImage, GL 4.6 core 8.19) still need to know, so the request is
                // recorded separately. Set right after AllocateLevel, which clears it.
                void SetRequestedCompressedFormat(Uint level, GLenum internalFormat);
                // GL_NONE when the level was not requested with a compressed internalformat.
                GLenum GetRequestedCompressedFormat(Uint level) const;

            protected:
                // Insert one clamped, non-empty write box, keeping the list disjoint
                // and bounded (see kMaxDirtyRects).
                void InsertDirtyRect(Uint level, MipmapDirtyRegion incoming);

                Vector<IntVec3> m_texelSizes;
                Vector<Vector<Uint8>> m_data;
                Vector<bool> m_isDirty;
                Vector<MipmapDirtyRegion> m_dirtyRegions;
                // Per level, the disjoint rect list behind m_dirtyRegions' union box.
                // An EMPTY list is the common resting state and always means "the union
                // box is the whole story" - clean levels, whole-level dirties and
                // respecifies all just clear it, so plain full-level uploads never pay
                // a heap allocation; the first scattered MarkDirtyRegion on an
                // already-dirty level seeds the list from the union box accumulated so
                // far and refines from there.
                Vector<Vector<MipmapDirtyRegion>> m_dirtyRects;
                Vector<Vector<Uint8>> m_compressedData;
                Vector<GLenum> m_compressedFormats;
                Vector<GLenum> m_requestedCompressedFormats;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
