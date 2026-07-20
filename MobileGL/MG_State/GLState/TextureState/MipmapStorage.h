// MobileGL - MobileGL/MG_State/GLState/TextureState/MipmapStorage.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureEnum.h"
#include "MG_Util/Types.h"
#include "MG_Util/Math/VectorTypes.h"
#include "TextureTypes.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
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

            protected:
                Vector<IntVec3> m_texelSizes;
                Vector<Vector<Uint8>> m_data;
                Vector<bool> m_isDirty;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
