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
    // full backend initialization never depends on ELF/DLL static constructors,
    // and so a fresh init can follow a full Destroy() (e.g. after the last
    // eglTerminate). The macOS dyld bootstrap installs only lightweight
    // NSOpenGL method hooks.
    void EnsureInitialized();
    void Destroy();

    namespace MG_Util::Debug {
        void InitFile();
    } // namespace MG_Util::Debug

    namespace MG_ConfigLoader {
        void Init();
        // One setting resolved the way Init() resolves it - the environment, then the system
        // configuration files (ConfigLoader.cpp) - for the few readers that must decide something
        // before MobileGL initializes. False when neither names it.
        Bool LookupSetting(const char* key, String& outValue);
    } // namespace MG_ConfigLoader

    // Whether this process can be served right now. False when MobileGL is configured to reach a
    // server that is not there (or a split session already failed to come up), or when the process
    // asked for software rendering (LIBGL_ALWAYS_SOFTWARE, desktop Linux), so a
    // caller that can choose another implementation - the system's EGL/GLX/GBM loaders - should
    // decline rather than hand out something that cannot draw. Cheap: once true it stays true
    // until the library is torn down, and a probe is one non-blocking connect with a short bound.
    Bool ImplementationAvailable();

    namespace MG_Backend {
        void Init();
    } // namespace MG_Backend

    namespace MG_Impl {
        void Init();
    } // namespace MG_Impl
} // namespace MobileGL
