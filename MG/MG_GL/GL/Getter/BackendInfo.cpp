//
// Created by BZLZHH on 2025/3/15.
//

#include "BackendInfo.h"

namespace MG_GL::Getter {
    const std::string GetBackendName() {
#if BACKEND_TYPE == BACKEND_VULKAN
        return "Vulkan";
#elif BACKEND_TYPE == BACKEND_METAL
        return "Metal";
#elif BACKEND_TYPE == BACKEND_GLES
        return "OpenGL ES";
#else
        return "<Unknown Backend>";
#endif
    }

    const std::string GetMGName() {
#if BACKEND_TYPE == BACKEND_VULKAN
        return "MobileGLUVK";
#elif BACKEND_TYPE == BACKEND_METAL
        return "MobileGLUMTL";
#elif BACKEND_TYPE == BACKEND_GLES
        return "MobileGLUES";
#else
        return "<Unknown MobileGL Version>";
#endif
    }

    void LogMGInfo() {
        std::string MGName = MG_GL::Getter::GetMGName()+
                             ", MobileGL (" + MG_GL::Getter::GetBackendName() + ")";

        std::string MGVersion = std::to_string(MG_Global::MG::VersionMajor) + "."
                                +  std::to_string(MG_Global::MG::VersionMinor) + "."
                                +  std::to_string(MG_Global::MG::VersionRevision);
        if (MG_Global::MG::VersionPatch != 0)
            MGVersion += "." + std::to_string(MG_Global::MG::VersionPatch);
        MGVersion += MG_Global::MG::VersionSuffix;

        std::string GLVersion = std::to_string(MG_Global::GL::GLVersionMajor) + "."
                                +  std::to_string(MG_Global::GL::GLVersionMinor) + "."
                                +  std::to_string(MG_Global::GL::GLVersionRevision);

        std::string backendVersion;
#if BACKEND_TYPE == BACKEND_VULKAN
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Vulkan Version Query";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        VkInstance instance;
        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan instance!");
        }
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("No Vulkan devices found!");
        }
        VkPhysicalDevice physicalDevice;
        vkEnumeratePhysicalDevices(instance, &deviceCount, &physicalDevice);
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
        uint32_t major = VK_VERSION_MAJOR(deviceProperties.apiVersion);
        uint32_t minor = VK_VERSION_MINOR(deviceProperties.apiVersion);
        uint32_t patch = VK_VERSION_PATCH(deviceProperties.apiVersion);
        vkDestroyInstance(instance, nullptr);
        
        backendVersion = "Vulkan " + std::to_string(major) + "." + 
                std::to_string(minor)+"." + std::to_string(patch);
#else
        backendVersion = "<Unknown Backend>";
#endif

        MG_Util::Debug::LogI("MobileGL Renderer Info:\n");
        MG_Util::Debug::LogI("----%s:", MGName.c_str());
        MG_Util::Debug::LogI("--------MobileGL Version: %s", MGVersion.c_str());
        MG_Util::Debug::LogI("--------Implemented OpenGL Version: %s", GLVersion.c_str());
        MG_Util::Debug::LogI("--------Backend Version: %s", backendVersion.c_str());
    }
}