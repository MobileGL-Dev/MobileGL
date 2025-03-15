//
// Created by BZLZHH on 2025/3/15.
//

#ifndef MOBILEGL_GLOBAL_H
#define MOBILEGL_GLOBAL_H

#include "Includes.h"

namespace MG_Global {
    namespace MG {
        inline const int VersionMajor = 0;
        inline const int VersionMinor = 0;
        inline const int VersionRevision = 1;
        inline const int VersionPatch = 0;
        inline const std::string VersionSuffix = "";
    }
    
    namespace Backend {
#define BACKEND_TYPE BACKEND_VULKAN
    }
    
    namespace GL {
        inline const int GLVersionMajor = 3;
        inline const int GLVersionMinor = 0;
        inline const int GLVersionRevision = 0;
    }
    
    namespace Common {
        inline const int LogLevel = MG_Constants::Common::LOG_LEVEL_DEBUG;
        
#ifdef __ANDROID__
        inline const char* LOG_FILE_PATH = "/sdcard/MG/latest.log";
#else
        inline const char* LOG_FILE_PATH = nullptr;
#endif
    }
}

#endif //MOBILEGL_GLOBAL_H
