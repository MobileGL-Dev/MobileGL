//
// Created by BZLZHH on 2025/3/15.
//

#include "LookUp.h"

#include "../../../Includes.h"

#define DEBUG 0

namespace MG_GL::GLX {
    void* GetProcAddress(const char *name) {
        void* proc = dlsym(RTLD_DEFAULT, (const char*)name);

        if (!proc) {
            MG_Util::Debug::LogW("Failed to get function: %s", (const char*)name);
            return nullptr;
        }

        return proc;
    }

    void* GetProcAddressARB(const char *name) {
        return GetProcAddress(name);
    }
}