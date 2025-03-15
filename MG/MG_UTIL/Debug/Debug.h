//
// Created by BZLZHH on 2025/1/26.
//

#ifndef MOBILEGL_DEBUG_H
#define MOBILEGL_DEBUG_H

#include "../../Includes.h"

#define FORCE_SYNC_WITH_LOG_FILE 0

namespace MG_Util {
    namespace Debug {
        const char* GetOSName();
        
        void LogD (const char* format, ...);
        void LogW (const char* format, ...);
        void LogE (const char* format, ...);
        void LogI (const char* format, ...);
        void LogF (const char* format, ...);
        void LogClear();
        void LogInit();
        void LogWrite(const char* format, ...);
        
        const char* GLEnumToString(::GLenum value);
    }
}

#define MOBILEGL_DEBUG_H

#endif //MOBILEGL_DEBUG_H
