// MobileGL - MobileGL/GlobalObjects.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Config.h"

namespace MobileGL {
    namespace MG_Config {
        BackendType ActiveBackendType;
    } // namespace MG_Config

    namespace MG_Backend {
        UniquePtr<BackendObject> pActiveBackendObject;
        GlobalBackendFunctionsTable gBackendFunctionsTable;
    } // namespace MG_Backend
} // namespace MobileGL
