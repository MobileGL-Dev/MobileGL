//
// Created by BZLZHH on 2025/3/30.
//

#include "EGL_EMU.h"

#if BACKEND_TYPE == BACKEND_VULKAN

std::atomic<uintptr_t> g_nextContextId{1};
std::atomic<uintptr_t> g_nextSurfaceId{1};
EGLenum g_currentAPI = EGL_OPENGL_ES_API;

std::mutex g_globalMutex;
ankerl::unordered_map<EGLDisplay, VulkanDisplay> g_displays;
ankerl::unordered_map<EGLSurface, VulkanSurface> g_surfaces;
ankerl::unordered_map<EGLContext, VulkanContext> g_contexts;
EGLint g_lastError = EGL_SUCCESS;

thread_local EGLContext tls_currentContext = EGL_NO_CONTEXT;
thread_local EGLSurface tls_drawSurface = EGL_NO_SURFACE;
thread_local EGLSurface tls_readSurface = EGL_NO_SURFACE;

static VkResult CreateRenderPass(VulkanDisplay& display, VulkanContext& ctx) {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_B8G8R8A8_SRGB;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttachment;

    return vkCreateRenderPass(display.device, &rpInfo, nullptr, &ctx.renderPass);
}

EGLDisplay eglGetDisplay(NativeDisplayType display) {
    std::lock_guard<std::mutex> lock(g_globalMutex);
    static uintptr_t nextDisplayId = 1;
    EGLDisplay newDisplay = (EGLDisplay)nextDisplayId++;
    g_displays[newDisplay].initialized = false;
    return newDisplay;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    VulkanDisplay& vkDpy = g_displays[dpy];
    if (vkDpy.initialized) return EGL_TRUE;

    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    instInfo.enabledExtensionCount = 2;
    instInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&instInfo, nullptr, &vkDpy.instance) != VK_SUCCESS) {
        g_lastError = EGL_NOT_INITIALIZED;
        return EGL_FALSE;
    }

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(vkDpy.instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> devices(devCount);
    vkEnumeratePhysicalDevices(vkDpy.instance, &devCount, devices.data());
    vkDpy.physicalDevice = devices[0];


    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vkDpy.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(vkDpy.physicalDevice, &queueFamilyCount, queueFamilies.data());

    // 寻找支持图形和呈现的队列家族
    bool foundQueue = false;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vkDpy.physicalDevice, i, VK_NULL_HANDLE, &presentSupport);

        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport) {
            vkDpy.graphicsQueueFamily = i;
            foundQueue = true;
            break;
        }
    }

    if (!foundQueue) {
        g_lastError = EGL_NOT_INITIALIZED;
        return EGL_FALSE;
    }

    // 创建逻辑设备时请求队列
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = vkDpy.graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    const char* devExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo devInfo{};
    devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devInfo.pQueueCreateInfos = &queueInfo;
    devInfo.queueCreateInfoCount = 1;
    devInfo.ppEnabledExtensionNames = devExtensions;
    devInfo.enabledExtensionCount = 1;

    if (vkCreateDevice(vkDpy.physicalDevice, &devInfo, nullptr, &vkDpy.device) != VK_SUCCESS) {
        g_lastError = EGL_NOT_INITIALIZED;
        return EGL_FALSE;
    }

    vkDpy.vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(vkDpy.device, "vkCreateSwapchainKHR");
    vkDpy.vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(vkDpy.device, "vkDestroySwapchainKHR");
    vkDpy.vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(vkDpy.device, "vkGetSwapchainImagesKHR");
    vkDpy.vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(vkDpy.device, "vkAcquireNextImageKHR");
    vkDpy.vkQueuePresentKHR = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(vkDpy.device, "vkQueuePresentKHR");
    vkGetDeviceQueue(vkDpy.device, vkDpy.graphicsQueueFamily, 0, &vkDpy.graphicsQueue);
    vkDpy.presentQueue = vkDpy.graphicsQueue;
    vkDpy.initialized = true;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
    VulkanDisplay& vkDpy = g_displays[dpy];
    EGLInternalConfig defaultConfig{ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 1 };
    vkDpy.configs = {defaultConfig};

    EGLint returnCount = 1;
    if (configs && config_size > 0) {
        configs[0] = (EGLConfig)0;
    }

    if (num_config) *num_config = returnCount;
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window, const EGLint* attrib_list) {
    VulkanDisplay& disp = g_displays[dpy];
    static uintptr_t nextSurfId = 1;

    ANativeWindow* nativeWindow = static_cast<ANativeWindow*>(window);
    if (!nativeWindow || ANativeWindow_getWidth(nativeWindow) <= 0 || ANativeWindow_getHeight(nativeWindow) <= 0) {
        g_lastError = EGL_BAD_NATIVE_WINDOW;
        return EGL_NO_SURFACE;
    }

    auto vkCreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(
            disp.instance, "vkCreateAndroidSurfaceKHR");
    if (!vkCreateAndroidSurfaceKHR) {
        g_lastError = EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }

    VulkanSurface surface{};
    surface.type = VulkanSurface::WINDOW;

    VkAndroidSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.window = nativeWindow;
    VkResult result = vkCreateAndroidSurfaceKHR(disp.instance, &createInfo, nullptr, &surface.surface);
    if (result != VK_SUCCESS) {
        g_lastError = (result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) ?
                      EGL_BAD_NATIVE_WINDOW : EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(disp.physicalDevice,
                                         disp.graphicsQueueFamily,
                                         surface.surface,
                                         &supported);
    if (!supported) {
        vkDestroySurfaceKHR(disp.instance, surface.surface, nullptr);
        g_lastError = EGL_BAD_MATCH;
        return EGL_NO_SURFACE;
    }

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(disp.physicalDevice, surface.surface, &caps);

    const VkExtent2D swapchainExtent = {
            (std::max)(caps.minImageExtent.width,  (uint32_t)ANativeWindow_getWidth(nativeWindow)),
            (std::max)(caps.minImageExtent.height, (uint32_t)ANativeWindow_getHeight(nativeWindow))
    };

    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = surface.surface;
    swapInfo.minImageCount = 3;
    swapInfo.imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
    swapInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapInfo.imageExtent = swapchainExtent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

    if (disp.vkCreateSwapchainKHR(disp.device, &swapInfo, nullptr, &surface.swapchain) != VK_SUCCESS) {
        vkDestroySurfaceKHR(disp.instance, surface.surface, nullptr);
        g_lastError = EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }

    uint32_t imageCount = 0;
    disp.vkGetSwapchainImagesKHR(disp.device, surface.swapchain, &imageCount, nullptr);
    surface.images.resize(imageCount);
    disp.vkGetSwapchainImagesKHR(disp.device, surface.swapchain, &imageCount, surface.images.data());

    surface.imageViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = surface.images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapInfo.imageFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(disp.device, &viewInfo, nullptr, &surface.imageViews[i]) != VK_SUCCESS) {
            // Handle error
        }
    }

    ANativeWindow_acquire(nativeWindow);
    ANativeWindow_setBuffersGeometry(nativeWindow,
                                     swapchainExtent.width,
                                     swapchainExtent.height,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    EGLSurface surfId = (EGLSurface)nextSurfId++;
    g_surfaces[surfId] = std::move(surface);
    return surfId;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list) {
    static uintptr_t nextCtxId = 1;
    VulkanDisplay &disp = g_displays[dpy];

    VulkanContext ctx{};
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = 0;
    if (vkCreateCommandPool(disp.device, &poolInfo, nullptr, &ctx.cmdPool) != VK_SUCCESS) {
        g_lastError = EGL_BAD_ALLOC;
        return EGL_NO_CONTEXT;
    }

    ctx.frameFences.resize(2);
    ctx.acquireSemaphores.resize(2);
    ctx.presentSemaphores.resize(2);
    for (uint32_t i = 0; i < 2; ++i) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                    VK_FENCE_CREATE_SIGNALED_BIT};
        vkCreateFence(disp.device, &fenceInfo, nullptr, &ctx.frameFences[i]);

        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(disp.device, &semInfo, nullptr, &ctx.acquireSemaphores[i]);
        vkCreateSemaphore(disp.device, &semInfo, nullptr, &ctx.presentSemaphores[i]);
    }

    if (CreateRenderPass(disp, ctx) != VK_SUCCESS) {
        g_lastError = EGL_BAD_CONFIG;
        return EGL_NO_CONTEXT;
    }

    EGLContext ctxId = (EGLContext)nextCtxId++;
    g_contexts[ctxId] = std::move(ctx);
    return ctxId;
}

