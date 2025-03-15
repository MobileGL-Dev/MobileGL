//
// Created by BZLZHH on 2025/3/15.
//

#include "BackendInfo.h"

namespace MG_GL::Getter {
    const std::string GetBackendName() {
#if BACKEND_TYPE == BACKEND_VULKAN
        return "Vulkan";
#elif BACKEND_TYPE == BACKEND_VULKAN
        return "Metal";
#elif BACKEND_TYPE == BACKEND_VULKAN
        return "OpenGL ES";
#else
        return "<Unknown Backend>";
#endif
    }
}