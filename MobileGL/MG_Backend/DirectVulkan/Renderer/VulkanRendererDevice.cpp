// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererDevice.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
#if defined(__APPLE__)
    namespace {
        constexpr unsigned long kNSWindowStyleMaskBorderless = 0;
        constexpr unsigned long kNSBackingStoreBuffered = 2;

        id SendId(id receiver, const char* selector) {
            return ObjcMsgSend<id (*)(id, SEL)>()(receiver, sel_registerName(selector));
        }

        void SendVoidBool(id receiver, const char* selector, bool value) {
            ObjcMsgSend<void (*)(id, SEL, bool)>()(receiver, sel_registerName(selector), value);
        }

        void SendVoidId(id receiver, const char* selector, id value) {
            ObjcMsgSend<void (*)(id, SEL, id)>()(receiver, sel_registerName(selector), value);
        }

        void SendVoidCGRect(id receiver, const char* selector, CGRect value) {
            ObjcMsgSend<void (*)(id, SEL, CGRect)>()(receiver, sel_registerName(selector), value);
        }

        void SendVoidCGSize(id receiver, const char* selector, CGSize value) {
            ObjcMsgSend<void (*)(id, SEL, CGSize)>()(receiver, sel_registerName(selector), value);
        }

        id Retain(id object) {
            return object ? SendId(object, "retain") : nil;
        }

        void* CreateInternalMetalLayer(Uint32 width, Uint32 height, void** outWindow) {
            const auto surfaceWidth = static_cast<CGFloat>(std::max<Uint32>(width, 1));
            const auto surfaceHeight = static_cast<CGFloat>(std::max<Uint32>(height, 1));
            id windowClass = reinterpret_cast<id>(objc_getClass("NSWindow"));
            id metalLayerClass = reinterpret_cast<id>(objc_getClass("CAMetalLayer"));
            MOBILEGL_ASSERT(windowClass && metalLayerClass,
                            "Failed to resolve NSWindow/CAMetalLayer for DirectVulkan pbuffer");

            CGRect frame = {{0.0, 0.0}, {surfaceWidth, surfaceHeight}};
            id window = SendId(windowClass, "alloc");
            window = ObjcMsgSend<id (*)(id, SEL, CGRect, unsigned long, unsigned long, bool)>()(
                window, sel_registerName("initWithContentRect:styleMask:backing:defer:"), frame,
                kNSWindowStyleMaskBorderless, kNSBackingStoreBuffered, true);
            MOBILEGL_ASSERT(window, "Failed to create hidden NSWindow for DirectVulkan pbuffer");

            id contentView = SendId(window, "contentView");
            MOBILEGL_ASSERT(contentView, "Failed to query hidden NSWindow contentView");
            SendVoidBool(contentView, "setWantsLayer:", true);

            id metalLayer = SendId(metalLayerClass, "layer");
            MOBILEGL_ASSERT(metalLayer, "Failed to create hidden CAMetalLayer for DirectVulkan pbuffer");
            Retain(metalLayer);
            SendVoidCGRect(metalLayer, "setFrame:", frame);
            SendVoidCGSize(metalLayer, "setDrawableSize:", frame.size);
            SendVoidId(contentView, "setLayer:", metalLayer);

            *outWindow = window;
            return metalLayer;
        }
    } // namespace
#endif

    void VulkanRenderer::CreateInstance() {
#if defined(VK_USE_PLATFORM_METAL_EXT)
        // MoltenVK snapshots its configuration when the loader first discovers the ICD. Set
        // this before instance-extension enumeration, while preserving an explicit user value.
        if (std::getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS") == nullptr) {
            if (::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0) == 0) {
                MGLOG_I("MoltenVK: enabling Metal argument buffers");
            } else {
                MGLOG_W("MoltenVK: could not enable Metal argument buffers before ICD discovery");
            }
        }