EGLint eglGetError() {
    return EGL_SUCCESS;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    std::lock_guard<std::mutex> lock(g_globalMutex);

    if (dpy == EGL_NO_DISPLAY || (ctx != EGL_NO_CONTEXT && !g_contexts.count(ctx)) ||
        (draw != EGL_NO_SURFACE && !g_surfaces.count(draw)) ||
        (read != EGL_NO_SURFACE && !g_surfaces.count(read))) {
        g_lastError = EGL_BAD_PARAMETER;
        return EGL_FALSE;
    }

    if (tls_currentContext != EGL_NO_CONTEXT) {
        VulkanContext& oldCtx = g_contexts[tls_currentContext];
        if (oldCtx.currentCmdBuffer) {
            vkEndCommandBuffer(oldCtx.currentCmdBuffer);
            oldCtx.currentCmdBuffer = VK_NULL_HANDLE;
        }
    }

    if (ctx != EGL_NO_CONTEXT) {
        VulkanContext& newCtx = g_contexts[ctx];
        VulkanDisplay& disp = g_displays[dpy];

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = newCtx.cmdPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(disp.device, &allocInfo, &newCtx.currentCmdBuffer) != VK_SUCCESS) {
            g_lastError = EGL_BAD_ALLOC;
            return EGL_FALSE;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(newCtx.currentCmdBuffer, &beginInfo);
    }

    tls_currentContext = ctx;
    tls_drawSurface = draw;
    tls_readSurface = read;
    return EGL_TRUE;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    std::lock_guard<std::mutex> lock(g_globalMutex);

    if (!g_contexts.count(ctx)) {
        g_lastError = EGL_BAD_CONTEXT;
        return EGL_FALSE;
    }

    VulkanContext& vkCtx = g_contexts[ctx];
    VulkanDisplay& disp = g_displays[dpy];

    vkDeviceWaitIdle(disp.device);

    vkDestroyCommandPool(disp.device, vkCtx.cmdPool, nullptr);
    vkDestroyRenderPass(disp.device, vkCtx.renderPass, nullptr);

    for (auto& fence : vkCtx.frameFences)
        vkDestroyFence(disp.device, fence, nullptr);
    for (auto& sem : vkCtx.acquireSemaphores)
        vkDestroySemaphore(disp.device, sem, nullptr);
    for (auto& sem : vkCtx.presentSemaphores)
        vkDestroySemaphore(disp.device, sem, nullptr);

    g_contexts.erase(ctx);
    if (tls_currentContext == ctx)
        tls_currentContext = EGL_NO_CONTEXT;

    return EGL_TRUE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    std::lock_guard<std::mutex> lock(g_globalMutex);

    if (!g_surfaces.count(surface)) {
        g_lastError = EGL_BAD_SURFACE;
        return EGL_FALSE;
    }

    VulkanSurface& surf = g_surfaces[surface];
    VulkanDisplay& disp = g_displays[dpy];

    vkDeviceWaitIdle(disp.device);

    if (surf.type == VulkanSurface::WINDOW) {
        disp.vkDestroySwapchainKHR(disp.device, surf.swapchain, nullptr);
        vkDestroySurfaceKHR(disp.instance, surf.surface, nullptr);
        for (auto& view : surf.imageViews)
            vkDestroyImageView(disp.device, view, nullptr);
    } else {
        vkDestroyImageView(disp.device, surf.pbufferImageView, nullptr);
        vkDestroyImage(disp.device, surf.pbufferImage, nullptr);
    }

    g_surfaces.erase(surface);
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    std::lock_guard<std::mutex> lock(g_globalMutex);

    if (!g_displays.count(dpy)) {
        g_lastError = EGL_BAD_DISPLAY;
        return EGL_FALSE;
    }

    VulkanDisplay& disp = g_displays[dpy];

    for (auto& [ctxId, ctx] : g_contexts)
        eglDestroyContext(dpy, ctxId);
    for (auto& [surfId, surf] : g_surfaces)
        eglDestroySurface(dpy, surfId);

    vkDestroyDevice(disp.device, nullptr);
    vkDestroyInstance(disp.instance, nullptr);

    g_displays.erase(dpy);
    return EGL_TRUE;
}

