// MobileGL - MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Loader.h"

namespace MobileGL::MG_Util::BackendLoader {
    inline Version DecodeApiVersion(uint32_t version) {
        return {(Int)VK_VERSION_MAJOR(version), (Int)VK_VERSION_MINOR(version), (Int)VK_VERSION_PATCH(version)};
    }

    inline std::string DecodeDriverVersion(uint32_t driverVersion) {
        std::ostringstream oss;
        oss << VK_VERSION_MAJOR(driverVersion) << "." << VK_VERSION_MINOR(driverVersion) << "."
            << VK_VERSION_PATCH(driverVersion);
        return oss.str();
    }

    Bool QueryVulkanCapabilities(MobileGL::MG_External::VulkanCapabilities& caps, VkPhysicalDevice physicalDevice) {
        if (!physicalDevice) {
            MGLOG_E("Invalid physical device handle");
            return false;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        if (props.apiVersion < VK_API_VERSION_1_1) {
            MGLOG_E("Vulkan API version %u.%u.%u is not supported, requires at least 1.1",
                    VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion));
            return false;
        }

        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

        const VkPhysicalDeviceProperties& p = props2.properties;
        caps.VulkanAPIVersion = DecodeApiVersion(p.apiVersion);
        caps.DeviceName = p.deviceName;
        caps.DriverVersionString = DecodeDriverVersion(p.driverVersion);
        caps.UniformBufferOffsetAlignment = static_cast<int>(p.limits.minUniformBufferOffsetAlignment);

        return true;
    }
} // namespace MobileGL::MG_Util::BackendLoader
