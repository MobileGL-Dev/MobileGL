//
// Created by BZLZHH on 2025/3/15.
//

#ifndef MOBILEGL_CONSTANTS_H
#define MOBILEGL_CONSTANTS_H

#ifdef __ANDROID__
#define __ANDROID_API__ 26
#endif
#define MG_EXPORT __attribute__((visibility("default")))

namespace MG_Constants {
    namespace Common {
        inline const int LOG_LEVEL_DEBUG = 0x0010;
        inline const int LOG_LEVEL_WARN = 0x0011;
        inline const int LOG_LEVEL_ERROR = 0x0012;
        inline const int LOG_LEVEL_INFO = 0x0013;
        inline const int LOG_LEVEL_FATAL = 0x0014;
    }

    namespace Backend {
#define BACKEND_VULKAN 0x0015
#define BACKEND_METAL 0x0016
#define BACKEND_GLES 0x0017
    }
    
    inline const char* RenderName = "MobileGL";
}

#endif //MOBILEGL_CONSTANTS_H
