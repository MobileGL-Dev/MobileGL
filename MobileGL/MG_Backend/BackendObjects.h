// MobileGL - MobileGL/MG_Backend/BackendObjects.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "BackendObject.h"
#include "DirectGLES/BackendObject_DirectGLES.h"

namespace MobileGL::MG_Backend {
    extern UniquePtr<BackendObject> pActiveBackendObject;
    extern GlobalBackendFunctionsTable gBackendFunctionsTable;

    void Init();
} // namespace MobileGL::MG_Backend
