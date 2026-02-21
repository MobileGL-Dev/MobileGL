// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Renderer/VulkanContext.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanContext.h"

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    namespace {
        Bool CheckDeviceExtensionSupport(VkPhysicalDevice device, const Vector<const char*>& required) {
            Uint32 count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
            Vector<VkExtensionProperties> props(count);
            if (count > 0) vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());

            for (auto* ext : required) {
                Bool found = false;
                for (const auto& p : props) {
                    if (std::strcmp(p.extensionName, ext) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }

        Bool FindGraphicsQueueFamily(VkPhysicalDevice device, VkSurfaceKHR surface, Uint32& outFamily) {
            Uint32 count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            if (count == 0) return false;
            Vector<VkQueueFamilyProperties> props(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());

            for (Uint32 i = 0; i < count; ++i) {
                if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                if (surface != VK_NULL_HANDLE) {
                    VkBool32 presentSupport = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
                    if (!presentSupport) continue;
                }
                outFamily = i;
                return true;
            }
            return false;
        }

        Bool SupportsDynamicRendering(VkPhysicalDevice device) {
            VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &vk13;
            vkGetPhysicalDeviceFeatures2(device, &features2);
            return vk13.dynamicRendering == VK_TRUE;
        }

        Bool SupportsTimelineSemaphore(VkPhysicalDevice device) {
            VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &vk12;
            vkGetPhysicalDeviceFeatures2(device, &features2);
            return vk12.timelineSemaphore == VK_TRUE;
        }
    } // namespace

    VulkanContext::~VulkanContext() {
        Cleanup();
    }

    void VulkanContext::Initialize(ANativeWindow* window, const char* appName) {
        if (m_instance != VK_NULL_HANDLE) return;
        m_window = window;
        CreateInstance(appName ? appName : "MobileGL-Vulkan");
        CreateSurface(window);
        PickPhysicalDevice();
        CreateDevice();
    }

    void VulkanContext::Cleanup() {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        m_physicalDevice = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_graphicsQueueFamily = ~0u;
        m_window = nullptr;
    }

    void VulkanContext::CreateInstance(const char* appName) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = appName;
        app.applicationVersion = VK_MAKE_VERSION(1, 3, 0);
        app.pEngineName = "MobileGL";
        app.engineVersion = VK_MAKE_VERSION(1, 3, 0);
        app.apiVersion = VK_API_VERSION_1_3;

        Vector<const char*> extensions;
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif

        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = static_cast<Uint32>(extensions.size());
        ici.ppEnabledExtensionNames = extensions.data();

        VK_VERIFY(vkCreateInstance(&ici, nullptr, &m_instance), "vkCreateInstance");
    }

    void VulkanContext::CreateSurface(ANativeWindow* window) {
        if (!window) return;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = window;
        VK_VERIFY(vkCreateAndroidSurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateAndroidSurfaceKHR");
#else
        (void)window;
#endif
    }

    void VulkanContext::PickPhysicalDevice() {
        Uint32 count = 0;
        VK_VERIFY(vkEnumeratePhysicalDevices(m_instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (count == 0) throw RuntimeError("No Vulkan physical devices found");

        Vector<VkPhysicalDevice> devices(count);
        VK_VERIFY(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()), "vkEnumeratePhysicalDevices list");

        Vector<const char*> requiredExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        for (auto device : devices) {
            Uint32 family = ~0u;
            if (!FindGraphicsQueueFamily(device, m_surface, family)) continue;
            if (!CheckDeviceExtensionSupport(device, requiredExts)) continue;
            if (!SupportsDynamicRendering(device)) continue;

            m_physicalDevice = device;
            m_graphicsQueueFamily = family;
            break;
        }

        if (m_physicalDevice == VK_NULL_HANDLE) {
            throw RuntimeError("No suitable Vulkan physical device found");
        }
    }

    void VulkanContext::CreateDevice() {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = m_graphicsQueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        Vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        VkPhysicalDeviceFeatures features{};
        VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        vk13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        vk12.timelineSemaphore = SupportsTimelineSemaphore(m_physicalDevice) ? VK_TRUE : VK_FALSE;
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.features = features;
        vk13.pNext = &vk12;
        features2.pNext = &vk13;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &features2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<Uint32>(deviceExtensions.size());
        dci.ppEnabledExtensionNames = deviceExtensions.data();
        dci.pEnabledFeatures = nullptr;

        VK_VERIFY(vkCreateDevice(m_physicalDevice, &dci, nullptr, &m_device), "vkCreateDevice");
        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    }
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager
