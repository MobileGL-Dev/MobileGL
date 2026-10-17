// MobileGL - MobileGL/MG_Backend/BackendFunctions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendFunctions.h"

namespace MobileGL::MG_Backend {
    namespace {
        MG_Util::Damage::Region& PresentDamageSlot() {
            thread_local MG_Util::Damage::Region damage = MG_Util::Damage::Region::Full();
            return damage;
        }
    } // namespace

    const MG_Util::Damage::Region& CurrentPresentDamage() {
        return PresentDamageSlot();
    }
    void SetCurrentPresentDamage(const MG_Util::Damage::Region& damage) {
        PresentDamageSlot() = damage;
    }
} // namespace MobileGL::MG_Backend
