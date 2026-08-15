// MobileGL - MobileGL/MG_Test/Backend/DirectVulkan/SanityTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <iostream>
#include <utility>
#include <vector>

#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>

TEST(DirectVulkanSanity, ProgramMovePreservesViewportIndexUsage) {
    using VkProgramObject = MobileGL::MG_Backend::DirectVulkan::ProgramFactory::VkProgramObject;

    VkProgramObject moveConstructedSource;
    moveConstructedSource.writesViewportIndexBuiltin = true;
    VkProgramObject moveConstructed(std::move(moveConstructedSource));
    EXPECT_TRUE(moveConstructed.writesViewportIndexBuiltin);
    EXPECT_FALSE(moveConstructedSource.writesViewportIndexBuiltin);

    VkProgramObject moveAssignedSource;
    moveAssignedSource.writesViewportIndexBuiltin = true;
    VkProgramObject moveAssigned;
    moveAssigned = std::move(moveAssignedSource);
    EXPECT_TRUE(moveAssigned.writesViewportIndexBuiltin);
    EXPECT_FALSE(moveAssignedSource.writesViewportIndexBuiltin);
}

TEST(DirectVulkanSanity, ExtensionEnumeration) {
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

    std::cout << extensionCount << " extensions supported\n";

    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

    for(const auto& extension : extensions) {
        std::cout << extension.extensionName
                  << " (r." << extension.specVersion << ")\n";
    }
}

#include <GLFW/glfw3.h>
TEST(DirectVulkanSanity, WindowCreation) {
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(800, 600, "MobileGL WindowCreation", nullptr, nullptr);

    while(!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }

    glfwDestroyWindow(window);

    glfwTerminate();
}

#define EGLAPI
#include <EGL/egl.h>
#ifdef _WIN32
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    #define GLFW_EXPOSE_NATIVE_X11
#elif defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif
TEST(DirectVulkanSanity, ContextCreation) {
    glfwInit();

    static EGLint const attribute_list[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);
    EGLConfig config;
    EGLint num_config;
    eglChooseConfig(display, attribute_list, &config, 1, &num_config);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(800, 600, "MobileGL ContextCreation", nullptr, nullptr);
    EGLNativeWindowType nativewindow = 0;
#ifdef _WIN32
    nativewindow = glfwGetWin32Window(window);
#elif defined(__linux__)
    nativewindow = static_cast<EGLNativeWindowType>(glfwGetX11Window(window));
#elif defined(__APPLE__)
    void* cocoaWindow = glfwGetCocoaWindow(window);
    ASSERT_NE(cocoaWindow, nullptr);
    auto msgSendObj = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    auto msgSendVoidObj = reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend);
    auto msgSendVoidBool = reinterpret_cast<void (*)(id, SEL, bool)>(objc_msgSend);
    id contentView = msgSendObj(static_cast<id>(cocoaWindow), sel_registerName("contentView"));
    ASSERT_NE(contentView, nullptr);
    msgSendVoidBool(contentView, sel_registerName("setWantsLayer:"), true);

    id metalLayerClass = reinterpret_cast<id>(objc_getClass("CAMetalLayer"));
    ASSERT_NE(metalLayerClass, nullptr);
    id metalLayer = msgSendObj(metalLayerClass, sel_registerName("layer"));
    ASSERT_NE(metalLayer, nullptr);

    msgSendVoidObj(contentView, sel_registerName("setLayer:"), metalLayer);
    nativewindow = reinterpret_cast<EGLNativeWindowType>(metalLayer);
#endif
    ASSERT_NE(nativewindow, static_cast<EGLNativeWindowType>(0));
    EGLSurface surface = eglCreateWindowSurface(display, config, nativewindow, nullptr);
    eglMakeCurrent(display, surface, surface, context);

    while(!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        eglSwapBuffers(display, surface);
    }

    glfwDestroyWindow(window);

    glfwTerminate();
}
