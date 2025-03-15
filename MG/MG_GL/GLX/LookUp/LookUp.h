//
// Created by BZLZHH on 2025/3/15.
//

#ifndef MOBILEGL_LOOKUP_H
#define MOBILEGL_LOOKUP_H

namespace MG_GL::GLX {
    void* GetProcAddress(const char *name);
    void* GetProcAddressARB(const char *name);
}
#endif //MOBILEGL_LOOKUP_H