EGLBoolean eglReleaseThread(void) {
    tls_currentContext = EGL_NO_CONTEXT;
    tls_drawSurface = EGL_NO_SURFACE;
    tls_readSurface = EGL_NO_SURFACE;
    return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void) {
    return tls_currentContext;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    VulkanDisplay& disp = g_displays[dpy];
    if (!disp.configs.empty() && config == (EGLConfig)0) {
        switch (attribute) {
            case EGL_BUFFER_SIZE:  *value = 32; break;
            case EGL_RED_SIZE:      *value = 8;  break;
            case EGL_GREEN_SIZE:    *value = 8;  break;
            case EGL_BLUE_SIZE:     *value = 8;  break;
            case EGL_ALPHA_SIZE:    *value = 8;  break;
            case EGL_CONFIG_ID:     *value = 1;  break;
            default: return EGL_FALSE;
        }
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

EGLBoolean eglBindAPI(EGLenum api) {
    if (api == EGL_OPENGL_ES_API) {
        g_currentAPI = api;
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
    VulkanDisplay& disp = g_displays[dpy];
    VulkanSurface surface{};
    surface.type = VulkanSurface::PBUFFER;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imgInfo.extent = {512, 512, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(disp.device, &imgInfo, nullptr, &surface.pbufferImage) != VK_SUCCESS) {
        g_lastError = EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = surface.pbufferImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imgInfo.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(disp.device, &viewInfo, nullptr, &surface.pbufferImageView) != VK_SUCCESS) {
        vkDestroyImage(disp.device, surface.pbufferImage, nullptr);
        g_lastError = EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }

    EGLSurface surfId = (EGLSurface)g_nextSurfaceId++;
    g_surfaces[surfId] = surface;
    return surfId;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface draw) {
    VulkanSurface& surf = g_surfaces[draw];
    VulkanDisplay& disp = g_displays[dpy];
    VulkanContext& ctx = g_contexts[tls_currentContext];
    
    if (!disp.presentQueue) {
        g_lastError = EGL_BAD_DISPLAY;
        return EGL_FALSE;
    }
    
    if (surf.type != VulkanSurface::WINDOW) {
        g_lastError = EGL_BAD_SURFACE;
        return EGL_FALSE;
    }

    vkEndCommandBuffer(ctx.currentCmdBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &ctx.currentCmdBuffer;

    vkQueueSubmit(disp.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &surf.swapchain;
    presentInfo.pImageIndices = &ctx.frameIndex;

    VkResult result = disp.vkQueuePresentKHR(disp.presentQueue, &presentInfo);
    if (result != VK_SUCCESS) {
        g_lastError = EGL_BAD_ALLOC;
        return EGL_FALSE;
    }

    ctx.frameIndex = (ctx.frameIndex + 1) % surf.images.size();
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    // TODO
    return EGL_TRUE;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    return (readdraw == EGL_READ) ? tls_readSurface : tls_drawSurface;
}

EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint *value) {
    VulkanSurface& surf = g_surfaces[surface];
    switch (attribute) {
        case EGL_WIDTH:  *value = surf.extent.width;  break;
        case EGL_HEIGHT: *value = surf.extent.height; break;
        case EGL_CONFIG_ID: *value = 1; break;
        default: return EGL_FALSE;
    }
    return EGL_TRUE;
}

#endif