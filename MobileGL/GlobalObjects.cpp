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
        // Leak-at-exit storage: the UniquePtr itself lives on the heap and is
        // never destroyed by the runtime, so process exit runs no backend
        // destructors (static destruction order across TUs is undefined).
        // Deterministic teardown happens inside the EGL lifecycle instead:
        // the last eglTerminate calls MobileGL::Destroy(), which .reset()s
        // these singletons while the process is still healthy.
        UniquePtr<BackendObject>& pActiveBackendObject = *new UniquePtr<BackendObject>();
        GlobalBackendFunctionsTable gBackendFunctionsTable;
    } // namespace MG_Backend
} // namespace MobileGL
