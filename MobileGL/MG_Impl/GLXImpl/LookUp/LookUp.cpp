// MobileGL - MobileGL/MG_Impl/GLXImpl/LookUp/LookUp.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LookUp.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include "../GLXImpl.h"
#endif

namespace MG_Impl::GLXImpl {
    void* GetProcAddress(const char* name) {
        if (!name) {
            return nullptr;
        }
        MGLOG_D("glXGetProcAddress(\"%s\")", name);
#if defined(__linux__) && !defined(__ANDROID__)
        if (name[0] == 'g' && name[1] == 'l' && name[2] == 'X') {
            // glX entry points resolve from the GLX layer's own table; GL/EGL
            // names fall through to the shared resolver below.
            void* proc = MobileGL::MG_Impl::GLXImpl::GetGLXEntryPoint(name);
            if (!proc) {
                MGLOG_D("glXGetProcAddress: unknown glX entry point %s", name);
            }
            return proc;
        }
#endif
        void* proc = MobileGL::MG_Impl::GetProcAddress(name);
        if (!proc) {
            MGLOG_D("Failed to get function: %s", (const char*)name);
            return nullptr;
        }

        return proc;
    }

    void* GetProcAddressARB(const char* name) {
        return GetProcAddress(name);
    }
} // namespace MG_Impl::GLXImpl
