//
// Created by BZLZHH on 2025/3/30.
//

#ifndef MOBILEGL_EGL_VK_EMU_H
#define MOBILEGL_EGL_VK_EMU_H

#include "../../../../Includes.h"

#if BACKEND_TYPE == BACKEND_VULKAN

struct EGLInternalConfig {
    VkFormat format;
    VkColorSpaceKHR colorSpace;
    uint32_t samples;
};

struct VulkanDisplay {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    uint32_t graphicsQueueFamily;
    VkQueue graphicsQueue;
    std::vector<EGLInternalConfig> configs;
    bool initialized = false;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;
    VkRenderPass defaultRenderPass;
    VkQueue presentQueue = VK_NULL_HANDLE;
};

struct VulkanSurface {
    enum Type { WINDOW, PBUFFER } type;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    VkExtent2D extent;
    VkFormat format;
    VkImage pbImage;
    VkImageView pbImageView;
    VkDeviceMemory pbMemory;
    VkFramebuffer framebuffer;
    VkImage pbufferImage;
    VkImageView pbufferImageView;
};

struct VulkanContext {
    VkCommandPool cmdPool;
    std::vector<VkCommandBuffer> cmdBuffers;
    uint32_t currentFrame = 0;
    VkRenderPass renderPass;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;
    std::vector<VkFence> frameFences;
    std::vector<VkSemaphore> acquireSemaphores;
    std::vector<VkSemaphore> presentSemaphores;
    VulkanSurface* boundSurface = nullptr;
    VkCommandBuffer currentCmdBuffer;
    uint32_t frameIndex;
    VkClearColorValue clearColor;
};

extern std::atomic<uintptr_t> g_nextContextId;
extern std::atomic<uintptr_t> g_nextSurfaceId;
extern EGLenum g_currentAPI;

extern std::mutex g_globalMutex;
extern ankerl::unordered_map<EGLDisplay, VulkanDisplay> g_displays;
extern ankerl::unordered_map<EGLSurface, VulkanSurface> g_surfaces;
extern ankerl::unordered_map<EGLContext, VulkanContext> g_contexts;
extern EGLint g_lastError;

extern thread_local EGLContext tls_currentContext;
extern thread_local EGLSurface tls_drawSurface;
extern thread_local EGLSurface tls_readSurface;

MG_EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window, const EGLint* attrib_list);
MG_EXPORT EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config);
MG_EXPORT EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list);
MG_EXPORT EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
MG_EXPORT EGLDisplay eglGetDisplay(NativeDisplayType display);
MG_EXPORT EGLint eglGetError();
MG_EXPORT EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
MG_EXPORT EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
MG_EXPORT EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
MG_EXPORT EGLBoolean eglTerminate(EGLDisplay dpy);
MG_EXPORT EGLBoolean eglReleaseThread(void);
MG_EXPORT EGLContext eglGetCurrentContext(void);
MG_EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value);
MG_EXPORT EGLBoolean eglBindAPI(EGLenum api);
MG_EXPORT EGLSurface eglGetCurrentSurface(EGLint readdraw);
MG_EXPORT EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint *value);
MG_EXPORT EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval);
MG_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface draw);
MG_EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list);

#endif

#endif //MOBILEGL_EGL_VK_EMU_H
