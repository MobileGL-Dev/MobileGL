// MobileGL - MobileGL/MG_State/GLState/TextureState/MipmapUploadTargetArray.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "MipmapStorage.h"
#include "TextureEnum.h"
#include "MG_Util/Types.h"
#include "MG_Util/Math/VectorTypes.h"
#include "TextureTypes.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            template <SizeT TargetCount>
            class MipmapUploadTargetArray {
            public:
                MipmapUploadTargetArray() {
                    static_assert(TargetCount > 0, "Upload target count must be greater than zero");
                }

                void AllocateLevel(Uint targetIndex, Uint level, MipmapInput input) {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "AllocateLevel: target invalid");

                    m_storage[targetIndex].AllocateLevel(level, input);
                }

                // Per-target, like AllocateLevel: cube-map faces are respecified independently, so
                // truncating one face must not disturb the others.
                void TruncateToLevelCount(Uint targetIndex, SizeT levelCount) {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "TruncateToLevelCount: target invalid");

                    m_storage[targetIndex].TruncateToLevelCount(levelCount);
                }

                void UpdateSubData(Uint targetIndex, Uint level, DataPtr input) {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "UpdateSubData: target invalid");
                    m_storage[targetIndex].UpdateSubData(level, input);
                }

                void* MapData(Uint targetIndex, Uint level) {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "UpdateSubData: target invalid");
                    return m_storage[targetIndex].MapData(level);
                }

                IntVec3 GetTexelSize(Uint targetIndex, Uint level) const {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "GetTexelSize: target invalid");

                    return m_storage[targetIndex].GetTexelSize(level);
                }

                SizeT GetByteSize(Uint targetIndex, Uint level) const {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "GetByteSize: target invalid");

                    return m_storage[targetIndex].GetByteSize(level);
                }

                SizeT GetLevelCount() const {
                    static_assert(TargetCount > 0, "Upload target count must be greater than zero");
                    return m_storage[0].GetLevelCount();
                }

                void MarkDirty(Uint targetIndex, Uint level, bool dirty) {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "MarkDirty: target invalid");
                    m_storage[targetIndex].MarkDirty(level, dirty);
                }

                bool IsDirty(Uint targetIndex, Uint level) const {
                    MOBILEGL_ASSERT(targetIndex < TargetCount, "IsDirty: target invalid");
                    return m_storage[targetIndex].IsDirty(level);
                }

            protected:
                Array<MipmapStorage, TargetCount> m_storage;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
