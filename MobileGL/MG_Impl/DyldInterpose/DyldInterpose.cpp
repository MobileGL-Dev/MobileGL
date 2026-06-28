// MobileGL - MobileGL/MG_Impl/DyldInterpose/DyldInterpose.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>

#if defined(__APPLE__)

#include "MG_Impl/GetProcAddress.h"

#include <dlfcn.h>

namespace {
    struct DyldInterposeEntry {
        const void* Replacement;
        const void* Replacee;
    };

    bool IsGLProcName(const char* name) {
        if (name == nullptr) {
            return false;
        }

        if (strncmp(name, "CGL", 3) == 0) {
            return true;
        }

        if (strncmp(name, "gl", 2) != 0) {
            return false;
        }

        // Avoid stealing glfw*/glib*/glX*/global application symbols.
        return name[2] >= 'A' && name[2] <= 'Z' && name[2] != 'X';
    }

    void* MobileGLDlsym(void* handle, const char* symbol) {
        if (IsGLProcName(symbol)) {
            if (void* proc = MobileGL::MG_Impl::GetProcAddress(symbol)) {
                return proc;
            }
        }

        return dlsym(handle, symbol);
    }

    __attribute__((used)) static const DyldInterposeEntry kMobileGLDyldInterpose[]
        __attribute__((section("__DATA,__interpose"))) = {
            {reinterpret_cast<const void*>(MobileGLDlsym), reinterpret_cast<const void*>(dlsym)},
        };
} // namespace

#endif
