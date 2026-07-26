// MobileGL - MobileGL/Init.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "Includes.h"

namespace MobileGL {
    void Initialize();
    // Thread-safe, idempotent, and re-entrant wrapper around Initialize().
    // Host layers (EGL/WGL/CGL entry points) call this lazily on first use so
    // MobileGL's lifecycle never depends on ELF/DLL static constructors, and
    // so a fresh init can follow a full Destroy() (e.g. after the last
    // eglTerminate).
    void EnsureInitialized();
    void Destroy();

    namespace MG_Util::Debug {
        void InitFile();
    } // namespace MG_Util::Debug

    namespace MG_ConfigLoader {
        void Init();
    } // namespace MG_ConfigLoader

    namespace MG_Backend {
        void Init();
    } // namespace MG_Backend

    namespace MG_Impl {
        void Init();
    } // namespace MG_Impl
} // namespace MobileGL
