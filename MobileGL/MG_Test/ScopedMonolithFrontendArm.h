// MobileGL - MobileGL/MG_Test/ScopedMonolithFrontendArm.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once
#include <Config.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Test {

    // P13 W4: THE MONOLITH FRONTEND ARM, PINNED FOR ONE CASE. A case that uses this asserts how
    // the frontend answers when the backend reads the frontend's objects itself - pushing a map's
    // bytes through the object, reading a texture back from the frontend's own level shadow - which
    // is the arm W4 retires family by family and W6 deletes. On the record arm (every wire, and
    // monolith's from W4) the client tracker and the backend's record readers own those answers,
    // and the split and integration lanes cover them. When W6 deletes the frontend arm, the cases
    // that use this go with it.
    //
    // Both halves of the arm: the predicate, and the backend table (the verb port's slots go back
    // to the backend's own entry points for the scope).
    class ScopedMonolithFrontendArm {
    public:
        ScopedMonolithFrontendArm() : m_savedTable(MG_Backend::gBackendFunctionsTable) {
            m_saved = MG_Config::MonolithTakesRecordArm;
            MG_Config::MonolithTakesRecordArm = false;
            if (MG_Config::Transport == MG_Config::TransportMode::Monolith && MG_Backend::pActiveBackendObject) {
                MG_Backend::gBackendFunctionsTable = MG_Backend::pActiveBackendObject->GetBackendFunctions();
            }
        }
        ~ScopedMonolithFrontendArm() {
            MG_Config::MonolithTakesRecordArm = m_saved;
            MG_Backend::gBackendFunctionsTable = m_savedTable;
        }
        ScopedMonolithFrontendArm(const ScopedMonolithFrontendArm&) = delete;
        ScopedMonolithFrontendArm& operator=(const ScopedMonolithFrontendArm&) = delete;

    private:
        MG_Backend::GlobalBackendFunctionsTable m_savedTable;
        Bool m_saved = false;
    };

} // namespace MobileGL::MG_Test
