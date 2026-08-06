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

            protected:
                Vector<IntVec3> m_texelSizes;
                Vector<Vector<Uint8>> m_data;
                Vector<bool> m_isDirty;
                Vector<MipmapDirtyRegion> m_dirtyRegions;
                Vector<Vector<Uint8>> m_compressedData;
                Vector<GLenum> m_compressedFormats;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