#endif
        m_extensions = EnumerateInstanceExtensions();
        MGLOG_I("Got %d Vulkan instance extensions: ", m_extensions.size());
        for (auto& extension : m_extensions) {
            MGLOG_I("    %s (r.%u)", extension.extensionName, extension.specVersion);
        }

        Bool validationLayerAvailable = CheckValidationLayerSupport();
        MGLOG_I("Validation layers %s.", validationLayerAvailable ? "available" : "not available");
        MGLOG_I("Validation layers %s.", m_config.EnableValidationLayers ? "requested" : "not requested");

        if (m_config.EnableValidationLayers && !validationLayerAvailable) {
            MGLOG_I("Validation layers not available! Disabling validation layers.");
        }

        m_validationLayersEnabled = m_config.EnableValidationLayers && validationLayerAvailable;

        // The debug messenger is a VK_EXT_debug_utils object, but a driver can ship
        // the validation layers while exposing only the older VK_EXT_debug_report
        // (Adreno 650 / Vulkan 1.1.128 does exactly that). Requesting the extension
        // unconditionally tripped the required-extension assert below, aborting every
        // validation-enabled build in CreateInstance. Keep the layers - they still
        // validate, and on Android they report to logcat on their own - and drop only
        // the messenger.
        const Bool debugUtilsAvailable =
            m_validationLayersEnabled && IsExtensionSupported(m_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        // Without a reporting channel the layers validate but say nothing, so fall
        // back to VK_EXT_debug_report when debug_utils is missing.
        const Bool debugReportAvailable = m_validationLayersEnabled && !debugUtilsAvailable &&
                                          IsExtensionSupported(m_extensions, VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        if (m_validationLayersEnabled && !debugUtilsAvailable) {
            MGLOG_I("%s not available; validation reports via %s instead.", VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                    debugReportAvailable ? VK_EXT_DEBUG_REPORT_EXTENSION_NAME : "(no channel)");
        }

        // ---------------- App info -------------------
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = m_config.AppName.c_str();
        appInfo.applicationVersion = VK_MAKE_VERSION(m_config.CacheVersion, 0, 0);
        appInfo.pEngineName = "MobileGL";
        appInfo.engineVersion = VK_MAKE_VERSION(m_config.Version.Major, m_config.Version.Minor, m_config.Version.Patch);
#ifdef VK_USE_PLATFORM_WIN32_KHR
        appInfo.apiVersion = VK_API_VERSION_1_3;
#else
        appInfo.apiVersion = VK_API_VERSION_1_1;
#endif

        // ---------------- Instance info -------------------
        VkInstanceCreateInfo instanceInfo = {};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        // Extensions
        Vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
        if (!m_window) {
#ifdef VK_USE_PLATFORM_METAL_EXT
            exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined VK_USE_PLATFORM_ANDROID_KHR
            m_headlessSurfaceSupported = IsExtensionSupported(m_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            if (m_headlessSurfaceSupported) {
                exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            } else {
                // No mobile ICD seen so far implements VK_EXT_headless_surface
                // (Mali r32p1 does not), and this used to abort the process the
                // moment an application asked for a pbuffer context. CreateSurface()
                // gives the WSI an AImageReader window instead, so request the
                // Android surface extension for it.
                MGLOG_I("%s not available; falling back to an AImageReader %s surface for the pbuffer context.",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                m_androidSurfaceEnabled = true;
            }
#elif defined VK_USE_PLATFORM_XLIB_KHR
            // An offscreen surface has ZERO window-system dependence, by design and on
            // every machine - including ones that do have a display. There used to be a
            // fallback here that requested VK_KHR_xlib_surface and had CreateSurface()
            // open a hidden, never-mapped X window; it is gone. A pbuffer that quietly
            // needs an X server is a pbuffer that works on a workstation and dies on a
            // headless runner, which is exactly what it did: with no DISPLAY, XOpenDisplay
            // returned null and the next Xlib call segfaulted. If the loader genuinely has
            // no VK_EXT_headless_surface, that is an honest bring-up failure and is
            // reported as one below - never papered over with a window.
            m_headlessSurfaceSupported = IsExtensionSupported(m_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            if (!m_headlessSurfaceSupported) {
                MGLOG_F("%s is not available from this Vulkan loader, so an offscreen (pbuffer) DirectVulkan "
                        "surface cannot be created. Refusing to substitute a window: offscreen surfaces must not "
                        "depend on a window system. Install an ICD that implements it (lavapipe does).",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
                throw RuntimeError("VK_EXT_headless_surface is unavailable for an offscreen DirectVulkan surface");
            }
            exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
#else
            exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
#endif
        } else {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
            exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
            m_androidSurfaceEnabled = true;
#elif defined VK_USE_PLATFORM_WIN32_KHR
            exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined VK_USE_PLATFORM_METAL_EXT
            exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined VK_USE_PLATFORM_XLIB_KHR
            exts.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#else
#warning "VulkanContext::CreateInstance: VK_KHR_*_surface extension not defined on this platform"
#endif
        } // TODO: support more platforms

#if defined(VK_USE_PLATFORM_METAL_EXT)
        if (IsExtensionSupported(m_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        } else {
            MGLOG_I("Optional Vulkan instance extension not supported: %s",
                    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
#endif

        if (debugUtilsAvailable) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else if (debugReportAvailable) {
            exts.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        }

        MGLOG_I("Enabling %d Vulkan instance extensions:", exts.size());
        for (const char* ext : exts) {
            MGLOG_I("    %s", ext);
        }

        for (const char* ext : exts) {
            if (!IsExtensionSupported(m_extensions, ext)) {
                MGLOG_E("Required Vulkan instance extension not found: %s", ext);
            }
            MOBILEGL_ASSERT(IsExtensionSupported(m_extensions, ext), "Required Vulkan instance extension not found: %s",
                            ext);
        }

        instanceInfo.enabledExtensionCount = exts.size();
        instanceInfo.ppEnabledExtensionNames = exts.data();

        auto debugMessengerCreateInfo = PopulateDebugMessengerCreateInfo();
        // Layers
        const void* instanceCreatePNext = nullptr;
        if (m_validationLayersEnabled) {
            MGLOG_I("Enabling validation layer...");
            instanceInfo.enabledLayerCount = static_cast<uint32_t>(std::size(s_validationLayerNames));
            instanceInfo.ppEnabledLayerNames = s_validationLayerNames;
            // Chaining the messenger create-info is only legal with the extension on.
            instanceCreatePNext = debugUtilsAvailable ? &debugMessengerCreateInfo : nullptr;
        } else {
            instanceInfo.enabledLayerCount = 0;
        }

        instanceInfo.pNext = instanceCreatePNext;

        VK_VERIFY(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "vkCreateInstance failed");

        if (debugUtilsAvailable) {
            VK_VERIFY(SetupDebugMessenger());
        } else if (debugReportAvailable) {
            VK_VERIFY(SetupDebugReportCallback());
        }
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT,
                                                              Uint64, size_t, Int32 messageCode,
                                                              const char* pLayerPrefix, const char* pMessage, void*) {
        if ((flags & (VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                      VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)) != 0) {
            // MGLOG_F, unlatched, on purpose: a validation-layer report means MobileGL fed
            // Vulkan something illegal, which is a broken invariant rather than an expected
            // failure mode. It stays loud and keeps repeating - the quietness rules that latch
            // W/E are for expected failures, not for this. The callback is only installed when
            // a build arms the debug report extension, so it costs shipping builds nothing.
            MGLOG_F("[Vulkan %s %d] %s", pLayerPrefix ? pLayerPrefix : "?", messageCode, pMessage ? pMessage : "");
        }
        return VK_FALSE;
    }

    VkResult VulkanRenderer::SetupDebugReportCallback() {
        auto vkCreateDebugReportCallbackEXT =
            (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugReportCallbackEXT");
        if (!vkCreateDebugReportCallbackEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkDebugReportCallbackCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
        createInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                           VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        createInfo.pfnCallback = &DebugReportCallback;
        return vkCreateDebugReportCallbackEXT(m_instance, &createInfo, nullptr, &m_debugReportCallback);
    }

    void VulkanRenderer::DestroyDebugReportCallback() {
        if (m_debugReportCallback == VK_NULL_HANDLE) return;
        auto func =
            (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugReportCallbackEXT");
        if (func != nullptr) func(m_instance, m_debugReportCallback, nullptr);
        m_debugReportCallback = VK_NULL_HANDLE;
    }

    VkResult VulkanRenderer::SetupDebugMessenger() {
        auto createInfo = PopulateDebugMessengerCreateInfo();
        auto vkCreateDebugUtilsMessengerEXT =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (!vkCreateDebugUtilsMessengerEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VK_VERIFY(vkCreateDebugUtilsMessengerEXT(m_instance, &createInfo, nullptr, &m_debugMessenger));
        return VK_SUCCESS;
    }

    VkResult VulkanRenderer::DestroyDebugMessenger() {
        if (m_debugMessenger != VK_NULL_HANDLE) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance,
                                                                                   "vkDestroyDebugUtilsMessengerEXT");
            if (func != nullptr) {
                func(m_instance, m_debugMessenger, nullptr);
            } else {
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
        return VK_SUCCESS;
    }

    VkDebugUtilsMessengerCreateInfoEXT VulkanRenderer::PopulateDebugMessengerCreateInfo() {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = DebugCallback;
        createInfo.pUserData = this;
        return createInfo;
    }

    void VulkanRenderer::PickPhysicalDevice() {
        Uint32 deviceCount = 0;
        VK_VERIFY(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr));
        if (deviceCount == 0) {
            // A real, reachable configuration, not a broken invariant: an instance can be
            // created from ICDs that load perfectly and then expose no device at all - a
            // GPU-less machine with the vendor ICDs installed (RADV/ANV/NVK on a CI runner)
            // is exactly that. It has to be a bring-up failure the caller can report.
            //
            // It used to be MGLOG_E + MOBILEGL_ASSERT, and back then BOTH were compiled out at
            // the INFO log level every shipping and CI build uses - the ordering bug that made
            // MGLOG_E dead at INFO was only fixed in 2026-08. The count-zero case therefore fell
            // through in silence to `devices[0]` on an EMPTY vector below and segfaulted in
            // vkGetPhysicalDeviceProperties. MGLOG_F stays: E is live now, but MOBILEGL_ASSERT
            // is still DEBUG-only and this is a genuine bring-up abort, not a recoverable error.
            MGLOG_F("No Vulkan physical devices found: the instance loaded ICDs but none of them exposes a "
                    "device. Cannot bring up DirectVulkan. (A software ICD such as lavapipe provides one; "
                    "pin it with VK_ICD_FILENAMES if the machine has no GPU.)");
            throw RuntimeError("No Vulkan physical devices available for DirectVulkan");
        }
        MGLOG_I("Found %d physical device(s).", deviceCount);

        Vector<VkPhysicalDevice> devices(deviceCount);
        // Same truncation hazard as the instance-extension enumeration: a VK_INCOMPLETE here
        // leaves the tail of `devices` default-constructed (VK_NULL_HANDLE), and every one of
        // those is a null handle waiting to be passed to the driver. Take only what was
        // actually written.
        const VkResult enumerateResult = vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());
        if (enumerateResult != VK_SUCCESS && enumerateResult != VK_INCOMPLETE) {
            VK_VERIFY(enumerateResult, "vkEnumeratePhysicalDevices failed");
        }
        devices.resize(deviceCount);
        if (devices.empty()) {
            MGLOG_F("vkEnumeratePhysicalDevices reported devices and then wrote none");
            throw RuntimeError("No Vulkan physical devices available for DirectVulkan");
        }
        for (Int i = 0; i < deviceCount; i++) {
            if (GetMoreCapablePhysicalDevice(devices[i], m_surface, m_physicalDevice, m_physicalDevice))
                MGLOG_I("Picked physical device %d.", i);
        }

        if (m_physicalDevice.handle == VK_NULL_HANDLE) {
            m_physicalDevice.handle = devices[0];
            vkGetPhysicalDeviceProperties(devices[0], &m_physicalDevice.properties);
            MGLOG_I("No suitable physical device picked yet, defaulting to device 0.");
            MGLOG_W("No graphics queue found on physical device. Picking a device that doesn't do graphics?");
        }
    }

    Bool VulkanRenderer::GetMoreCapablePhysicalDevice(VkPhysicalDevice newVkDevice, VkSurfaceKHR surface,
                                                      const PhysicalDevice& otherDevice,
                                                      PhysicalDevice& outBetterDevice) {
        const auto deviceTypeToStr = [](VkPhysicalDeviceType type) {
            switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                return "INTEGRATED_GPU";
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                return "DISCRETE_GPU";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                return "CPU";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                return "VIRTUAL_GPU";
            case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                return "OTHER";
            default:
                return "UNKNOWN";
            }
        };

        PhysicalDevice newDevice;
        newDevice.handle = newVkDevice;

        vkGetPhysicalDeviceProperties(newVkDevice, &newDevice.properties);
        const auto& deviceProperties = newDevice.properties;
        auto apiVersion = deviceProperties.apiVersion;
        MGLOG_I("    %s (Vulkan %d.%d.%d, %s)", deviceProperties.deviceName, VK_VERSION_MAJOR(apiVersion),
                VK_VERSION_MINOR(apiVersion), VK_VERSION_PATCH(apiVersion),
                deviceTypeToStr(deviceProperties.deviceType));

        // Check device extensions (including swapchain extension)
        Bool deviceExtSupported = IsNecessaryDeviceExtensionSupported(newVkDevice);
        if (!deviceExtSupported) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: Some of the required device extension not supported on this "
                    "device)");
            return false;
        }

        // Check swapchain capabilities
        auto swapchainCapabilities = SwapchainObject::GetSwapchainCapabilities(newVkDevice, surface);
        if (!swapchainCapabilities.IsComplete()) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: Swapchain capabilities not met)");
            return false;
        }

        // Check queue families
        Vector<VkQueueFamilyProperties> queueFamilies = GetQueueFamilyFromPhysicalDevice(newVkDevice);
        newDevice.queueFamilies.graphicsFamily = GetQueueFamilyIndex(queueFamilies, VK_QUEUE_GRAPHICS_BIT);
        if (newDevice.queueFamilies.graphicsFamily == -1) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: No graphics queue family)");
            return false;
        }

        newDevice.queueFamilies.presentFamily =
            GetPresentQueueFamilyIndex(newDevice, surface, queueFamilies, newDevice.queueFamilies.graphicsFamily);
        if (newDevice.queueFamilies.presentFamily == -1) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: No present queue family)");
            return false;
        }

        // Accept software/virtual/other devices when no discrete or integrated GPU
        // has been selected yet. This is important for Linux headless CI using lavapipe.
        if (!otherDevice.IsComplete()) {
            outBetterDevice = newDevice;
            MGLOG_I("    Picked physical device. (Reason: First suitable device)");
            return true;
        }

        // Pick discrete GPU
        if (newDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            otherDevice.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            outBetterDevice = newDevice;
            MGLOG_I("    Picked physical device. (Reason: Discrete GPU)");
            return true;
        }

        // Pick integrated GPU if no discrete GPU
        if (newDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            otherDevice.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            outBetterDevice = newDevice;
            MGLOG_I("    Picked physical device. (Reason: Integrated GPU and no discrete one found yet)");
            return true;
        }

        // Ignore other GPU when discrete GPU found
        if (newDevice.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            otherDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: Already picked discrete GPU)");
            return false;
        }

        return false;
    }

    Bool VulkanRenderer::IsNecessaryDeviceExtensionSupported(VkPhysicalDevice device) {
        const Vector<VkExtensionProperties> availableExtensions = EnumerateDeviceExtensions(device);

        MGLOG_I("Got %u Vulkan device extensions: ", static_cast<Uint32>(availableExtensions.size()));
        for (auto& extension : availableExtensions) {
            MGLOG_I("    %s (r.%u)", extension.extensionName, extension.specVersion);
        }

        for (SizeT i = 0; i < std::size(s_deviceExtensionNames); ++i) {
            if (!IsExtensionSupported(availableExtensions, s_deviceExtensionNames[i])) {
                MGLOG_I("Required extension not found: %s", s_deviceExtensionNames[i]);
                return false;
            }
            MGLOG_I("Required extension found: %s", s_deviceExtensionNames[i]);
        }

        return true;
    }

    void VulkanRenderer::CreateLogicalDeviceAndQueues() {
        Float queuePriority = 1.0f;

        Vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        MOBILEGL_ASSERT(m_physicalDevice.queueFamilies.graphicsFamily != -1, "Graphics queue family not found.");
        VkDeviceQueueCreateInfo& gfxQueueCreateInfo = queueCreateInfos.emplace_back();
        gfxQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        gfxQueueCreateInfo.queueFamilyIndex = m_physicalDevice.queueFamilies.graphicsFamily;
        gfxQueueCreateInfo.queueCount = 1;
        gfxQueueCreateInfo.pQueuePriorities = &queuePriority;

        if (m_physicalDevice.queueFamilies.graphicsFamily != m_physicalDevice.queueFamilies.presentFamily) {
            MOBILEGL_ASSERT(m_physicalDevice.queueFamilies.presentFamily != -1, "Present queue family not found.");
            VkDeviceQueueCreateInfo& presentQueueCreateInfo = queueCreateInfos.emplace_back();
            presentQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            presentQueueCreateInfo.queueFamilyIndex = m_physicalDevice.queueFamilies.presentFamily;
            presentQueueCreateInfo.queueCount = 1;
            presentQueueCreateInfo.pQueuePriorities = &queuePriority;
        }

        VkPhysicalDeviceFeatures supportedDeviceFeatures{};
        vkGetPhysicalDeviceFeatures(m_physicalDevice.handle, &supportedDeviceFeatures);

        VkPhysicalDeviceFeatures deviceFeatures{};
        // Match GL's robust buffer-fetch behavior where the Vulkan device supports it. This covers
        // out-of-range fetches; arbitrary GL vertex strides/offsets still need the explicit tight
        // repack in VertexInputStateFactory when they violate Vulkan's address-alignment rules.
        // MOBILEGL_MAGMA_DISABLE_ROBUST_BUFFER_ACCESS leaves it off to measure or dodge its GPU cost.
        deviceFeatures.robustBufferAccess =
            MG_Config::Features.MagmaDisableRobustBufferAccess ? VK_FALSE : supportedDeviceFeatures.robustBufferAccess;
        deviceFeatures.geometryShader = supportedDeviceFeatures.geometryShader;
        deviceFeatures.tessellationShader = supportedDeviceFeatures.tessellationShader;
        // gl_PointSize is an ORDINARY per-vertex output in desktop GL - a tessellation
        // evaluation or geometry shader may write it, and a program may capture it by name -
        // but in Vulkan the PointSize built-in is only usable from those two stages when this
        // feature is on (VUID-RuntimeSpirv-PointSize-06439; SPIR-V spells the requirement as
        // the TessellationPointSize / GeometryPointSize capabilities, which glslang emits from
        // any such write). Left off, every one of those programs is invalid usage that a lenient
        // driver silently gives an undefined point size and a strict one faults on. Nothing here
        // asks for it speculatively: the feature is taken only where the device advertises it.
        deviceFeatures.shaderTessellationAndGeometryPointSize =
            supportedDeviceFeatures.shaderTessellationAndGeometryPointSize;
        m_tessellationAndGeometryPointSizeFeatureEnabled =
            deviceFeatures.shaderTessellationAndGeometryPointSize == VK_TRUE;
        // Sampled-read barriers may only name the shader stages whose device feature is
        // actually enabled (VUID-vkCmdPipelineBarrier-srcStageMask-04090/-04091), so the
        // mask is assembled here, next to the feature decision, and handed to consumers.
        m_sampledReadStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (deviceFeatures.geometryShader == VK_TRUE) {
            m_sampledReadStageMask |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
        }
        if (deviceFeatures.tessellationShader == VK_TRUE) {
            m_sampledReadStageMask |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
        }
        deviceFeatures.independentBlend = supportedDeviceFeatures.independentBlend;
        m_independentBlendFeatureEnabled = deviceFeatures.independentBlend == VK_TRUE;
        deviceFeatures.fillModeNonSolid = supportedDeviceFeatures.fillModeNonSolid;
        m_fillModeNonSolidFeatureEnabled = deviceFeatures.fillModeNonSolid == VK_TRUE;
        deviceFeatures.dualSrcBlend = supportedDeviceFeatures.dualSrcBlend;
        // GL_POLYGON_OFFSET_CLAMP (glPolygonOffsetClamp) maps onto vkCmdSetDepthBias's clamp
        // argument, which must stay 0 unless this feature is enabled.
        deviceFeatures.depthBiasClamp = supportedDeviceFeatures.depthBiasClamp;
        m_depthBiasClampFeatureEnabled = deviceFeatures.depthBiasClamp == VK_TRUE;
        m_dualSrcBlendFeatureEnabled = deviceFeatures.dualSrcBlend == VK_TRUE;
        // ARB_sample_shading. Without this feature a pipeline may not set sampleShadingEnable
        // (VUID-VkPipelineMultisampleStateCreateInfo-sampleShadingEnable-00784), so the GL enable
        // has to be dropped rather than forwarded - which is what the flag below records.
        deviceFeatures.sampleRateShading = supportedDeviceFeatures.sampleRateShading;
        m_sampleRateShadingFeatureEnabled = deviceFeatures.sampleRateShading == VK_TRUE;
        // ARB_viewport_array rasterization. Without multiViewport a pipeline may declare exactly
        // one viewport (VUID-VkPipelineViewportStateCreateInfo-viewportCount-01216), so a shader's
        // gl_ViewportIndex can only ever select viewport 0 and the other fifteen rectangles are
        // state with nowhere to go. The GL state stays 16 wide either way - GL 4.3 core requires
        // MAX_VIEWPORTS >= 16 and that is a frontend promise, not a device one; this gate decides
        // only whether a DRAW can rasterize into more than one of them.
        deviceFeatures.multiViewport = supportedDeviceFeatures.multiViewport;
        m_multiViewportFeatureEnabled = deviceFeatures.multiViewport == VK_TRUE;
        m_maxRasterizableViewports =
            m_multiViewportFeatureEnabled
                ? std::min<Uint32>(RenderStateParameters::MAX_VIEWPORTS,
                                   std::max<Uint32>(m_physicalDevice.properties.limits.maxViewports, 1u))
                : 1u;
        MGLOG_I("Vulkan: multiViewport %s; rasterizable viewports=%u (device limit %u, GL state width %u)",
                m_multiViewportFeatureEnabled ? "enabled" : "UNAVAILABLE", m_maxRasterizableViewports,
                m_physicalDevice.properties.limits.maxViewports,
                static_cast<Uint32>(RenderStateParameters::MAX_VIEWPORTS));
        if (!m_multiViewportFeatureEnabled) {
            MGLOG_W("Vulkan: the device does not support the multiViewport feature; gl_ViewportIndex will always "
                    "select viewport 0 and per-viewport scissor/depth-range state past index 0 cannot be "
                    "rasterized (the state itself is still stored and queryable)");
        }
        deviceFeatures.logicOp = supportedDeviceFeatures.logicOp;
        deviceFeatures.shaderClipDistance = supportedDeviceFeatures.shaderClipDistance;
        deviceFeatures.shaderCullDistance = supportedDeviceFeatures.shaderCullDistance;
        deviceFeatures.wideLines = supportedDeviceFeatures.wideLines;
        m_logicOpFeatureEnabled = deviceFeatures.logicOp == VK_TRUE;
        deviceFeatures.shaderInt64 = supportedDeviceFeatures.shaderInt64;
        // Required for any module that declares OpCapability Float64 - which is every shader with a
        // double in it, including the 64-bit vertex attribute path (the attribute itself arrives as
        // uint32 words, but the bitcast result and everything computed from it is Float64). Without
        // it vkCreateShaderModule is invalid usage (VUID-VkShaderModuleCreateInfo-pCode-08740),
        // which is why SupportsFloat64VertexAttributes gates the entry point on the same feature.
        deviceFeatures.shaderFloat64 = supportedDeviceFeatures.shaderFloat64;
        // Required before a VK_IMAGE_VIEW_TYPE_CUBE_ARRAY view may be created
        // (VUID-VkImageViewCreateInfo-viewType-01004). Without it a cube map array texture cannot
        // get its sampled or full view, so SyncTextureResource fails and the texture stays unbacked.
        deviceFeatures.imageCubeArray = supportedDeviceFeatures.imageCubeArray;
        // Required for desktop GL image load/store semantics. iterationRP writes storage
        // images from vertex and fragment stages and uses formats outside Vulkan's small
        // mandatory storage-image set.
        deviceFeatures.vertexPipelineStoresAndAtomics = supportedDeviceFeatures.vertexPipelineStoresAndAtomics;
        deviceFeatures.fragmentStoresAndAtomics = supportedDeviceFeatures.fragmentStoresAndAtomics;
        deviceFeatures.shaderStorageImageExtendedFormats = supportedDeviceFeatures.shaderStorageImageExtendedFormats;
        // The formatless float-storage compatibility path must be all-or-nothing: transformed
        // modules declare both capabilities and image bindings may be read, written, or both.
        m_unformattedFloatStorageImagesEnabled =
            supportedDeviceFeatures.shaderStorageImageReadWithoutFormat == VK_TRUE &&
            supportedDeviceFeatures.shaderStorageImageWriteWithoutFormat == VK_TRUE;
        if (m_unformattedFloatStorageImagesEnabled) {
            deviceFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
            deviceFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        } else {
            // Surface the degradation instead of failing silently: shader packs that bind a
            // float storage image with a format different from its declaration (e.g.
            // iterationRP) will render incorrectly on this device.
            MGLOG_W("CreateLogicalDeviceAndQueues: shaderStorageImage*WithoutFormat unavailable "
                    "(read=%d write=%d); float storage-image format reinterpretation is disabled "
                    "and packs relying on it may misrender",
                    supportedDeviceFeatures.shaderStorageImageReadWithoutFormat,
                    supportedDeviceFeatures.shaderStorageImageWriteWithoutFormat);
        }
        deviceFeatures.drawIndirectFirstInstance = supportedDeviceFeatures.drawIndirectFirstInstance;
        m_drawIndirectFirstInstanceFeatureEnabled = deviceFeatures.drawIndirectFirstInstance == VK_TRUE;
        deviceFeatures.multiDrawIndirect = supportedDeviceFeatures.multiDrawIndirect;
        m_multiDrawIndirectFeatureEnabled = deviceFeatures.multiDrawIndirect == VK_TRUE;
        m_logicOpFeatureEnabled = deviceFeatures.logicOp == VK_TRUE;
        // Backs GL_TEXTURE_MAX_ANISOTROPY_EXT; optional in Vulkan, so the sampler manager falls back
        // to isotropic filtering (and the extension goes unadvertised) when the device lacks it.
        deviceFeatures.samplerAnisotropy = supportedDeviceFeatures.samplerAnisotropy;
        m_samplerAnisotropyFeatureEnabled = deviceFeatures.samplerAnisotropy == VK_TRUE;
        // GL_SAMPLES_PASSED needs exact sample counts; without the feature the boolean
        // occlusion result still satisfies any-samples-style consumers.
        deviceFeatures.occlusionQueryPrecise = supportedDeviceFeatures.occlusionQueryPrecise;
        m_occlusionQueryPreciseEnabled = deviceFeatures.occlusionQueryPrecise == VK_TRUE;
        m_tessellationShaderFeatureEnabled = deviceFeatures.tessellationShader == VK_TRUE;
        // Backs the GL_PRIMITIVES_GENERATED reroute's statistics tier (see the
        // m_primGenReroute* members): a VK_QUERY_TYPE_PIPELINE_STATISTICS pool may only
        // be created with this feature enabled. Enabled wherever the device has it - the
        // feature alone costs nothing; pools exist only where the reroute is armed.
        deviceFeatures.pipelineStatisticsQuery = supportedDeviceFeatures.pipelineStatisticsQuery;
        m_pipelineStatisticsQueryFeatureEnabled = deviceFeatures.pipelineStatisticsQuery == VK_TRUE;

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
        deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
        if (m_validationLayersEnabled) {
            deviceCreateInfo.enabledLayerCount = static_cast<uint32_t>(std::size(s_validationLayerNames));
            deviceCreateInfo.ppEnabledLayerNames = s_validationLayerNames;
        } else {
            deviceCreateInfo.enabledLayerCount = 0;
        }

        Vector<const char*> enabledDeviceExtensions;
        enabledDeviceExtensions.reserve(std::size(s_deviceExtensionNames) + 2);
        for (const char* extensionName : s_deviceExtensionNames) {
            enabledDeviceExtensions.push_back(extensionName);
        }

        const Vector<VkExtensionProperties> availableExtensions = EnumerateDeviceExtensions(m_physicalDevice.handle);
        ResolveOptionalDeviceExtensions(availableExtensions, enabledDeviceExtensions);

        // VK_KHR_image_format_list lets a MUTABLE_FORMAT image declare exactly which formats it
        // may be viewed as. Adreno drops UBWC bandwidth compression on a blindly-mutable image
        // (measured: 65 -> 80 fps in MC 26.2 once mutability is not requested); an explicit,
        // compression-compatible format list is the portable way to keep both.
        m_imageFormatListExtensionEnabled =
            IsExtensionSupported(availableExtensions, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        if (m_imageFormatListExtensionEnabled) {
            enabledDeviceExtensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        }
        MGLOG_I("VK_KHR_image_format_list enabled: %s", m_imageFormatListExtensionEnabled ? "true" : "false");
        MGLOG_I("VK_KHR_draw_indirect_count enabled: %s", m_drawIndirectCountExtensionEnabled ? "true" : "false");

        m_indexTypeUint8ExtensionEnabled = false;
        const char* indexTypeUint8ExtensionName = nullptr;
        if (IsExtensionSupported(availableExtensions, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME)) {
            indexTypeUint8ExtensionName = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
        } else if (IsExtensionSupported(availableExtensions, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME)) {
            indexTypeUint8ExtensionName = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
        }

        auto getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2"));
        if (getPhysicalDeviceFeatures2 == nullptr) {
            getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2KHR"));
        }

        m_updateAfterBindLimits = {};
        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
        descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        VkPhysicalDeviceDescriptorIndexingProperties descriptorIndexingProperties{};
        descriptorIndexingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
        const Bool descriptorIndexingCore =
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
            // CreateInstance requests Vulkan 1.1 on these platforms. A 1.2+ GPU
            // does not promote descriptor indexing into that application's core
            // API, so the record arm enables VK_EXT_descriptor_indexing instead (P13 W6:
            // every library, not only the transport build - the old monolith-only `true`
            // here treated the 1.2 device as core under a 1.1 instance).
            false &&
#endif
            m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2;
        const Bool descriptorIndexingExtension =
            IsExtensionSupported(availableExtensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
        if (getPhysicalDeviceProperties2 == nullptr) {
            getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        // Linux/Android request Vulkan 1.1: a 1.2 physical device alone does
        // not expose the promoted renderpass2/depth-resolve API to this app.
#ifdef VK_USE_PLATFORM_WIN32_KHR
        const Bool wireDepthResolveCore = m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2;
#else
        const Bool wireDepthResolveCore = false;
#endif
        const Bool wireDepthResolveExtensions =
            m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_1 &&
            IsExtensionSupported(availableExtensions, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) &&
            IsExtensionSupported(availableExtensions, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        const Bool wireDepthResolveEnabled =
            getPhysicalDeviceProperties2 && (wireDepthResolveCore || wireDepthResolveExtensions);
        m_wireCreateRenderPass2 = nullptr;
        m_wireDepthResolveModes = m_wireStencilResolveModes = 0;
        if (wireDepthResolveEnabled) {
            if (!wireDepthResolveCore) {
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                              VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                              VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
            }
            VkPhysicalDeviceDepthStencilResolveProperties resolveProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &resolveProperties;
            getPhysicalDeviceProperties2(m_physicalDevice.handle, &properties);
            m_wireDepthResolveModes = resolveProperties.supportedDepthResolveModes;
            m_wireStencilResolveModes = resolveProperties.supportedStencilResolveModes;
        }
        // P7 wave 2-B2, CONTRACT-P7 §3.2 (`multisample-blit-aspect`): the shader resolve's
        // stencil half needs SPV_EXT_shader_stencil_export, because a fragment shader cannot
        // write the stencil aspect without it. Enabled on the same terms as the resolve
        // extensions above - wire arms only - and the arm declines stencil where it is absent
        // rather than producing an undefined aspect.
        m_wireShaderStencilExport =
            IsExtensionSupported(availableExtensions, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        if (m_wireShaderStencilExport)
            EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                          VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        // P7 gate 5 (g5-msprobe): which multisample depth/stencil resolve arm goes first is measured
        // once the device exists (ArmWireDepthResolveOrder, below), not keyed on the vendor.
        m_wirePreferShaderDepthResolve = false;
        // P11 B2 (T0): a client's AHardwareBuffer imported as a wire buffer store. Wire arms, on
        // Android, whenever the device advertises the extension - not per session: the device is
        // created once per server and a later session asking T0 must find it able. Enabling it
        // changes no T2 behaviour; only an import uses it. The 1.0-era dependencies are core at the
        // 1.1 this device is created at, and are enabled too when advertised (the spike's set).
        m_wireAhbImport = false;
        m_wireGetAhbProperties = nullptr;
        m_sharedImageSyncFd = false;
#if defined(__ANDROID__)
        if (m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_1 &&
            IsExtensionSupported(availableExtensions,
                                 VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
            IsExtensionSupported(availableExtensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            m_wireAhbImport = true;
            for (const char* name :
                 {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                  VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                  VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                  VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
                  VK_KHR_MAINTENANCE_1_EXTENSION_NAME}) {
                if (IsExtensionSupported(availableExtensions, name))
                    EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions, name);
            }
            // Shared-image fences (WireSharedImage.cpp) cross sessions as sync_files, which this
            // device waits on and signals through SYNC_FD binary semaphores - imported for a wait,
            // exported for a signal. Both directions, or the CPU fallback.
            if (IsExtensionSupported(availableExtensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
                auto getExternalSemaphoreProperties =
                    reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                        vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
                if (getExternalSemaphoreProperties == nullptr) {
                    getExternalSemaphoreProperties =
                        reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                            vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
                }
                VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
                if (getExternalSemaphoreProperties != nullptr) {
                    VkPhysicalDeviceExternalSemaphoreInfo query{
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
                    query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                    getExternalSemaphoreProperties(m_physicalDevice.handle, &query, &properties);
                }
                constexpr VkExternalSemaphoreFeatureFlags both =
                    VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
                if ((properties.externalSemaphoreFeatures & both) == both) {
                    m_sharedImageSyncFd = true;
                    for (const char* name :
                         {VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
                        if (IsExtensionSupported(availableExtensions, name))
                            EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions, name);
                    }
                }
            }
            MGLOG_I("DirectVulkan: shared-image fences %s",
                    m_sharedImageSyncFd ? "wait and signal on the GPU (SYNC_FD semaphores)"
                                        : "are waited for on the CPU (no SYNC_FD semaphore import and export)");
        }
#endif
        if ((descriptorIndexingCore || descriptorIndexingExtension) && getPhysicalDeviceFeatures2 != nullptr &&
            getPhysicalDeviceProperties2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &descriptorIndexingFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            VkPhysicalDeviceProperties2 propertyQuery{};
            propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            propertyQuery.pNext = &descriptorIndexingProperties;
            getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);

            // This renderer emits every descriptor category listed below, including
            // dynamic UBOs and combined image samplers. Do not enable a partial
            // descriptor-indexing contract: it would make a later reflected program
            // fail in the driver instead of choosing its ordinary descriptor layout.
            const Bool allUpdateAfterBindFeatures =
                descriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_TRUE &&
                (!deviceFeatures.robustBufferAccess || descriptorIndexingProperties.robustBufferAccessUpdateAfterBind);
            if (allUpdateAfterBindFeatures) {
                if (!descriptorIndexingCore &&
                    !IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
                }
                descriptorIndexingFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &descriptorIndexingFeatures;
                m_updateAfterBindLimits = {
                    true,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindSamplers,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindUniformBuffers,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindStorageImages,
                    descriptorIndexingProperties.maxPerStageUpdateAfterBindResources,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSamplers,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindUniformBuffers,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffers,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageImages};
                MGLOG_I("Vulkan: update-after-bind descriptor layouts enabled");
            } else {
                MGLOG_I("Vulkan: descriptor indexing is present but lacks the complete update-after-bind feature set; "
                        "using ordinary descriptor layouts");
            }
        } else {
            MGLOG_I("Vulkan: descriptor indexing unavailable; using ordinary descriptor layouts");
        }

        VkPhysicalDeviceIndexTypeUint8Features indexTypeUint8Features{};
        indexTypeUint8Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES;
        if (indexTypeUint8ExtensionName != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &indexTypeUint8Features;
            MOBILEGL_ASSERT(getPhysicalDeviceFeatures2 != nullptr,
                            "CreateLogicalDeviceAndQueues: vkGetPhysicalDeviceFeatures2 is unavailable");
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (indexTypeUint8Features.indexTypeUint8 == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, indexTypeUint8ExtensionName)) {
                    enabledDeviceExtensions.push_back(indexTypeUint8ExtensionName);
                }
                m_indexTypeUint8ExtensionEnabled = true;
                indexTypeUint8Features.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &indexTypeUint8Features;
                MGLOG_I("Enabled optional device extension: %s", indexTypeUint8ExtensionName);
            } else {
                MGLOG_W("%s is advertised, but indexTypeUint8 feature is unavailable; uint8 index buffers will stay "
                        "disabled",
                        indexTypeUint8ExtensionName);
            }
        } else {
            MGLOG_W("VK_KHR_index_type_uint8 / VK_EXT_index_type_uint8 not supported; uint8 index buffers will stay "
                    "disabled");
        }

        m_shaderDrawParametersFeatureEnabled = false;
        VkPhysicalDeviceShaderDrawParametersFeatures shaderDrawParametersFeatures{};
        shaderDrawParametersFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
        if (m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_1 && getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &shaderDrawParametersFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (shaderDrawParametersFeatures.shaderDrawParameters == VK_TRUE) {
                shaderDrawParametersFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &shaderDrawParametersFeatures;
                m_shaderDrawParametersFeatureEnabled = true;
            }
        } else if (m_shaderDrawParametersExtensionEnabled) {
            // Vulkan 1.0 device: enabling VK_KHR_shader_draw_parameters alone exposes the SPIR-V
            // DrawParameters capability; the shaderDrawParameters feature struct only exists from 1.1.
            m_shaderDrawParametersFeatureEnabled = true;
        }
        if (!m_shaderDrawParametersFeatureEnabled) {
            MGLOG_W("shaderDrawParameters is unavailable; shaders using gl_DrawID/gl_BaseInstance will not work");
        }

        // primitiveTopologyListRestart lets primitive restart work on *list* topologies (strip/fan
        // restart needs no feature). Optional; enabled via VK_EXT_primitive_topology_list_restart.
        m_primitiveTopologyListRestartFeatureEnabled = false;
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT listRestartFeatures{};
        listRestartFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &listRestartFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (listRestartFeatures.primitiveTopologyListRestart == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
                }
                listRestartFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &listRestartFeatures;
                m_primitiveTopologyListRestartFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s", VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
            }
        }

        // VK_EXT_custom_border_color: an arbitrary GL_TEXTURE_BORDER_COLOR, in float or integer form,
        // instead of the four predefined VkBorderColor values. Without it a border outside
        // transparent black / opaque black / opaque white has to be snapped, which is what made every
        // border texel of a GL_RGBA8 texture with border (255,255,255,255) sample as 0 and what made
        // an integer border of -1 come back as 0.
        //
        // customBorderColorWithoutFormat is required alongside customBorderColors, not merely
        // preferred: a GL sampler object carries a border colour with no idea which texture it will
        // be paired with, so the VkSamplerCustomBorderColorCreateInfoEXT this backend builds has to
        // leave `format` VK_FORMAT_UNDEFINED.
        m_customBorderColorFeatureEnabled = false;
        m_maxCustomBorderColorSamplers = 0;
        VkPhysicalDeviceCustomBorderColorFeaturesEXT customBorderColorFeatures{};
        customBorderColorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &customBorderColorFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (customBorderColorFeatures.customBorderColors == VK_TRUE &&
                customBorderColorFeatures.customBorderColorWithoutFormat == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
                }
                customBorderColorFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &customBorderColorFeatures;
                m_customBorderColorFeatureEnabled = true;

                if (getPhysicalDeviceProperties2 != nullptr) {
                    VkPhysicalDeviceCustomBorderColorPropertiesEXT customBorderColorProperties{};
                    customBorderColorProperties.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT;
                    VkPhysicalDeviceProperties2 propertyQuery{};
                    propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    propertyQuery.pNext = &customBorderColorProperties;
                    getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
                    m_maxCustomBorderColorSamplers = customBorderColorProperties.maxCustomBorderColorSamplers;
                }
                MGLOG_I("Enabled optional device extension: %s (maxCustomBorderColorSamplers=%u)",
                        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, m_maxCustomBorderColorSamplers);
            }
        }
        if (!m_customBorderColorFeatureEnabled) {
            MGLOG_I("%s unavailable; GL_TEXTURE_BORDER_COLOR snaps to the nearest predefined VkBorderColor",
                    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        }

        // Native subgroup topology, and VK_EXT_subgroup_size_control's
        // computeFullSubgroups feature. REQUIRE_FULL_SUBGROUPS on a compute stage is what
        // turns the derived gl_NumSubgroups (DeriveNumSubgroupsPass) from
        // encouraged-but-unspecified driver behaviour into a spec guarantee: with the bit
        // set and local_size_x a multiple of the subgroup size, every subgroup launches
        // full, so the subgroup count is exactly invocations / size ("Full Subgroups",
        // VUID-VkPipelineShaderStageCreateInfo-flags-02759/-02785).
        m_nativeSubgroupSize = 0;
        m_nativeSubgroupSupported = false;
        m_computeFullSubgroupsFeatureEnabled = false;
        if (getPhysicalDeviceProperties2 != nullptr) {
            VkPhysicalDeviceSubgroupProperties subgroupProperties{};
            subgroupProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
            VkPhysicalDeviceProperties2 subgroupPropertyQuery{};
            subgroupPropertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            subgroupPropertyQuery.pNext = &subgroupProperties;
            getPhysicalDeviceProperties2(m_physicalDevice.handle, &subgroupPropertyQuery);
            // Mirrors the loader's HasUsableShaderSubgroupSupport gate, so the module lowerings
            // can never disagree with the advertised capabilities.
            const Bool usableSubgroups = subgroupProperties.subgroupSize > 0 &&
                                         (subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                                         (subgroupProperties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
            if (usableSubgroups) {
                m_nativeSubgroupSize = subgroupProperties.subgroupSize;
                m_nativeSubgroupSupported = true;
            }
        }
        VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroupSizeControlFeatures{};
        subgroupSizeControlFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
        m_maxComputeWorkgroupSubgroups = 0;
        if (m_nativeSubgroupSupported &&
            IsExtensionSupported(availableExtensions, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &subgroupSizeControlFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (getPhysicalDeviceProperties2 != nullptr) {
                VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroupSizeControlProperties{};
                subgroupSizeControlProperties.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 propertyQuery{};
                propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                propertyQuery.pNext = &subgroupSizeControlProperties;
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
                m_maxComputeWorkgroupSubgroups = subgroupSizeControlProperties.maxComputeWorkgroupSubgroups;
            }
            if (subgroupSizeControlFeatures.computeFullSubgroups == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
                }
                // Only the full-subgroups guarantee is wanted; required/varying subgroup
                // sizes stay unrequested.
                subgroupSizeControlFeatures.subgroupSizeControl = VK_FALSE;
                subgroupSizeControlFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &subgroupSizeControlFeatures;
                m_computeFullSubgroupsFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s (computeFullSubgroups)",
                        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
            }
        }

        // VK_EXT_transform_feedback backs GL transform feedback capture.
        m_transformFeedbackFeatureEnabled = false;
        VkPhysicalDeviceTransformFeedbackFeaturesEXT transformFeedbackFeatures{};
        transformFeedbackFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &transformFeedbackFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (transformFeedbackFeatures.transformFeedback == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
                }
                transformFeedbackFeatures.geometryStreams = VK_FALSE;
                transformFeedbackFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &transformFeedbackFeatures;
                m_transformFeedbackFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s", VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
            }
        }
        // VK_EXT_primitives_generated_query - the query Vulkan defines for GL's
        // GL_PRIMITIVES_GENERATED precisely because the stream query above needs no
        // capture by spec but drivers disagree. Taken with BOTH the base feature and the
        // rasterizer-discard feature or not at all: without the latter, a discarding draw
        // inside the query is invalid usage, and GL applications toggle discard freely.
        // Only the PRIMITIVES_GENERATED reroute consumes it (see ArmPrimGenReroute).
        m_primitivesGeneratedQueryFeatureEnabled = false;
        m_primitivesGeneratedQueryDiscardFeatureEnabled = false;
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT primitivesGeneratedQueryFeatures{};
        primitivesGeneratedQueryFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &primitivesGeneratedQueryFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (primitivesGeneratedQueryFeatures.primitivesGeneratedQuery == VK_TRUE &&
                primitivesGeneratedQueryFeatures.primitivesGeneratedQueryWithRasterizerDiscard == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME);
                }
                primitivesGeneratedQueryFeatures.primitivesGeneratedQueryWithNonZeroStreams = VK_FALSE;
                primitivesGeneratedQueryFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &primitivesGeneratedQueryFeatures;
                m_primitivesGeneratedQueryFeatureEnabled = true;
                m_primitivesGeneratedQueryDiscardFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s", VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME);
            }
        }
        // VK_EXT_provoking_vertex. Two independent features live behind one extension:
        //   provokingVertexLast                       -> flat varyings, gl_Layer/gl_ViewportIndex and
        //                                                the input-assembler capture order.
        //   transformFeedbackPreservesProvokingVertex -> spec-level guarantee for the capture order;
        //                                                only legal when the transformFeedback
        //                                                feature is also enabled, which is why this
        //                                                block sits after the one above.
        // They are enabled independently on purpose: gating the first on the second would leave flat
        // shading GL-wrong on any device without VK_EXT_transform_feedback, for no legality reason.
        m_provokingVertexLastEnabled = false;
        m_provokingVertexXfbPreserveEnabled = false;
        m_provokingVertexModePerPipeline = false;
        m_provokingVertexFanPreserved = false;
        VkPhysicalDeviceProvokingVertexFeaturesEXT provokingVertexFeatures{};
        provokingVertexFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &provokingVertexFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);

            VkPhysicalDeviceProvokingVertexPropertiesEXT provokingVertexProperties{};
            provokingVertexProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT;
            auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
            if (getPhysicalDeviceProperties2 == nullptr) {
                getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
            }
            if (getPhysicalDeviceProperties2 != nullptr) {
                VkPhysicalDeviceProperties2 propertyQuery{};
                propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                propertyQuery.pNext = &provokingVertexProperties;
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
            }
            m_provokingVertexModePerPipeline = provokingVertexProperties.provokingVertexModePerPipeline == VK_TRUE;
            m_provokingVertexFanPreserved =
                provokingVertexProperties.transformFeedbackPreservesTriangleFanProvokingVertex == VK_TRUE;

            if (provokingVertexFeatures.provokingVertexLast == VK_TRUE) {
                // transformFeedbackPreservesProvokingVertex is deliberately NOT requested. Measured:
                // asking for it regresses transform_feedback.geometry on GL33 through GL45. A
                // geometry shader emits its triangles already in GL's vertex order, and the pipeline
                // that captures them runs on FIRST (see SelectProvokingVertexMode); without the
                // guarantee the driver leaves that stream alone, but with it the capture is forced to
                // follow the pipeline's FIRST convention and comes back rotated. The guarantee buys
                // nothing here either - the input-assembler capture order that
                // direct_state_access.queries_functional needs comes from provokingVertexLast alone,
                // which was confirmed by measurement. Leaving it off also keeps VU 04884 disarmed, so
                // a TRIANGLE_FAN pipeline may take LAST on any device.
                const Bool wantXfbPreserve = false;

                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
                }
                provokingVertexFeatures.provokingVertexLast = VK_TRUE;
                provokingVertexFeatures.transformFeedbackPreservesProvokingVertex =
                    wantXfbPreserve ? VK_TRUE : VK_FALSE;
                provokingVertexFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &provokingVertexFeatures;
                m_provokingVertexLastEnabled = true;
                m_provokingVertexXfbPreserveEnabled = wantXfbPreserve;
                MGLOG_I("Enabled optional device extension: %s (transformFeedbackPreservesProvokingVertex=%s)",
                        VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, wantXfbPreserve ? "true" : "false");
            }
        }
        if (!m_provokingVertexLastEnabled) {
            MGLOG_W("VK_EXT_provoking_vertex is unavailable; flat-shaded varyings take a primitive's first "
                    "vertex instead of GL's last, and transform feedback records TRIANGLE_STRIP/TRIANGLE_FAN "
                    "triangles rotated (0,1,2 / 1,3,2 instead of 0,1,2 / 2,1,3)");
        }

        if (!m_transformFeedbackFeatureEnabled) {
            MGLOG_W("VK_EXT_transform_feedback is unavailable; transform feedback capture will not work");
        }

        // VK_EXT_vertex_attribute_divisor. Vulkan's instance input rate advances an attribute
        // once per instance and nothing else, so without this every glVertexAttribDivisor value
        // collapses to 1 and an attribute meant to change every N instances changes every one.
        m_vertexAttributeDivisorEnabled = false;
        VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vertexAttributeDivisorFeatures{};
        vertexAttributeDivisorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &vertexAttributeDivisorFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (vertexAttributeDivisorFeatures.vertexAttributeInstanceRateDivisor == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
                }
                vertexAttributeDivisorFeatures.vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
                vertexAttributeDivisorFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &vertexAttributeDivisorFeatures;
                m_vertexAttributeDivisorEnabled = true;
                MGLOG_I("Enabled optional device extension: %s", VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
            }
        }
        if (!m_vertexAttributeDivisorEnabled) {
            MGLOG_W("VK_EXT_vertex_attribute_divisor is unavailable; a glVertexAttribDivisor other "
                    "than 1 will advance its attribute once per instance");
        }

        // Host query reset lets the occlusion-query ring recycle slots without a
        // command-buffer round trip.
        m_hostQueryResetEnabled = false;
        VkPhysicalDeviceHostQueryResetFeatures hostQueryResetFeatures{};
        hostQueryResetFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
        if (IsExtensionSupported(availableExtensions, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &hostQueryResetFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (hostQueryResetFeatures.hostQueryReset == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
                }
                hostQueryResetFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &hostQueryResetFeatures;
                m_hostQueryResetEnabled = true;
            }
        }

        // YUV shared images (WireYuvImage.cpp) are sampled through a VkSamplerYcbcrConversion, a
        // feature of its own; asked only where AHardwareBuffers are imported at all.
        m_samplerYcbcrConversion = false;
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures{};
        ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        if (m_wireAhbImport && getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &ycbcrFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (ycbcrFeatures.samplerYcbcrConversion == VK_TRUE) {
                ycbcrFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &ycbcrFeatures;
                m_samplerYcbcrConversion = true;
            }
        }

        // VK_EXT_multi_draw: tier 1 of the multi-draw dispatch - one vkCmdDrawMulti(Indexed)EXT
        // for a whole glMultiDraw* batch (VkMultiDrawIndexedInfoEXT carries per-draw
        // firstIndex/indexCount/vertexOffset, so glMultiDrawElementsBaseVertex fits natively).
        // Requested only when both the extension and its multiDraw feature are present;
        // absent it, the dispatch falls to the multiDrawIndirect tier or the unrolled loop.
        m_multiDrawExtensionEnabled = false;
        m_maxMultiDrawCount = 0;
        VkPhysicalDeviceMultiDrawFeaturesEXT multiDrawFeatures{};
        multiDrawFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_MULTI_DRAW_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &multiDrawFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (multiDrawFeatures.multiDraw == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_MULTI_DRAW_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_MULTI_DRAW_EXTENSION_NAME);
                }
                multiDrawFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &multiDrawFeatures;
                m_multiDrawExtensionEnabled = true;

                VkPhysicalDeviceMultiDrawPropertiesEXT multiDrawProperties{};
                multiDrawProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT;
                auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
                if (getPhysicalDeviceProperties2 == nullptr) {
                    getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                        vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
                }
                if (getPhysicalDeviceProperties2 != nullptr) {
                    VkPhysicalDeviceProperties2 propertyQuery{};
                    propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    propertyQuery.pNext = &multiDrawProperties;
                    getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
                }
                // Spec minimum is 1024; a driver reporting 0 through a failed query must not
                // zero out every batch, so fall back to the spec minimum.
                m_maxMultiDrawCount =
                    multiDrawProperties.maxMultiDrawCount != 0 ? multiDrawProperties.maxMultiDrawCount : 1024;
                MGLOG_I("Enabled optional device extension: %s (maxMultiDrawCount=%u)",
                        VK_EXT_MULTI_DRAW_EXTENSION_NAME, m_maxMultiDrawCount);
            } else {
                MGLOG_I("VK_EXT_multi_draw is advertised but its multiDraw feature is unavailable; "
                        "multi-draw batches use the indirect or unrolled tier");
            }
        }

        // VK_KHR_fragment_shading_rate, for GL_EXT_fragment_shading_rate. pipelineFragmentShadingRate
        // is the floor (glShadingRateEXT, read back through gl_ShadingRateEXT); the primitive rate
        // (gl_PrimitiveShadingRateEXT, combiner op 0) rides along when the device has it. The
        // attachment rate is left off: no GL framebuffer carries a rate attachment to this backend
        // yet, and enabling the feature would only change what the frontend may promise.
        // The extension builds on VK_KHR_create_renderpass2, which this 1.1 device needs by name.
        m_fragmentShadingRate = {};
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragmentShadingRateFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        if (IsExtensionSupported(availableExtensions, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) &&
            (m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2 ||
             IsExtensionSupported(availableExtensions, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) &&
            getPhysicalDeviceFeatures2 != nullptr && getPhysicalDeviceProperties2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            featureQuery.pNext = &fragmentShadingRateFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            auto getFragmentShadingRates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
            if (fragmentShadingRateFeatures.pipelineFragmentShadingRate == VK_TRUE &&
                getFragmentShadingRates != nullptr) {
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                              VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                              VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
                fragmentShadingRateFeatures.attachmentFragmentShadingRate = VK_FALSE;
                fragmentShadingRateFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &fragmentShadingRateFeatures;

                VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragmentShadingRateProperties{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
                VkPhysicalDeviceProperties2 propertyQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                propertyQuery.pNext = &fragmentShadingRateProperties;
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);

                auto& support = m_fragmentShadingRate;
                support.Pipeline = true;
                support.Primitive = fragmentShadingRateFeatures.primitiveFragmentShadingRate == VK_TRUE;
                support.NonTrivialCombiners = fragmentShadingRateProperties.fragmentShadingRateNonTrivialCombinerOps;
                support.WithShaderDepthStencilWrites =
                    fragmentShadingRateProperties.fragmentShadingRateWithShaderDepthStencilWrites;
                support.WithSampleMask = fragmentShadingRateProperties.fragmentShadingRateWithSampleMask;
                support.PrimitiveWithMultipleViewports =
                    fragmentShadingRateProperties.primitiveFragmentShadingRateWithMultipleViewports;
                support.LayeredAttachments = fragmentShadingRateProperties.layeredShadingRateAttachments;
                support.MinAttachmentTexelWidth =
                    fragmentShadingRateProperties.minFragmentShadingRateAttachmentTexelSize.width;
                support.MinAttachmentTexelHeight =
                    fragmentShadingRateProperties.minFragmentShadingRateAttachmentTexelSize.height;
                support.MaxAttachmentTexelWidth =
                    fragmentShadingRateProperties.maxFragmentShadingRateAttachmentTexelSize.width;
                support.MaxAttachmentTexelHeight =
                    fragmentShadingRateProperties.maxFragmentShadingRateAttachmentTexelSize.height;
                support.MaxAttachmentTexelAspectRatio =
                    fragmentShadingRateProperties.maxFragmentShadingRateAttachmentTexelSizeAspectRatio;

                Uint32 rateCount = 0;
                getFragmentShadingRates(m_physicalDevice.handle, &rateCount, nullptr);
                Vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(
                    rateCount, VkPhysicalDeviceFragmentShadingRateKHR{
                                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
                getFragmentShadingRates(m_physicalDevice.handle, &rateCount, rates.data());
                const auto log2Of = [](Uint32 size) -> Int {
                    return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : -1;
                };
                for (Uint32 r = 0; r < rateCount; ++r) {
                    const Int w = log2Of(rates[r].fragmentSize.width);
                    const Int h = log2Of(rates[r].fragmentSize.height);
                    if (w < 0 || h < 0) continue; // larger than GL's 4x4 can name
                    const Uint16 bit = static_cast<Uint16>(1u << ((w << 2) | h));
                    for (Uint32 i = 0; i < std::size(support.RateMasks); ++i) {
                        if ((rates[r].sampleCounts & (1u << i)) != 0) support.RateMasks[i] |= bit;
                    }
                }
                MGLOG_I("%s: primitive=%d nonTrivialCombiners=%d "
                        "rates(1x)=0x%04x rates(4x)=0x%04x",
                        VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, support.Primitive, support.NonTrivialCombiners,
                        support.RateMasks[0], support.RateMasks[2]);
            }
        }

        // Codex closeout finding 2 (cf-magma): GL 4.6 core 8.26 makes an access through an
        // invalid image unit load zero and DISCARD stores and atomics. A null storage-image
        // descriptor is that rule exactly, so the wire arm binds one for such a unit
        // (UniformManager::SetWireInvalidStorageImageArm). ONLY nullDescriptor is requested:
        // robustBufferAccess2 / robustImageAccess2 would put a bounds check on every access of
        // every draw. Wire arms only, on the same terms as the depth-resolve extensions above -
        // the monolith arm resolves image units elsewhere and keeps its device as it was.
        m_wireNullDescriptor = false;
        VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features{};
        robustness2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        const char* robustness2Name = IsExtensionSupported(availableExtensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)
                                          ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME
                                      : IsExtensionSupported(availableExtensions, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME)
                                          ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME
                                          : nullptr;
        if (robustness2Name != nullptr && getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            featureQuery.pNext = &robustness2Features;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (robustness2Features.nullDescriptor == VK_TRUE) {
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions, robustness2Name);
                robustness2Features.robustBufferAccess2 = VK_FALSE;
                robustness2Features.robustImageAccess2 = VK_FALSE;
                robustness2Features.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &robustness2Features;
                m_wireNullDescriptor = true;
            }
        }
        MGLOG_I("Magma wire: robustness2 nullDescriptor %s (%s)", m_wireNullDescriptor ? "enabled" : "unavailable",
                robustness2Name != nullptr ? robustness2Name : "no robustness2 extension");

        deviceCreateInfo.enabledExtensionCount = static_cast<Uint32>(enabledDeviceExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
        MGLOG_I("Device feature support: robustBufferAccess=%s geometryShader=%s independentBlend=%s logicOp=%s "
                "shaderClipDistance=%s "
                "shaderCullDistance=%s wideLines=%s shaderInt64=%s vertexStoresAtomics=%s "
                "fragmentStoresAtomics=%s storageImageExtendedFormats=%s storageImageReadWithoutFormat=%s "
                "storageImageWriteWithoutFormat=%s drawIndirectFirstInstance=%s "
                "multiDrawIndirect=%s",
                supportedDeviceFeatures.robustBufferAccess ? "true" : "false",
                supportedDeviceFeatures.geometryShader ? "true" : "false",
                supportedDeviceFeatures.independentBlend ? "true" : "false",
                supportedDeviceFeatures.logicOp ? "true" : "false",
                supportedDeviceFeatures.shaderClipDistance ? "true" : "false",
                supportedDeviceFeatures.shaderCullDistance ? "true" : "false",
                supportedDeviceFeatures.wideLines ? "true" : "false",
                supportedDeviceFeatures.shaderInt64 ? "true" : "false",
                supportedDeviceFeatures.vertexPipelineStoresAndAtomics ? "true" : "false",
                supportedDeviceFeatures.fragmentStoresAndAtomics ? "true" : "false",
                supportedDeviceFeatures.shaderStorageImageExtendedFormats ? "true" : "false",
                supportedDeviceFeatures.shaderStorageImageReadWithoutFormat ? "true" : "false",
                supportedDeviceFeatures.shaderStorageImageWriteWithoutFormat ? "true" : "false",
                supportedDeviceFeatures.drawIndirectFirstInstance ? "true" : "false",
                supportedDeviceFeatures.multiDrawIndirect ? "true" : "false");
        MGLOG_I("Device feature enabled: robustBufferAccess=%s geometryShader=%s independentBlend=%s logicOp=%s "
                "shaderClipDistance=%s "
                "shaderCullDistance=%s wideLines=%s shaderInt64=%s vertexStoresAtomics=%s "
                "fragmentStoresAtomics=%s storageImageExtendedFormats=%s storageImageReadWithoutFormat=%s "
                "storageImageWriteWithoutFormat=%s drawIndirectFirstInstance=%s "
                "multiDrawIndirect=%s shaderDrawParameters=%s",
                deviceFeatures.robustBufferAccess ? "true" : "false", deviceFeatures.geometryShader ? "true" : "false",
                deviceFeatures.independentBlend ? "true" : "false", deviceFeatures.logicOp ? "true" : "false",
                deviceFeatures.shaderClipDistance ? "true" : "false",
                deviceFeatures.shaderCullDistance ? "true" : "false", deviceFeatures.wideLines ? "true" : "false",
                deviceFeatures.shaderInt64 ? "true" : "false",
                deviceFeatures.vertexPipelineStoresAndAtomics ? "true" : "false",
                deviceFeatures.fragmentStoresAndAtomics ? "true" : "false",
                deviceFeatures.shaderStorageImageExtendedFormats ? "true" : "false",
                deviceFeatures.shaderStorageImageReadWithoutFormat ? "true" : "false",
                deviceFeatures.shaderStorageImageWriteWithoutFormat ? "true" : "false",
                deviceFeatures.drawIndirectFirstInstance ? "true" : "false",
                deviceFeatures.multiDrawIndirect ? "true" : "false",
                m_shaderDrawParametersFeatureEnabled ? "true" : "false");
        VK_VERIFY(vkCreateDevice(m_physicalDevice.handle, &deviceCreateInfo, nullptr, &m_device), "vkCreateDevice");

        if (wireDepthResolveEnabled) {
            m_wireCreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(
                vkGetDeviceProcAddr(m_device, wireDepthResolveCore ? "vkCreateRenderPass2" : "vkCreateRenderPass2KHR"));
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        if (m_wireAhbImport) {
            m_wireGetAhbProperties =
                reinterpret_cast<void*>(vkGetDeviceProcAddr(m_device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
            if (m_wireGetAhbProperties == nullptr) m_wireAhbImport = false;
        }
        if (m_sharedImageSyncFd) {
            m_importSemaphoreFd =
                reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(m_device, "vkImportSemaphoreFdKHR"));
            m_getSemaphoreFd =
                reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
            if (m_importSemaphoreFd == nullptr || m_getSemaphoreFd == nullptr) {
                MGLOG_W("DirectVulkan: VK_KHR_external_semaphore_fd is missing an entry point; shared-image fences "
                        "are waited for on the CPU");
                m_sharedImageSyncFd = false;
            }
        }
#endif

        s_vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
            vkGetDeviceProcAddr(m_device, "vkCmdDrawIndexedIndirectCountKHR"));
        if (s_vkCmdDrawIndexedIndirectCount == nullptr) {
            s_vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
                vkGetDeviceProcAddr(m_device, "vkCmdDrawIndexedIndirectCount"));
        }
        if (m_drawIndirectCountExtensionEnabled && s_vkCmdDrawIndexedIndirectCount == nullptr) {
            MGLOG_W("VK_KHR_draw_indirect_count enabled but vkCmdDrawIndexedIndirectCount entry point is missing, will "
                    "continue as if VK_KHR_draw_indirect_count is not supported!");
            m_drawIndirectCountExtensionEnabled = false;
        }
        // P8-D: the wire arm's glMultiDrawArraysIndirectCount. Null = that verb reads its count on
        // the CPU (WireDraw.cpp's DrawWireIndirectNative answers false for it).
        s_vkCmdWireDrawIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
            vkGetDeviceProcAddr(m_device, "vkCmdDrawIndirectCountKHR"));
        if (s_vkCmdWireDrawIndirectCount == nullptr) {
            s_vkCmdWireDrawIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
                vkGetDeviceProcAddr(m_device, "vkCmdDrawIndirectCount"));
        }

        s_vkCmdDrawMultiEXT = nullptr;
        s_vkCmdDrawMultiIndexedEXT = nullptr;
        if (m_multiDrawExtensionEnabled) {
            s_vkCmdDrawMultiEXT =
                reinterpret_cast<PFN_vkCmdDrawMultiEXT>(vkGetDeviceProcAddr(m_device, "vkCmdDrawMultiEXT"));
            s_vkCmdDrawMultiIndexedEXT = reinterpret_cast<PFN_vkCmdDrawMultiIndexedEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdDrawMultiIndexedEXT"));
            if (s_vkCmdDrawMultiEXT == nullptr || s_vkCmdDrawMultiIndexedEXT == nullptr) {
                MGLOG_W("VK_EXT_multi_draw enabled but its entry points are missing, will continue as if "
                        "VK_EXT_multi_draw is not supported!");
                s_vkCmdDrawMultiEXT = nullptr;
                s_vkCmdDrawMultiIndexedEXT = nullptr;
                m_multiDrawExtensionEnabled = false;
            }
        }

        s_vkCmdSetFragmentShadingRateKHR = nullptr;
        if (m_fragmentShadingRate.Pipeline) {
            s_vkCmdSetFragmentShadingRateKHR = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(
                vkGetDeviceProcAddr(m_device, "vkCmdSetFragmentShadingRateKHR"));
            if (s_vkCmdSetFragmentShadingRateKHR == nullptr) {
                MGLOG_W("VK_KHR_fragment_shading_rate enabled but vkCmdSetFragmentShadingRateKHR is missing, "
                        "will continue without fragment shading rates!");
                m_fragmentShadingRate = {};
            }
        }

        // Resolve the multi-draw dispatch tiers once: device support clamped by the
        // MOBILEGL_MAGMA_MULTIDRAW_MODE preference. Requesting an unavailable tier is
        // never an error - the dispatch falls down the chain ext -> indirect -> unroll.
        {
            using MG_Config::MultiDrawMode;
            const MultiDrawMode mode = MG_Config::Features.MagmaMultiDrawMode;
            m_multiDrawAllowExt =
                m_multiDrawExtensionEnabled && (mode == MultiDrawMode::Auto || mode == MultiDrawMode::Ext);
            m_multiDrawAllowIndirect = m_multiDrawIndirectFeatureEnabled && mode != MultiDrawMode::Unroll;
            m_multiDrawForceUnrollIndirect = mode == MultiDrawMode::Unroll;
            if (mode == MultiDrawMode::Ext && !m_multiDrawExtensionEnabled) {
                MGLOG_I("MOBILEGL_MAGMA_MULTIDRAW_MODE=ext requested but VK_EXT_multi_draw is unavailable; "
                        "falling back to the %s tier",
                        m_multiDrawAllowIndirect ? "indirect" : "unroll");
            }
            if (mode == MultiDrawMode::Indirect && !m_multiDrawIndirectFeatureEnabled) {
                MGLOG_I("MOBILEGL_MAGMA_MULTIDRAW_MODE=indirect requested but the multiDrawIndirect device "
                        "feature is unavailable; falling back to the unroll tier");
            }
            MGLOG_I("Multi-draw dispatch tier: %s (VK_EXT_multi_draw=%s, multiDrawIndirect=%s, mode=%s)",
                    m_multiDrawAllowExt ? "ext" : (m_multiDrawAllowIndirect ? "indirect" : "unroll"),
                    m_multiDrawExtensionEnabled ? "true" : "false",
                    m_multiDrawIndirectFeatureEnabled ? "true" : "false",
                    mode == MultiDrawMode::Auto       ? "auto"
                    : mode == MultiDrawMode::Ext      ? "ext"
                    : mode == MultiDrawMode::Indirect ? "indirect"
                                                      : "unroll");
        }

        if (m_transformFeedbackFeatureEnabled) {
            s_vkCmdBindTransformFeedbackBuffersEXT = reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdBindTransformFeedbackBuffersEXT"));
            s_vkCmdBeginTransformFeedbackEXT = reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdBeginTransformFeedbackEXT"));
            s_vkCmdEndTransformFeedbackEXT = reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdEndTransformFeedbackEXT"));
            if (s_vkCmdBindTransformFeedbackBuffersEXT == nullptr || s_vkCmdBeginTransformFeedbackEXT == nullptr ||
                s_vkCmdEndTransformFeedbackEXT == nullptr) {
                MGLOG_W("VK_EXT_transform_feedback entry points missing; transform feedback capture disabled");
                m_transformFeedbackFeatureEnabled = false;
            }
        }
        if (m_hostQueryResetEnabled) {
            s_vkResetQueryPool =
                reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(m_device, "vkResetQueryPool"));
            if (s_vkResetQueryPool == nullptr) {
                s_vkResetQueryPool =
                    reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(m_device, "vkResetQueryPoolEXT"));
            }
            if (s_vkResetQueryPool == nullptr) {
                m_hostQueryResetEnabled = false;
            }
        }
        if (m_transformFeedbackFeatureEnabled) {
            s_vkCmdBeginQueryIndexedEXT = reinterpret_cast<PFN_vkCmdBeginQueryIndexedEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdBeginQueryIndexedEXT"));
            s_vkCmdEndQueryIndexedEXT =
                reinterpret_cast<PFN_vkCmdEndQueryIndexedEXT>(vkGetDeviceProcAddr(m_device, "vkCmdEndQueryIndexedEXT"));
            VkPhysicalDeviceTransformFeedbackPropertiesEXT xfbProperties{};
            xfbProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &xfbProperties;
            // Resolved via proc addr: vkGetPhysicalDeviceProperties2 is Vulkan 1.1, and Android's
            // libvulkan.so only exports it from API 28 while minSdk is 26.
            auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
            if (getPhysicalDeviceProperties2 == nullptr) {
                getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
            }
            if (getPhysicalDeviceProperties2 != nullptr) {
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &properties2);
            }
            m_xfbQueriesSupported = xfbProperties.transformFeedbackQueries == VK_TRUE &&
                                    s_vkCmdBeginQueryIndexedEXT != nullptr && s_vkCmdEndQueryIndexedEXT != nullptr;
        }
        MGLOG_I("index type uint8 enabled: %s", m_indexTypeUint8ExtensionEnabled ? "true" : "false");
        MGLOG_I("Logical device created.");

        // Queues
        vkGetDeviceQueue(m_device, m_physicalDevice.queueFamilies.graphicsFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_physicalDevice.queueFamilies.presentFamily, 0, &m_presentQueue);
        MGLOG_I("Queues got successfully.");

        // Timestamp (timer query) support: re-enumerate the graphics queue
        // family's properties for its timestampValidBits (0 means the queue
        // cannot write timestamps) and take timestampPeriod (ns per tick) from
        // the device limits.
        const auto timestampQueueFamilies = GetQueueFamilyFromPhysicalDevice(m_physicalDevice.handle);
        m_timestampValidBits = 0;
        const Int32 graphicsFamilyIndex = m_physicalDevice.queueFamilies.graphicsFamily;
        if (graphicsFamilyIndex >= 0 && static_cast<SizeT>(graphicsFamilyIndex) < timestampQueueFamilies.size()) {
            m_timestampValidBits = timestampQueueFamilies[graphicsFamilyIndex].timestampValidBits;
        }
        m_timestampPeriodNs = m_physicalDevice.properties.limits.timestampPeriod;
        m_timerQuerySupported = m_timestampValidBits > 0 && m_timestampPeriodNs > 0.0f;
        MGLOG_I("Timer queries %s (timestampValidBits=%u, timestampPeriod=%f ns/tick)",
                m_timerQuerySupported ? "supported" : "not supported", m_timestampValidBits, m_timestampPeriodNs);

        // Last, because it records on m_graphicsQueue: decide the PRIMITIVES_GENERATED
        // reroute for XFB-inactive draws. Nothing else has touched the queue yet.
        ArmPrimGenReroute();
        // Same terms: it records on m_graphicsQueue, which nothing but the probe above has used.
        ArmWireDepthResolveOrder();
        // Same terms again.
        ArmWireE5RawReads();
        ArmWireCopySliceSplit();
        ArmWireDiscardStateReemit();
    }

    // P7 gate 5 (g5-msprobe): THE ARM ORDER OF THE MULTISAMPLE DEPTH/STENCIL RESOLVE IS A MEASURED
    // PROPERTY OF THIS DEVICE. WireDepthResolveArm.h says why (the Adreno 830's no-draw
    // VK_KHR_depth_stencil_resolve pass writes nothing), WireDepthResolveProbe.h what is run. The
    // answer is a device property, so it is memoized per DEVICE IDENTITY (WireDepthResolveArmCache:
    // vendor, device, driver version, pipeline-cache UUID - codex closeout finding 7), and logged once
    // per identity with every reading - the line the device evidence quotes.
    //
    // THE MEASUREMENT CAN BE REPLACED, AND ONLY HERE: MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug|clean
    // hands ChooseWireDepthResolveArm a canned measurement instead of running the probe - no host
    // lane runs on a device with the defect - and the DirectVulkan.{Split,Spawn}.MsResolveBug./
    // MsFlipBug. entries force `bug` and read ResolveWireDepthStencil's arm line back from the
    // server log. The canned measurement is evaluated like a real one, so those entries go red
    // when the evaluation stops detecting the defect. MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject
    // runs the REAL probe with its render-pass resolve left unrecorded (the sentinel survives, the
    // shader control still resolves), so the DirectVulkan.{Split,Spawn}.MsResolveElide. entries go
    // red when the real recording, readback or tally stops detecting it too.
    void VulkanRenderer::ArmWireDepthResolveOrder() {
        m_wirePreferShaderDepthResolve = false;
        // ResolveWireDepthStencil's own test for the render-pass arm, less its per-format half
        // (the probe asks that per format): without it the shader pass is the only arm.
        const Bool renderPassArmAvailable = !MagmaWireForcedShaderDepthResolve() && m_wireCreateRenderPass2 &&
                                            (m_wireDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0;
        const char* knobValue = std::getenv("MGITEST_MAGMA_DEPTH_RESOLVE_PROBE");
        const WireDepthResolveProbeKnob knob = ParseWireDepthResolveProbeKnob(knobValue);
        // Memoized per DEVICE IDENTITY, not per process (codex closeout finding 7; see
        // WireDepthResolveArmCache): a second renderer on another device or driver probes for itself.
        const WireDepthResolveDeviceIdentity identity =
            MakeWireDepthResolveDeviceIdentity(m_physicalDevice.properties, renderPassArmAvailable, knob);
        static WireDepthResolveArmCache s_choices;
        const WireDepthResolveArmChoice resolved = s_choices.Resolve(identity, [&]() {
            if (knob == WireDepthResolveProbeKnob::Unrecognised)
                MGLOG_W("DirectVulkan: MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=%s is not `bug`, `clean` or "
                        "`elide-subject`; the resolve probe runs as if it were unset",
                        knobValue);
            WireDepthResolveArmChoice choice = ChooseWireDepthResolveArm(knob, renderPassArmAvailable, [&]() {
                WireDepthResolveProbeContext context;
                context.physicalDevice = m_physicalDevice.handle;
                context.device = m_device;
                context.queue = m_graphicsQueue;
                context.queueFamilyIndex = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                context.createRenderPass2 = m_wireCreateRenderPass2;
                context.depthResolveModes = m_wireDepthResolveModes;
                context.stencilResolveModes = m_wireStencilResolveModes;
                context.shaderStencilExport = m_wireShaderStencilExport;
                context.elideSubject = knob == WireDepthResolveProbeKnob::ElideSubject;
                return RunWireDepthResolveProbe(context);
            });
            if (choice.measurement.fenceWaitTimedOut) {
                // Its objects are leaked on purpose (see the PRIMITIVES_GENERATED probe above for
                // why this device must not be idle-waited on their account).
                MGLOG_W("DirectVulkan: the depth/stencil resolve probe timed out waiting on its own "
                        "submission; its Vulkan objects are deliberately leaked and the render-pass arm "
                        "keeps its place");
            } else if (choice.verdict == WireDepthResolveProbeVerdict::Inconclusive) {
                MGLOG_W("DirectVulkan: depth/stencil resolve probe verdict=inconclusive - the shader "
                        "control did not resolve the probe's inputs either, so nothing is concluded about "
                        "the render pass and the default arm order stands");
            }
            MGLOG_I("DirectVulkan: depth/stencil resolve probe (%s) verdict=%s: %s%s%s",
                    choice.measurement.fromKnob ? "forced by MGITEST_MAGMA_DEPTH_RESOLVE_PROBE"
                    : choice.measurement.subjectElided
                        ? "measured on this device with its render-pass resolve elided by "
                          "MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject"
                    : renderPassArmAvailable ? "measured on this device"
                                             : "not run: no render-pass arm",
                    WireDepthResolveProbeVerdictName(choice.verdict),
                    choice.preferShader ? "the shader pass resolves first, the no-draw render pass is the fallback"
                    : (renderPassArmAvailable || choice.measurement.fromKnob)
                        ? "the VK_KHR_depth_stencil_resolve render pass resolves first"
                        : "the shader pass is the only arm",
                    choice.measurement.failureReason.empty() ? "" : "; ", choice.measurement.failureReason.c_str());
            for (const WireDepthResolveFormatReading& reading : choice.measurement.formats)
                MGLOG_I("DirectVulkan: depth/stencil resolve probe %s",
                        DescribeWireDepthResolveFormat(reading).c_str());
            return choice;
        });
        m_wirePreferShaderDepthResolve = resolved.preferShader;
    }

    // THE RAW E5B9G9R9 READ ROUTE IS ARMED BY MEASUREMENT (WireE5RawCopy.h): Auto runs
    // RunWireE5TransferProbe once per device identity and arms the route only where a plain transfer
    // read changed words a shader read back exactly; MOBILEGL_MAGMA_E5_RAW_READS=1/0 forces it.
    void VulkanRenderer::ArmWireE5RawReads() {
        m_wireE5RawReads = false;
        const MG_Config::QuirkOverride setting = MG_Config::Features.MagmaE5RawReads;
        if (setting == MG_Config::QuirkOverride::ForceOff) {
            MGLOG_I("DirectVulkan: E5B9G9R9 raw reads off (MOBILEGL_MAGMA_E5_RAW_READS)");
            return;
        }
        if (!WireE5RawCopySupported(m_physicalDevice.handle)) {
            MGLOG_I("DirectVulkan: E5B9G9R9 raw reads unavailable (R32_UINT is not sampled + storage here)");
            return;
        }
        Bool armed = setting == MG_Config::QuirkOverride::ForceOn;
        if (setting == MG_Config::QuirkOverride::Auto) {
            struct Entry {
                WireDepthResolveDeviceIdentity identity;
                WireE5TransferProbeVerdict verdict;
            };
            static std::mutex s_mutex;
            static Vector<Entry> s_entries;
            const WireDepthResolveDeviceIdentity identity = MakeWireDepthResolveDeviceIdentity(
                m_physicalDevice.properties, false, WireDepthResolveProbeKnob::Measure);
            std::lock_guard<std::mutex> lock(s_mutex);
            const Entry* found = nullptr;
            for (const Entry& entry : s_entries)
                if (entry.identity == identity) found = &entry;
            if (found == nullptr) {
                WireE5TransferProbeContext context;
                context.physicalDevice = m_physicalDevice.handle;
                context.device = m_device;
                context.queue = m_graphicsQueue;
                context.queueFamilyIndex = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                const WireE5TransferProbeMeasurement measurement = RunWireE5TransferProbe(context);
                const WireE5TransferProbeVerdict verdict = EvaluateWireE5TransferProbe(measurement);
                if (measurement.fenceWaitTimedOut)
                    MGLOG_W("DirectVulkan: the E5B9G9R9 transfer probe timed out waiting on its own submission; "
                            "its Vulkan objects are deliberately leaked");
                MGLOG_I("DirectVulkan: E5B9G9R9 transfer probe verdict=%s: %s", WireE5TransferProbeVerdictName(verdict),
                        DescribeWireE5TransferProbe(measurement).c_str());
                s_entries.push_back({identity, verdict});
                found = &s_entries.back();
            }
            armed = found->verdict == WireE5TransferProbeVerdict::TransferReadNormalizes;
        }
        if (armed) {
            if (const VkResult result = CreateWireE5RawCopyKit(m_device, m_wireE5RawCopyKit); result != VK_SUCCESS) {
                MGLOG_W("DirectVulkan: E5B9G9R9 raw route kit creation failed (%d); transfer reads stay plain",
                        static_cast<Int>(result));
                DestroyWireE5RawCopyKit(m_device, m_wireE5RawCopyKit);
                armed = false;
            }
        }
        m_wireE5RawReads = armed;
        MGLOG_I("DirectVulkan: E5B9G9R9 reads %s", armed ? "go through the raw shader route" : "use plain transfers");
    }

    // THE PER-SLICE SPLIT IS ARMED BY MEASUREMENT (WireCopySliceProbe.h), once per device identity,
    // or forced by MOBILEGL_MAGMA_SPLIT_SLICE_COPIES.
    void VulkanRenderer::ArmWireCopySliceSplit() {
        const MG_Config::QuirkOverride setting = MG_Config::Features.MagmaSplitSliceCopies;
        Bool armed = setting == MG_Config::QuirkOverride::ForceOn;
        if (setting == MG_Config::QuirkOverride::Auto) {
            struct Entry {
                WireDepthResolveDeviceIdentity identity;
                WireCopySliceProbeVerdict verdict;
            };
            static std::mutex s_mutex;
            static Vector<Entry> s_entries;
            const WireDepthResolveDeviceIdentity identity = MakeWireDepthResolveDeviceIdentity(
                m_physicalDevice.properties, false, WireDepthResolveProbeKnob::Measure);
            std::lock_guard<std::mutex> lock(s_mutex);
            const Entry* found = nullptr;
            for (const Entry& entry : s_entries)
                if (entry.identity == identity) found = &entry;
            if (found == nullptr) {
                WireCopySliceProbeContext context;
                context.physicalDevice = m_physicalDevice.handle;
                context.device = m_device;
                context.queue = m_graphicsQueue;
                context.queueFamilyIndex = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                const WireCopySliceProbeMeasurement measurement = RunWireCopySliceProbe(context);
                const WireCopySliceProbeVerdict verdict = EvaluateWireCopySliceProbe(measurement);
                if (measurement.fenceWaitTimedOut)
                    MGLOG_W("DirectVulkan: the slice copy probe timed out waiting on its own submission; its Vulkan "
                            "objects are deliberately leaked");
                MGLOG_I("DirectVulkan: slice copy probe verdict=%s: %s", WireCopySliceProbeVerdictName(verdict),
                        DescribeWireCopySliceProbe(measurement).c_str());
                s_entries.push_back({identity, verdict});
                found = &s_entries.back();
            }
            armed = found->verdict == WireCopySliceProbeVerdict::CrossFormatSlicesDropped;
        }
        m_wireSplitCrossFormatSliceCopies = armed;
        MGLOG_I("DirectVulkan: cross-format layered <-> 3D copies %s",
                armed ? "are split one slice per region" : "go as one region");
    }

    // THE RE-EMIT AFTER A DISCARD DRAW IS ARMED BY MEASUREMENT (WireDiscardStateProbe.h), once per
    // device identity, or forced by MOBILEGL_MAGMA_REEMIT_STATE_AFTER_DISCARD.
    void VulkanRenderer::ArmWireDiscardStateReemit() {
        const MG_Config::QuirkOverride setting = MG_Config::Features.MagmaReemitStateAfterDiscard;
        Bool armed = setting == MG_Config::QuirkOverride::ForceOn;
        if (setting == MG_Config::QuirkOverride::Auto) {
            struct Entry {
                WireDepthResolveDeviceIdentity identity;
                WireDiscardStateProbeVerdict verdict;
            };
            static std::mutex s_mutex;
            static Vector<Entry> s_entries;
            const WireDepthResolveDeviceIdentity identity = MakeWireDepthResolveDeviceIdentity(
                m_physicalDevice.properties, false, WireDepthResolveProbeKnob::Measure);
            std::lock_guard<std::mutex> lock(s_mutex);
            const Entry* found = nullptr;
            for (const Entry& entry : s_entries)
                if (entry.identity == identity) found = &entry;
            if (found == nullptr) {
                WireDiscardStateProbeContext context;
                context.physicalDevice = m_physicalDevice.handle;
                context.device = m_device;
                context.queue = m_graphicsQueue;
                context.queueFamilyIndex = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                const WireDiscardStateProbeMeasurement measurement = RunWireDiscardStateProbe(context);
                const WireDiscardStateProbeVerdict verdict = EvaluateWireDiscardStateProbe(measurement);
                if (measurement.fenceWaitTimedOut)
                    MGLOG_W("DirectVulkan: the discard state probe timed out waiting on its own submission; its "
                            "Vulkan objects are deliberately leaked");
                MGLOG_I("DirectVulkan: discard state probe verdict=%s: %s", WireDiscardStateProbeVerdictName(verdict),
                        DescribeWireDiscardStateProbe(measurement).c_str());
                s_entries.push_back({identity, verdict});
                found = &s_entries.back();
            }
            armed = found->verdict == WireDiscardStateProbeVerdict::DiscardDrawDropsState;
        }
        m_wireReemitStateAfterDiscard = armed;
        MGLOG_I("DirectVulkan: dynamic state %s",
                armed ? "is set again after every rasterizer-discard draw" : "carries across discard draws");
    }

    void VulkanRenderer::ArmPrimGenReroute() {
        using namespace MG_Util::SelfTest;
        m_primGenRerouteKind = PrimGenRerouteKind::None;
        const MG_Config::QuirkOverride overrideSetting = MG_Config::Features.MagmaPrimGenQueryReroute;
        // Without stream queries the GENERATED path never opens a slot at all, so
        // there is nothing to reroute - whatever the override says.
        if (!m_xfbQueriesSupported || !m_hostQueryResetEnabled) {
            return;
        }
        const Bool primitivesGeneratedQueryUsable =
            m_primitivesGeneratedQueryFeatureEnabled && m_primitivesGeneratedQueryDiscardFeatureEnabled;
        PrimitivesGeneratedNoXfbVerdict verdict = PrimitivesGeneratedNoXfbVerdict::Inconclusive;
        // The probe only matters under Auto (ForceOn bypasses the verdict, ForceOff
        // never asks), and the answer is a device property - so it is memoized per
        // process rather than re-paid on every renderer recreation.
        if (overrideSetting == MG_Config::QuirkOverride::Auto) {
            static const PrimitivesGeneratedNoXfbMeasurement s_measurement = [&]() {
                PrimitivesGeneratedNoXfbProbeContext probeContext;
                probeContext.device = m_device;
                probeContext.queue = m_graphicsQueue;
                probeContext.queueFamilyIndex = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                probeContext.transformFeedbackQueriesUsable = m_xfbQueriesSupported;
                probeContext.primitivesGeneratedQueryUsable = primitivesGeneratedQueryUsable;
                probeContext.pipelineStatisticsEnabled = m_pipelineStatisticsQueryFeatureEnabled;
                probeContext.tessellationEnabled = m_tessellationShaderFeatureEnabled;
                auto& fns = probeContext.fns;
                fns.vkCreateCommandPool = vkCreateCommandPool;
                fns.vkDestroyCommandPool = vkDestroyCommandPool;
                fns.vkAllocateCommandBuffers = vkAllocateCommandBuffers;
                fns.vkBeginCommandBuffer = vkBeginCommandBuffer;
                fns.vkEndCommandBuffer = vkEndCommandBuffer;
                fns.vkCreateQueryPool = vkCreateQueryPool;
                fns.vkDestroyQueryPool = vkDestroyQueryPool;
                fns.vkCmdResetQueryPool = vkCmdResetQueryPool;
                fns.vkCmdBeginQuery = vkCmdBeginQuery;
                fns.vkCmdEndQuery = vkCmdEndQuery;
                fns.vkCmdBeginQueryIndexedEXT = s_vkCmdBeginQueryIndexedEXT;
                fns.vkCmdEndQueryIndexedEXT = s_vkCmdEndQueryIndexedEXT;
                fns.vkCreateRenderPass = vkCreateRenderPass;
                fns.vkDestroyRenderPass = vkDestroyRenderPass;
                fns.vkCreateFramebuffer = vkCreateFramebuffer;
                fns.vkDestroyFramebuffer = vkDestroyFramebuffer;
                fns.vkCmdBeginRenderPass = vkCmdBeginRenderPass;
                fns.vkCmdEndRenderPass = vkCmdEndRenderPass;
                fns.vkCreateShaderModule = vkCreateShaderModule;
                fns.vkDestroyShaderModule = vkDestroyShaderModule;
                fns.vkCreatePipelineLayout = vkCreatePipelineLayout;
                fns.vkDestroyPipelineLayout = vkDestroyPipelineLayout;
                fns.vkCreateGraphicsPipelines = vkCreateGraphicsPipelines;
                fns.vkDestroyPipeline = vkDestroyPipeline;
                fns.vkCmdBindPipeline = vkCmdBindPipeline;
                fns.vkCmdDraw = vkCmdDraw;
                fns.vkCreateFence = vkCreateFence;
                fns.vkDestroyFence = vkDestroyFence;
                fns.vkQueueSubmit = vkQueueSubmit;
                fns.vkWaitForFences = vkWaitForFences;
                fns.vkGetQueryPoolResults = vkGetQueryPoolResults;
                fns.vkDeviceWaitIdle = vkDeviceWaitIdle;
                return RunPrimitivesGeneratedNoXfbProbe(probeContext);
            }();
            verdict = EvaluatePrimitivesGeneratedNoXfbVerdict(s_measurement);
            if (s_measurement.fenceWaitTimedOut) {
                // The probe's submission never signaled within its bound, so it left its
                // command pool, query pools, render pass, framebuffer, shader modules,
                // pipeline layout, pipelines and fence alive on purpose. This device is the
                // renderer's own and outlives them, so nothing here may destroy them or
                // wait the device idle - the queue may still be executing that submission,
                // and an idle wait is the hang the bound exists to prevent. They leak for
                // the process's life; a device this sick has bigger problems.
                MGLOG_W("PRIMITIVES_GENERATED probe timed out waiting on its own submission (%s); its "
                        "Vulkan objects are deliberately leaked and XFB-inactive draws keep the stream "
                        "query",
                        s_measurement.failureReason.c_str());
            } else if (!s_measurement.ran) {
                MGLOG_W("PRIMITIVES_GENERATED probe did not run (%s); XFB-inactive draws keep the "
                        "stream query",
                        s_measurement.failureReason.c_str());
            } else {
                const auto logShape = [](const char* name,
                                         const MG_Util::SelfTest::PrimitivesGeneratedNoXfbShapeMeasurement& shape) {
                    MGLOG_I("PRIMITIVES_GENERATED probe %s: drawn=%d stream=%llu/%llu pgq=%llu(%d) "
                            "stat=%llu(%d)",
                            name, shape.drawn ? 1 : 0, static_cast<unsigned long long>(shape.streamGenerated),
                            static_cast<unsigned long long>(shape.expectedPrimitives),
                            static_cast<unsigned long long>(shape.primitivesGeneratedExt),
                            shape.primitivesGeneratedExtMeasured ? 1 : 0,
                            static_cast<unsigned long long>(shape.statisticsClippingInput),
                            shape.statisticsMeasured ? 1 : 0);
                };
                logShape("triangles", s_measurement.trianglesPlain);
                logShape("triangles+discard", s_measurement.trianglesDiscard);
                logShape("patches+discard", s_measurement.patchesDiscard);
            }
        }
        // A driver whose stream query counts capture-less draws counts a PAUSED span's
        // draws through the stream slot they take, so that span's result must not have
        // the frontend's CPU paused counter added on top of it either (the pre-reroute
        // accounting did exactly that, double counting every paused draw the CPU could
        // price). Measured, not assumed: the forced arms never ask the probe and leave
        // this false.
        m_primGenStreamCountsXfbInactiveDraws = verdict == PrimitivesGeneratedNoXfbVerdict::StreamCounts;
        m_primGenRerouteKind = ChoosePrimitivesGeneratedReroute(
            overrideSetting, verdict, primitivesGeneratedQueryUsable, m_pipelineStatisticsQueryFeatureEnabled);
        if (m_primGenRerouteKind != PrimGenRerouteKind::None) {
            MGLOG_I("PRIMITIVES_GENERATED for XFB-inactive draws will accumulate through a %s pool%s",
                    m_primGenRerouteKind == PrimGenRerouteKind::PrimitivesGeneratedExt
                        ? "VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT"
                        : "clipping-invocations pipeline-statistics",
                    overrideSetting == MG_Config::QuirkOverride::ForceOn ? " (forced on)" : "");
        }
    }

    void VulkanRenderer::CreateAllocator() {
        MOBILEGL_ASSERT(m_instance != VK_NULL_HANDLE, "CreateAllocator requires valid VkInstance");
        MOBILEGL_ASSERT(m_physicalDevice.handle != VK_NULL_HANDLE, "CreateAllocator requires valid physical device");
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE, "CreateAllocator requires valid VkDevice");

        if (m_allocator != nullptr) {
            return;
        }

        VmaAllocatorCreateInfo allocatorInfo{};
        VmaVulkanFunctions vulkanFunctions{};
        vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        allocatorInfo.instance = m_instance;
        allocatorInfo.physicalDevice = m_physicalDevice.handle;
        allocatorInfo.device = m_device;
        allocatorInfo.pVulkanFunctions = &vulkanFunctions;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_0;

        VK_VERIFY(vmaCreateAllocator(&allocatorInfo, &m_allocator), "vmaCreateAllocator");
    }

    void VulkanRenderer::DestroyAllocator() {
        if (m_allocator != nullptr) {
            vmaDestroyAllocator(m_allocator);
            m_allocator = nullptr;
        }
    }

    void VulkanRenderer::CreateSwapchain() {
        const VkExtent2D desiredExtent = {
            std::max<Uint32>(m_config.SurfaceWidth, 1),
            std::max<Uint32>(m_config.SurfaceHeight, 1),
        };
        m_swapchainObject.Create(m_device, m_physicalDevice.handle, m_surface,
                                 static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily),
                                 static_cast<Uint32>(m_physicalDevice.queueFamilies.presentFamily),
                                 m_config.MaxFramesInFlight, MG_Config::Features.MagmaExtraSwapchainImages,
                                 desiredExtent, m_config.SwapInterval);
        if (m_config.SwapInterval && m_config.SwapInterval != m_swapchainSwapInterval) {
            MGLOG_D("DirectVulkan: swap interval %d -> %s", *m_config.SwapInterval,
                    SwapchainObject::GetPresentModeName(m_swapchainObject.GetPresentMode()));
        }
        m_swapchainSwapInterval = m_config.SwapInterval;
        // The FragCoordYFlip variants bake this height in; it is the only input to a shader
        // module that lives outside the GL program, so the factory has to learn it here (and on
        // every recreation, which is the only way it can change).
        if (m_programFactory) {
            m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        }
    }

    void VulkanRenderer::CreateCommandPool() {
        VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = m_physicalDevice.queueFamilies.graphicsFamily;
        VK_VERIFY(vkCreateCommandPool(m_device, &createInfo, nullptr, &m_commandPool));
        MGLOG_I("Command pool created");
    }

    Bool VulkanRenderer::CreateSurface() {
        if (!m_window) {
#if defined VK_USE_PLATFORM_METAL_EXT
            m_window = reinterpret_cast<NativeWindowType>(
                CreateInternalMetalLayer(m_config.SurfaceWidth, m_config.SurfaceHeight, &m_platformDisplay));
            m_platformLibrary = reinterpret_cast<void*>(m_window);
#elif defined VK_USE_PLATFORM_ANDROID_KHR
            // EVERY ENTRY POINT HERE IS OPTIONAL, and each refusal is a `return false`, not
            // MOBILEGL_ASSERT: that macro is compiled out at the INFO level every shipping build uses,
            // and a renderer created for a WINDOW never enabled the headless extension - asking its
            // instance for vkCreateHeadlessSurfaceEXT answers null, and calling that crashed the server
            // the first time such a renderer needed an offscreen target.
            auto* createHeadlessSurface = m_headlessSurfaceSupported
                                              ? reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                                                    vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"))
                                              : nullptr;
            const OffscreenSurfaceRoute route = ChooseOffscreenSurfaceRoute(
                m_headlessSurfaceSupported, createHeadlessSurface != nullptr, m_androidSurfaceEnabled);
            if (route == OffscreenSurfaceRoute::Headless) {
                VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
                const VkResult result = createHeadlessSurface(m_instance, &sci, nullptr, &m_surface);
                if (result != VK_SUCCESS) {
                    MGLOG_E("DirectVulkan: vkCreateHeadlessSurfaceEXT failed (%d) for an offscreen target",
                            static_cast<int>(result));
                    m_surface = VK_NULL_HANDLE;
                    return false;
                }
                return true;
            }
            if (route == OffscreenSurfaceRoute::Unavailable) {
                MGLOG_E("DirectVulkan: no offscreen surface route on this instance (neither %s nor %s enabled)",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                return false;
            }
            // Windowless context on a driver without VK_EXT_headless_surface: give
            // the WSI an AImageReader's ANativeWindow. It is a real, valid producer
            // surface that is attached to no display and whose images this code never
            // acquires, which is exactly the "drawable nobody sees" the Xlib fallback
            // below builds out of an unmapped window. libmediandk is dlopen'd rather
            // than linked so a device without it degrades to a refusal instead of
            // failing to load the library at all.
            {
                void* mediaLib = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
                if (mediaLib == nullptr) {
                    MGLOG_E("DirectVulkan: libmediandk.so could not be loaded for an offscreen target");
                    return false;
                }
                using AImageReaderNewFn = int (*)(int32_t, int32_t, int32_t, int32_t, void**);
                using AImageReaderGetWindowFn = int (*)(void*, void**);
                using AImageReaderDeleteFn = void (*)(void*);
                auto* imageReaderNew = reinterpret_cast<AImageReaderNewFn>(dlsym(mediaLib, "AImageReader_new"));
                auto* imageReaderGetWindow =
                    reinterpret_cast<AImageReaderGetWindowFn>(dlsym(mediaLib, "AImageReader_getWindow"));
                auto* imageReaderDelete =
                    reinterpret_cast<AImageReaderDeleteFn>(dlsym(mediaLib, "AImageReader_delete"));
                if (imageReaderNew == nullptr || imageReaderGetWindow == nullptr || imageReaderDelete == nullptr) {
                    MGLOG_E("DirectVulkan: libmediandk.so is missing AImageReader_new/getWindow/delete");
                    dlclose(mediaLib);
                    return false;
                }

                constexpr int32_t kAndroidFormatRgba8888 = 0x1; // AIMAGE_FORMAT_RGBA_8888
                const int32_t width = static_cast<int32_t>(std::max<Uint32>(m_config.SurfaceWidth, 1));
                const int32_t height = static_cast<int32_t>(std::max<Uint32>(m_config.SurfaceHeight, 1));
                void* reader = nullptr;
                // maxImages must cover the swapchain's images; the reader never
                // acquires any, so this only sizes its buffer queue.
                const int status = imageReaderNew(width, height, kAndroidFormatRgba8888, 8, &reader);
                void* nativeWindow = nullptr;
                const int windowStatus =
                    (status == 0 && reader != nullptr) ? imageReaderGetWindow(reader, &nativeWindow) : -1;
                if (windowStatus != 0 || nativeWindow == nullptr) {
                    MGLOG_E("DirectVulkan: no AImageReader window for an offscreen target (new %d, getWindow %d)",
                            status, windowStatus);
                    if (reader != nullptr) imageReaderDelete(reader);
                    dlclose(mediaLib);
                    return false;
                }
                m_fallbackImageReader = reader;
                m_platformLibrary = mediaLib;
                m_window = reinterpret_cast<NativeWindowType>(nativeWindow);
            }
#elif defined VK_USE_PLATFORM_XLIB_KHR
            // No fall-through to Xlib: an offscreen surface never touches a window
            // system. CreateInstance() has already refused the bring-up if the loader
            // lacks the extension, so reaching here without it is a broken invariant
            // rather than a platform limitation - report it and fail, do not continue.
            auto* createHeadlessSurface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
            if (!m_headlessSurfaceSupported || createHeadlessSurface == nullptr) {
                MGLOG_F("vkCreateHeadlessSurfaceEXT is unavailable (%s reported as %s) while creating an "
                        "offscreen DirectVulkan surface",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
                        m_headlessSurfaceSupported ? "supported" : "unsupported");
                throw RuntimeError("vkCreateHeadlessSurfaceEXT is unavailable for an offscreen DirectVulkan surface");
            }
            VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
            VK_VERIFY(createHeadlessSurface(m_instance, &sci, nullptr, &m_surface),
                      "vkCreateHeadlessSurfaceEXT failed");
            return true;
#else
            auto* createHeadlessSurface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
            if (createHeadlessSurface == nullptr) {
                // Same class as the Xlib branch above: a null entry point behind
                // MOBILEGL_ASSERT is a segv on the next line in every INFO-level build.
                MGLOG_F("vkCreateHeadlessSurfaceEXT is unavailable while creating an offscreen DirectVulkan "
                        "surface (%s missing from this loader)",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
                throw RuntimeError("vkCreateHeadlessSurfaceEXT is unavailable for an offscreen DirectVulkan surface");
            }
            VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
            VK_VERIFY(createHeadlessSurface(m_instance, &sci, nullptr, &m_surface),
                      "vkCreateHeadlessSurfaceEXT failed");
            return true;
#endif
        }
#if defined VK_USE_PLATFORM_ANDROID_KHR
        auto* nativeWindow = static_cast<ANativeWindow*>(m_window);
        if (!nativeWindow) throw RuntimeError("ANativeWindowType is null");

        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = nativeWindow;
        const VkResult androidSurfaceResult = vkCreateAndroidSurfaceKHR(m_instance, &sci, nullptr, &m_surface);
        if (androidSurfaceResult != VK_SUCCESS) {
            MGLOG_E("DirectVulkan: vkCreateAndroidSurfaceKHR failed (%d)", static_cast<int>(androidSurfaceResult));
            m_surface = VK_NULL_HANDLE;
            return false;
        }
#elif defined VK_USE_PLATFORM_WIN32_KHR
        auto hwnd = static_cast<HWND>(m_window);
        MOBILEGL_ASSERT(hwnd, "HWND is null");

        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = hwnd;
        VK_VERIFY(vkCreateWin32SurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateWin32SurfaceKHR failed");
#elif defined VK_USE_PLATFORM_METAL_EXT
        MOBILEGL_ASSERT(m_window, "CAMetalLayer is null");

        VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        sci.pLayer = reinterpret_cast<const void*>(m_window);
        VK_VERIFY(vkCreateMetalSurfaceEXT(m_instance, &sci, nullptr, &m_surface), "vkCreateMetalSurfaceEXT failed");
#elif defined VK_USE_PLATFORM_XLIB_KHR
        // Reached only for a REAL on-screen window surface (a windowed desktop app,
        // retrace in window mode). Presentation to a window legitimately needs a
        // window system; offscreen requests returned above and never come here, so
        // there is no longer any path that opens a display on a caller's behalf.
        //
        // Every failure below is a real error return, not MOBILEGL_ASSERT: that macro
        // is compiled out at the INFO log level every shipping and CI build uses, so
        // asserting here meant a null Display sailed straight into the next Xlib call
        // and segfaulted - which is exactly how this presented in CI.
        if (!m_window) {
            MGLOG_F("CreateSurface: a window surface was requested with no native window");
            throw RuntimeError("CreateSurface: no native window for the Vulkan Xlib surface");
        }

        void* x11Lib = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_NOW);
        if (!x11Lib) {
            x11Lib = dlopen("libX11.so", RTLD_LOCAL | RTLD_NOW);
        }
        if (x11Lib == nullptr) {
            MGLOG_F("Failed to open libX11 (.so.6 and .so) while creating a Vulkan Xlib window surface: %s", dlerror());
            throw RuntimeError("libX11 is unavailable for the Vulkan Xlib window surface");
        }
        using XOpenDisplayFn = Display* (*)(const char*);
        using XCloseDisplayFn = int (*)(Display*);
        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        if (xOpenDisplay == nullptr || xCloseDisplay == nullptr) {
            MGLOG_F("Failed to resolve XOpenDisplay/XCloseDisplay while creating a Vulkan Xlib window surface");
            dlclose(x11Lib);
            throw RuntimeError("libX11 is missing XOpenDisplay/XCloseDisplay");
        }

        const char* displayName = std::getenv("DISPLAY");
        // Never the display this process serves (MG_Util/X11/DisplayGuard.h).
        auto* display = displayName != nullptr && MG_Util::X11::MayDialX11Display(displayName)
                            ? xOpenDisplay(displayName)
                            : nullptr;
        if (display == nullptr) {
            MGLOG_F("XOpenDisplay(%s) failed while creating a Vulkan Xlib window surface; there is no usable X "
                    "display for the requested window surface",
                    displayName != nullptr ? displayName : "<DISPLAY unset>");
            dlclose(x11Lib);
            throw RuntimeError("XOpenDisplay failed for the Vulkan Xlib window surface");
        }
        m_platformDisplay = display;
        m_platformLibrary = x11Lib;
        m_platformCloseDisplay = reinterpret_cast<void*>(xCloseDisplay);

        VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        sci.dpy = display;
        sci.window = static_cast<Window>(m_window);
        VK_VERIFY(vkCreateXlibSurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateXlibSurfaceKHR failed");
#else
        // #warning "VulkanRenderer::Initialize called on a platform which is not supported yet"
        MGLOG_W("VulkanRenderer::Initialize called on a platform which is not supported yet"); // TODO: support more
                                                                                               // platforms
#endif
        return true;
    }

    Vector<VkQueueFamilyProperties> VulkanRenderer::GetQueueFamilyFromPhysicalDevice(VkPhysicalDevice device) {
        Uint32 queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        Vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        return queueFamilies;
    }

    Int VulkanRenderer::GetQueueFamilyIndex(const Vector<VkQueueFamilyProperties>& queueFamilies,
                                            VkQueueFlagBits flag) {
        for (Uint32 i = 0; i < queueFamilies.size(); i++) {
            if (queueFamilies[i].queueFlags & flag) {
                return i;
            }
        }
        return -1;
    }

    Int VulkanRenderer::GetPresentQueueFamilyIndex(const PhysicalDevice& physicalDevice, VkSurfaceKHR surface,
                                                   const Vector<VkQueueFamilyProperties>& queueFamilies,
                                                   Int preferredFamilyIndex) {
        if (preferredFamilyIndex != -1) {
            VkBool32 supportsPresent = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice.handle, preferredFamilyIndex, surface,
                                                 &supportsPresent);
            if (supportsPresent) return preferredFamilyIndex;
        }

        for (Uint32 i = 0; i < queueFamilies.size(); i++) {
            VkBool32 supportsPresent = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice.handle, i, surface, &supportsPresent);
            if (supportsPresent) return i;
        }
        return -1;
    }

    Vector<VkExtensionProperties> VulkanRenderer::EnumerateInstanceExtensions() {
        // The two-call idiom has a race the spec explicitly allows for: the loader
        // re-scans ICDs, so the property count can GROW between the sizing call and
        // the fill call, and the fill then returns VK_INCOMPLETE having written only
        // as many entries as the caller asked for. The result is a silently TRUNCATED
        // extension list - and which extensions fall off the end is exactly as stable
        // as the loader's scan order, i.e. not at all. That is how a headless CI
        // runner could decide VK_EXT_headless_surface did not exist on one run and
        // did on the next, sending the pbuffer path into the Xlib fallback with no
        // X server to open. The sibling EnumerateDeviceExtensions below already
        // checked its second call; this one dropped the result on the floor.
        // Loop until a fill call agrees with its own sizing call.
        Vector<VkExtensionProperties> extensions;
        for (Uint32 attempt = 0; attempt < 8; ++attempt) {
            Uint32 extensionCount = 0;
            VK_VERIFY(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
            extensions.resize(extensionCount);
            if (extensionCount == 0) {
                return extensions;
            }
            const VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
            if (result == VK_SUCCESS) {
                extensions.resize(extensionCount);
                return extensions;
            }
            if (result != VK_INCOMPLETE) {
                VK_VERIFY(result, "vkEnumerateInstanceExtensionProperties failed");
                return extensions;
            }
            MGLOG_I("vkEnumerateInstanceExtensionProperties returned VK_INCOMPLETE (the loader's list grew "
                    "mid-enumeration); re-enumerating");
        }
        MGLOG_F("vkEnumerateInstanceExtensionProperties never settled; the instance extension list may be "
                "truncated and surface-extension selection is about to be made on incomplete information");
        return extensions;
    }

    Vector<VkExtensionProperties> VulkanRenderer::EnumerateDeviceExtensions(VkPhysicalDevice device) {
        Uint32 extensionCount = 0;
        VK_VERIFY(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr));
        Vector<VkExtensionProperties> extensions(extensionCount);
        VK_VERIFY(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()));
        return extensions;
    }

    Bool VulkanRenderer::IsExtensionSupported(const Vector<VkExtensionProperties>& availableExtensions,
                                              const char* extensionName) {
        for (const auto& extension : availableExtensions) {
            if (strcmp(extension.extensionName, extensionName) == 0) {
                return true;
            }
        }
        return false;
    }

    Bool VulkanRenderer::IsExtensionAlreadyEnabled(const Vector<const char*>& enabledExtensions,
                                                   const char* extensionName) {
        return std::any_of(enabledExtensions.begin(), enabledExtensions.end(),
                           [&extensionName](const String& name) { return name == extensionName; });
    }

    Bool VulkanRenderer::EnableOptionalDeviceExtension(const Vector<VkExtensionProperties>& availableExtensions,
                                                       Vector<const char*>& inOutEnabledExtensions,
                                                       const char* extensionName) {
        if (!IsExtensionSupported(availableExtensions, extensionName)) {
            MGLOG_I("Optional device extension not supported: %s", extensionName);
            return false;
        }

        if (!IsExtensionAlreadyEnabled(inOutEnabledExtensions, extensionName)) {
            inOutEnabledExtensions.push_back(extensionName);
        }
        MGLOG_I("Enabled optional device extension: %s", extensionName);
        return true;
    }

    void VulkanRenderer::ResolveOptionalDeviceExtensions(const Vector<VkExtensionProperties>& availableExtensions,
                                                         Vector<const char*>& inOutEnabledExtensions) {
        m_drawIndirectCountExtensionEnabled = EnableOptionalDeviceExtension(availableExtensions, inOutEnabledExtensions,
                                                                            VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
        m_shaderDrawParametersExtensionEnabled = EnableOptionalDeviceExtension(
            availableExtensions, inOutEnabledExtensions, VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        EnableOptionalDeviceExtension(availableExtensions, inOutEnabledExtensions,
                                      VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
    }

    Bool VulkanRenderer::CheckValidationLayerSupport() {
        Uint32 layerCount = 0;
        VK_VERIFY(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));

        Vector<VkLayerProperties> layers(layerCount);
        VK_VERIFY(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()));

        for (const char* layerName : s_validationLayerNames) {
            for (const auto& layerProperties : layers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    return true;
                }
            }
        }
        return false;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
