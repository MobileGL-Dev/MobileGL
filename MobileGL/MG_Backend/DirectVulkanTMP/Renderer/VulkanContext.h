// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Renderer/VulkanContext.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "VkCommon.h"

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    class VulkanContext {
    public:
        VulkanContext() = default;
        ~VulkanContext();

        VulkanContext(const VulkanContext&) = delete;
        VulkanContext& operator=(const VulkanContext&) = delete;

        void Initialize(ANativeWindow* window, const char* appName);
        void Cleanup();

        VkInstance GetInstance() const { return m_instance; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
        VkDevice GetDevice() const { return m_device; }
        VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
        Uint32 GetGraphicsQueueFamily() const { return m_graphicsQueueFamily; }
        VkSurfaceKHR GetSurface() const { return m_surface; }
        ANativeWindow* GetWindow() const { return m_window; }

    private:
        void CreateInstance(const char* appName);
        void CreateSurface(ANativeWindow* window);
        void PickPhysicalDevice();
        void CreateDevice();

        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        Uint32 m_graphicsQueueFamily = ~0u;
        ANativeWindow* m_window = nullptr;
    };
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager
