// MobileGL - MobileGL/MG_Util/SelfTest/DriverPost.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DriverPost.h"
#include "MG_Util/BackendLoaders/OpenGL/Loader.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        struct ReportBuilder {
            BackendPostReport report;
            Bool fatalFailed = false;
            Bool warnUnmet = false;

            void Pass(String name, String detail) { report.checks.push_back({Move(name), "PASS", Move(detail)}); }

            void Fail(String name, String detail) {
                fatalFailed = true;
                report.checks.push_back({Move(name), "FAIL", Move(detail)});
            }

            void Warn(String name, String detail) {
                warnUnmet = true;
                report.checks.push_back({Move(name), "WARN", Move(detail)});
            }

            void Info(String name, String detail) { report.checks.push_back({Move(name), "INFO", Move(detail)}); }

            void Finalize() { report.verdict = fatalFailed ? "UNSUPPORTED" : (warnUnmet ? "DEGRADED" : "OK"); }
        };

        // Runs a callable when the enclosing scope exits, so driver teardown still happens
        // even if a String/format allocation throws while report rows are being built.
        template <typename Callable>
        struct ScopeGuard {
            explicit ScopeGuard(Callable callable) : onExit(Move(callable)) {}
            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;
            ~ScopeGuard() { onExit(); }

        private:
            Callable onExit;
        };

        String EGLErrorSuffix(const MG_External::EGLFunctionsTable& eglFuncs) {
            if (!eglFuncs.eglGetError) {
                return "";
            }
            return format(" (EGL error 0x{:x})", eglFuncs.eglGetError());
        }

        void EvaluateGlesChecklist(ReportBuilder& builder, const MG_External::GLESCapabilities& caps,
                                   const MG_External::GLESFunctionsTable& glesFuncs) {
            const Int major = caps.GLESVersion.Major;
            const Int minor = caps.GLESVersion.Minor;
            const Bool es31 = major > 3 || (major == 3 && minor >= 1);
            const Bool es32 = major > 3 || (major == 3 && minor >= 2);
            const String versionLabel = format("OpenGL ES {}.{}", major, minor);
            if (es32) {
                builder.Pass("OpenGL ES version", versionLabel + " (>= 3.2, full native feature set)");
            } else if (es31) {
                builder.Warn("OpenGL ES version",
                             versionLabel +
                                 " (compute shaders and native indirect draws available; ES 3.2 is recommended)");
            } else {
                builder.Fail("OpenGL ES version",
                             versionLabel + " (< 3.1: no compute shaders or native indirect draws)");
            }

            if (es31) {
                GLint maxVertexSsboBlocks = 0;
                glesFuncs.glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSsboBlocks);
                while (glesFuncs.glGetError && glesFuncs.glGetError() != GL_NO_ERROR) {
                }
                if (maxVertexSsboBlocks >= 1) {
                    builder.Pass("Vertex shader storage blocks",
                                 format("GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = {}", maxVertexSsboBlocks));
                } else {
                    builder.Warn("Vertex shader storage blocks",
                                 format("GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = {}; the Flywheel/Create indirect draw "
                                        "machinery cannot read indirect command buffers from the vertex stage",
                                        maxVertexSsboBlocks));
                }

                if (caps.MaxShaderStorageBufferBindings >= 8) {
                    builder.Pass("Shader storage buffer bindings",
                                 format("GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = {} (the last binding is reserved "
                                        "for mg_IndirectParams)",
                                        caps.MaxShaderStorageBufferBindings));
                } else {
                    builder.Warn("Shader storage buffer bindings",
                                 format("GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = {} (< 8); reserving the last "
                                        "binding for mg_IndirectParams leaves little room for app SSBOs",
                                        caps.MaxShaderStorageBufferBindings));
                }
            }

            builder.Info("GL_EXT_buffer_storage",
                         caps.SupportsPersistentMapping ? "supported (persistent buffer mapping)" : "not supported");
            builder.Info("GL_EXT_base_instance",
                         caps.SupportsBaseInstance ? "supported (native baseInstance draws)" : "not supported");
            if (caps.SupportsNorm16Texture) {
                builder.Pass("GL_EXT_texture_norm16", "supported");
            } else {
                builder.Warn("GL_EXT_texture_norm16",
                             "not supported; 16-bit normalized texture formats need emulation");
            }

            builder.Info("Indirect gl_InstanceID semantics",
                         caps.IndirectDrawInstanceIdIncludesBaseInstance
                             ? "includes baseInstance (ANGLE-style; MobileGL's shader rewrite keeps gl_InstanceID "
                               "zero-based)"
                             : "conforming (zero-based)");

            builder.Info("GL_VENDOR", caps.GLESVendorString);
            builder.Info("GL_RENDERER", caps.GLESRendererString);
            builder.Info("GL_VERSION", caps.GLESVersionString);
        }
    } // namespace

    BackendPostReport RunGlesDriverPost() {
        MGLOG_I("Driver POST: probing the device GLES driver");
        ReportBuilder builder;

        MG_External::EGLFunctionsTable eglFuncs{};
        BackendLoader::AcquireEGLFunctions(eglFuncs);
        const Bool eglLoaded = eglFuncs.eglGetDisplay && eglFuncs.eglInitialize && eglFuncs.eglBindAPI &&
                               eglFuncs.eglChooseConfig && eglFuncs.eglCreatePbufferSurface &&
                               eglFuncs.eglCreateContext && eglFuncs.eglMakeCurrent && eglFuncs.eglDestroySurface &&
                               eglFuncs.eglDestroyContext && eglFuncs.eglTerminate && eglFuncs.eglGetProcAddress;
        if (!eglLoaded) {
            builder.Fail("EGL library", "libEGL.so or one of its required entry points is missing");
            builder.Finalize();
            return builder.report;
        }
        builder.Pass("EGL library", "libEGL.so loaded with all required entry points");

        EGLDisplay display = eglFuncs.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            builder.Fail("EGL display", "eglGetDisplay returned EGL_NO_DISPLAY");
            builder.Finalize();
            return builder.report;
        }
        EGLint eglMajor = 0;
        EGLint eglMinor = 0;
        if (!eglFuncs.eglInitialize(display, &eglMajor, &eglMinor)) {
            builder.Fail("EGL display",
                         "eglInitialize failed on the default display" + EGLErrorSuffix(eglFuncs));
            builder.Finalize();
            return builder.report;
        }
        builder.Pass("EGL display", format("EGL {}.{} initialized on the default display", eglMajor, eglMinor));
        builder.report.available = true;

        EGLSurface surface = EGL_NO_SURFACE;
        EGLContext context = EGL_NO_CONTEXT;
        const ScopeGuard eglTeardown([&]() {
            eglFuncs.eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) {
                eglFuncs.eglDestroySurface(display, surface);
            }
            if (context != EGL_NO_CONTEXT) {
                eglFuncs.eglDestroyContext(display, context);
            }
            // eglTerminate is deliberately not called: the probe shares EGL_DEFAULT_DISPLAY with
            // the process UI renderer (HWUI), and terminating it can invalidate the UI's EGL
            // objects on pre-refcounting Android builds. Unbinding and destroying our own
            // surface/context is sufficient cleanup.
        });
        do {
            if (!eglFuncs.eglBindAPI(EGL_OPENGL_ES_API)) {
                builder.Fail("OpenGL ES API bind", "eglBindAPI(EGL_OPENGL_ES_API) failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            builder.Pass("OpenGL ES API bind", "eglBindAPI(EGL_OPENGL_ES_API) succeeded");

            const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                            EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,      8,
                                            EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,      8,
                                            EGL_NONE};
            EGLConfig config = nullptr;
            EGLint numConfigs = 0;
            if (!eglFuncs.eglChooseConfig(display, configAttribs, &config, 1, &numConfigs)) {
                builder.Fail("ES3 RGBA8888 pbuffer config", "eglChooseConfig failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            if (numConfigs < 1) {
                // No EGL error suffix here: eglChooseConfig succeeded, so it would read EGL_SUCCESS.
                builder.Fail("ES3 RGBA8888 pbuffer config", "no ES3-capable RGBA8888 pbuffer config");
                break;
            }
            builder.Pass("ES3 RGBA8888 pbuffer config", "ES3-renderable RGBA8888 pbuffer config found");

            const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            surface = eglFuncs.eglCreatePbufferSurface(display, config, surfaceAttribs);
            if (surface == EGL_NO_SURFACE) {
                builder.Fail("1x1 pbuffer surface",
                             "eglCreatePbufferSurface failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            builder.Pass("1x1 pbuffer surface", "probe surface created");

            const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            context = eglFuncs.eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
            if (context == EGL_NO_CONTEXT) {
                builder.Fail("OpenGL ES 3 context", "eglCreateContext failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            if (!eglFuncs.eglMakeCurrent(display, surface, surface, context)) {
                builder.Fail("OpenGL ES 3 context", "eglMakeCurrent failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            builder.Pass("OpenGL ES 3 context", "context created and made current");

            MG_External::GLESFunctionsTable glesFuncs{};
            BackendLoader::AcquireGLESFunctions(glesFuncs, eglFuncs.eglGetProcAddress);
            MG_External::GLESCapabilities caps{};
            if (!BackendLoader::FillInGLESCapabilities(caps, glesFuncs)) {
                builder.Fail("GLES capability query",
                             "required GLES entry points could not be resolved through eglGetProcAddress");
                break;
            }
            builder.report.rendererInfo = format("{} ({})", caps.GLESRendererString, caps.GLESVersionString);
            EvaluateGlesChecklist(builder, caps, glesFuncs);
        } while (false);

        builder.Finalize();
        MGLOG_I("Driver POST: GLES verdict = %s", builder.report.verdict.c_str());
        return builder.report;
    }

    namespace {
        // The Vulkan loader is bootstrapped through dlopen + vkGetInstanceProcAddr instead of
        // static linking so the POST also works in build configurations that do not link a
        // Vulkan loader (and degrades gracefully when the device ships none). The library
        // handle is intentionally never closed: Android Vulkan ICDs may register threads and
        // state that do not survive unloading, and the loader stays resident for the real
        // backend anyway.
        void* OpenVulkanLoaderLibrary() {
#if defined(_WIN32)
            return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#else
            static const char* const LoaderNames[] = {
#if defined(__APPLE__)
                "libvulkan.dylib",
                "libvulkan.1.dylib",
                "libMoltenVK.dylib",
#else
                "libvulkan.so.1",
                "libvulkan.so",
#endif
            };
            for (const char* name : LoaderNames) {
                if (void* library = dlopen(name, RTLD_LOCAL | RTLD_NOW)) {
                    MGLOG_I("Driver POST: loaded Vulkan loader library: %s", name);
                    return library;
                }
            }
            return nullptr;
#endif
        }

        void* VulkanLoaderSymbol(void* library, const char* name) {
#if defined(_WIN32)
            return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
            return dlsym(library, name);
#endif
        }

        Bool HasVkExtension(const Vector<VkExtensionProperties>& extensions, const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        }

        String VkApiVersionToString(Uint32 version) {
            return format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                          VK_VERSION_PATCH(version));
        }
    } // namespace

    BackendPostReport RunVulkanDriverPost() {
        MGLOG_I("Driver POST: probing the device Vulkan driver");
        ReportBuilder builder;

        void* loaderLibrary = OpenVulkanLoaderLibrary();
        if (loaderLibrary == nullptr) {
            builder.Fail("Vulkan loader", "libvulkan.so could not be loaded; no Vulkan loader on this device");
            builder.Finalize();
            return builder.report;
        }
        const auto getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(VulkanLoaderSymbol(loaderLibrary, "vkGetInstanceProcAddr"));
        if (getInstanceProcAddr == nullptr) {
            builder.Fail("Vulkan loader", "vkGetInstanceProcAddr is missing from the Vulkan loader library");
            builder.Finalize();
            return builder.report;
        }
        builder.Pass("Vulkan loader", "Vulkan loader library loaded and vkGetInstanceProcAddr resolved");

        const auto vkCreateInstanceFn =
            reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(nullptr, "vkCreateInstance"));
        const auto vkEnumerateInstanceVersionFn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            getInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        const auto vkEnumerateInstanceExtensionPropertiesFn =
            reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                getInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));

        Uint32 instanceApiVersion = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersionFn != nullptr) {
            vkEnumerateInstanceVersionFn(&instanceApiVersion);
        }
        if (vkCreateInstanceFn == nullptr || vkEnumerateInstanceVersionFn == nullptr ||
            instanceApiVersion < VK_API_VERSION_1_1) {
            builder.Fail("Instance API version",
                         format("instance API {} (< 1.1); the DirectVulkan backend requires a Vulkan 1.1 instance",
                                VkApiVersionToString(instanceApiVersion)));
            builder.Finalize();
            return builder.report;
        }
        builder.Pass("Instance API version", format("instance API {}", VkApiVersionToString(instanceApiVersion)));

        Vector<VkExtensionProperties> instanceExtensions;
        if (vkEnumerateInstanceExtensionPropertiesFn != nullptr) {
            Uint32 extensionCount = 0;
            if (vkEnumerateInstanceExtensionPropertiesFn(nullptr, &extensionCount, nullptr) == VK_SUCCESS &&
                extensionCount > 0) {
                instanceExtensions.resize(extensionCount);
                if (vkEnumerateInstanceExtensionPropertiesFn(nullptr, &extensionCount, instanceExtensions.data()) ==
                    VK_SUCCESS) {
                    instanceExtensions.resize(extensionCount);
                } else {
                    instanceExtensions.clear();
                }
            }
        }
        if (HasVkExtension(instanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME)) {
            builder.Pass("VK_KHR_surface", "instance extension present");
        } else {
            builder.Fail("VK_KHR_surface", "required instance extension missing; on-screen rendering is impossible");
        }
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (HasVkExtension(instanceExtensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
            builder.Pass("VK_KHR_android_surface", "instance extension present");
        } else {
            builder.Fail("VK_KHR_android_surface",
                         "required instance extension missing; ANativeWindow surfaces cannot be created");
        }
#endif

        // The probe never creates a surface, so the instance is created without extensions.
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "MobileGL Driver POST";
        appInfo.pEngineName = "MobileGL";
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        VkInstance instance = VK_NULL_HANDLE;
        const VkResult createResult = vkCreateInstanceFn(&instanceInfo, nullptr, &instance);
        if (createResult != VK_SUCCESS || instance == VK_NULL_HANDLE) {
            builder.Fail("Vulkan instance",
                         format("vkCreateInstance failed (VkResult = {})", static_cast<Int>(createResult)));
            builder.Finalize();
            return builder.report;
        }
        builder.Pass("Vulkan instance", "Vulkan 1.1 instance created");

        const auto vkDestroyInstanceFn =
            reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(instance, "vkDestroyInstance"));
        const auto vkEnumeratePhysicalDevicesFn = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
        const auto vkGetPhysicalDevicePropertiesFn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
        const auto vkGetPhysicalDeviceQueueFamilyPropertiesFn =
            reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        const auto vkGetPhysicalDeviceFeaturesFn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures"));
        const auto vkEnumerateDeviceExtensionPropertiesFn =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                getInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
        const auto vkGetPhysicalDeviceFeatures2Fn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
        const auto vkGetPhysicalDeviceProperties2Fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

        // The instance is destroyed from a scope guard so it is released on every early-return
        // path and even if a String/format allocation throws while report rows are being built.
        const ScopeGuard destroyInstance([&]() {
            if (vkDestroyInstanceFn != nullptr) {
                vkDestroyInstanceFn(instance, nullptr);
            }
        });

        if (vkEnumeratePhysicalDevicesFn == nullptr || vkGetPhysicalDevicePropertiesFn == nullptr ||
            vkGetPhysicalDeviceQueueFamilyPropertiesFn == nullptr || vkGetPhysicalDeviceFeaturesFn == nullptr ||
            vkEnumerateDeviceExtensionPropertiesFn == nullptr) {
            builder.Fail("Vulkan core entry points",
                         "vkGetInstanceProcAddr could not resolve required Vulkan 1.0 functions");
            builder.Finalize();
            return builder.report;
        }

        Uint32 deviceCount = 0;
        const VkResult countResult = vkEnumeratePhysicalDevicesFn(instance, &deviceCount, nullptr);
        if (countResult != VK_SUCCESS) {
            builder.Fail("Physical device", format("vkEnumeratePhysicalDevices failed (VkResult = {})",
                                                   static_cast<Int>(countResult)));
            builder.Finalize();
            return builder.report;
        }
        if (deviceCount == 0) {
            builder.Fail("Physical device", "no Vulkan physical devices found");
            builder.Finalize();
            return builder.report;
        }
        builder.report.available = true;
        Vector<VkPhysicalDevice> devices(deviceCount);
        const VkResult enumerateResult = vkEnumeratePhysicalDevicesFn(instance, &deviceCount, devices.data());
        if (enumerateResult != VK_SUCCESS) {
            builder.Fail("Physical device", format("vkEnumeratePhysicalDevices failed (VkResult = {})",
                                                   static_cast<Int>(enumerateResult)));
            builder.Finalize();
            return builder.report;
        }
        devices.resize(deviceCount);

        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        for (VkPhysicalDevice candidate : devices) {
            Uint32 queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyPropertiesFn(candidate, &queueFamilyCount, nullptr);
            Vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyPropertiesFn(candidate, &queueFamilyCount, queueFamilies.data());
            const Bool hasGraphicsQueue =
                std::any_of(queueFamilies.begin(), queueFamilies.end(), [](const VkQueueFamilyProperties& family) {
                    return family.queueCount > 0 && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                });
            if (hasGraphicsQueue) {
                physicalDevice = candidate;
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            builder.Fail("Graphics queue",
                         format("none of the {} physical device(s) exposes a graphics queue family", deviceCount));
            builder.Finalize();
            return builder.report;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDevicePropertiesFn(physicalDevice, &properties);
        builder.Pass("Physical device",
                     format("{} ({} device(s) enumerated, picked the first with a graphics queue)",
                            String(properties.deviceName), deviceCount));
        builder.Pass("Graphics queue", "graphics queue family present");

        // driverVersion is vendor-encoded (each vendor packs its own bit layout), so it is
        // reported as raw hex instead of being decoded with the VK_VERSION_* macros.
        const String driverVersionString = format("0x{:08x}", properties.driverVersion);
        builder.report.rendererInfo = format("{} (Vulkan {}, driver {})", String(properties.deviceName),
                                             VkApiVersionToString(properties.apiVersion), driverVersionString);

        if (properties.apiVersion >= VK_API_VERSION_1_1) {
            builder.Pass("Device API version", format("Vulkan {}", VkApiVersionToString(properties.apiVersion)));
        } else {
            builder.Fail("Device API version",
                         format("Vulkan {} (< 1.1); the DirectVulkan backend requires a Vulkan 1.1 device",
                                VkApiVersionToString(properties.apiVersion)));
        }

        Vector<VkExtensionProperties> deviceExtensions;
        Uint32 deviceExtensionCount = 0;
        if (vkEnumerateDeviceExtensionPropertiesFn(physicalDevice, nullptr, &deviceExtensionCount, nullptr) ==
                VK_SUCCESS &&
            deviceExtensionCount > 0) {
            deviceExtensions.resize(deviceExtensionCount);
            if (vkEnumerateDeviceExtensionPropertiesFn(physicalDevice, nullptr, &deviceExtensionCount,
                                                       deviceExtensions.data()) == VK_SUCCESS) {
                deviceExtensions.resize(deviceExtensionCount);
            } else {
                deviceExtensions.clear();
            }
        }
        if (HasVkExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            builder.Pass("VK_KHR_swapchain", "device extension present");
        } else {
            builder.Fail("VK_KHR_swapchain", "required device extension missing; presentation is impossible");
        }

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeaturesFn(physicalDevice, &features);
        if (features.multiDrawIndirect == VK_TRUE) {
            builder.Pass("multiDrawIndirect", "indirect multi-draw batches run as single native commands");
        } else {
            builder.Warn("multiDrawIndirect",
                         "unsupported; indirect multi-draw batches fall back to one draw per command");
        }
        if (features.drawIndirectFirstInstance == VK_TRUE) {
            builder.Pass("drawIndirectFirstInstance", "indirect commands may carry a non-zero firstInstance");
        } else {
            builder.Warn("drawIndirectFirstInstance",
                         "unsupported; indirect commands with a non-zero baseInstance cannot run natively");
        }
        if (features.vertexPipelineStoresAndAtomics == VK_TRUE) {
            builder.Pass("vertexPipelineStoresAndAtomics",
                         "supported by driver (not currently enabled by the DirectVulkan backend)");
        } else {
            builder.Warn("vertexPipelineStoresAndAtomics",
                         "unsupported; shaders that write storage buffers from the vertex stage will not work");
        }

        Bool shaderDrawParameters = false;
        if (vkGetPhysicalDeviceFeatures2Fn != nullptr && properties.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceShaderDrawParametersFeatures drawParametersFeatures{};
            drawParametersFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &drawParametersFeatures;
            vkGetPhysicalDeviceFeatures2Fn(physicalDevice, &features2);
            shaderDrawParameters = drawParametersFeatures.shaderDrawParameters == VK_TRUE;
        } else if (HasVkExtension(deviceExtensions, VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME)) {
            // Vulkan 1.0 device: the extension alone exposes the SPIR-V DrawParameters capability.
            shaderDrawParameters = true;
        }
        if (shaderDrawParameters) {
            builder.Pass("shaderDrawParameters", "gl_DrawID/gl_BaseVertex/gl_BaseInstance shaders supported");
        } else {
            builder.Warn("shaderDrawParameters",
                         "unavailable; shaders using gl_DrawID/gl_BaseInstance will not work");
        }

        if (vkGetPhysicalDeviceProperties2Fn != nullptr && properties.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceSubgroupProperties subgroupProperties{};
            subgroupProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &subgroupProperties;
            vkGetPhysicalDeviceProperties2Fn(physicalDevice, &properties2);
            const Bool subgroupUsable = subgroupProperties.subgroupSize > 0 &&
                                        (subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                                        (subgroupProperties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
            if (subgroupUsable) {
                builder.Pass("Compute shader subgroup",
                             format("basic subgroup operations in compute, subgroup size {}",
                                    subgroupProperties.subgroupSize));
            } else {
                builder.Warn("Compute shader subgroup",
                             "basic subgroup operations are not usable from compute shaders");
            }
        } else {
            builder.Warn("Compute shader subgroup", "subgroup properties could not be queried");
        }

        builder.Info("VK_KHR_draw_indirect_count",
                     HasVkExtension(deviceExtensions, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)
                         ? "supported (GPU-driven indirect draw counts)"
                         : "not supported");
        const Bool indexTypeUint8 = HasVkExtension(deviceExtensions, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME) ||
                                    HasVkExtension(deviceExtensions, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        builder.Info("Index type uint8",
                     indexTypeUint8 ? "supported (native GL_UNSIGNED_BYTE index buffers)" : "not supported");
        builder.Info("Device", String(properties.deviceName));
        builder.Info("Driver version", driverVersionString + " (vendor-encoded)");

        builder.Finalize();
        MGLOG_I("Driver POST: Vulkan verdict = %s", builder.report.verdict.c_str());
        return builder.report;
    }
} // namespace MobileGL::MG_Util::SelfTest
