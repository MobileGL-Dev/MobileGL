//
// Created by BZLZHH on 2025/3/15.
//

#include "MesaEmu.h"

// Code From DeepSeek

static thread_local OSMesaContext currentContext = nullptr;
static uint32_t findGraphicsQueueFamily(VkPhysicalDevice physicalDevice) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return i;
        }
    }
    throw std::runtime_error("No graphics queue family found");
}
extern "C" MG_EXPORT OSMesaContext OSMesaCreateContext(GLenum format, OSMesaContext sharelist) {
    OSMesaContextData newCtx;
    try {
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        if (vkCreateInstance(&instanceInfo, nullptr, &newCtx.instance) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan instance");
        }
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(newCtx.instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("No Vulkan devices found");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(newCtx.instance, &deviceCount, devices.data());
        newCtx.physicalDevice = devices[0];
        newCtx.queueFamilyIndex = findGraphicsQueueFamily(newCtx.physicalDevice);
        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = newCtx.queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueCreateInfo;
        if (vkCreateDevice(newCtx.physicalDevice, &deviceInfo, nullptr, &newCtx.device) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create logical device");
        }
        vkGetDeviceQueue(newCtx.device, newCtx.queueFamilyIndex, 0, &newCtx.graphicsQueue);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = newCtx.queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(newCtx.device, &poolInfo, nullptr, &newCtx.commandPool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create command pool");
        }
        OSMesaContext ctx = reinterpret_cast<OSMesaContext>(new int(0));
        ctxMap[ctx] = newCtx;
        return ctx;
    } catch (const std::exception& e) {
        if (newCtx.device) vkDestroyDevice(newCtx.device, nullptr);
        if (newCtx.instance) vkDestroyInstance(newCtx.instance, nullptr);
        return nullptr;
    }
}
extern "C" MG_EXPORT void OSMesaDestroyContext(OSMesaContext ctx) {
    if (ctxMap.find(ctx) != ctxMap.end()) {
        auto& data = ctxMap[ctx];

        if (data.commandPool) {
            vkDestroyCommandPool(data.device, data.commandPool, nullptr);
        }
        if (data.imageView) {
            vkDestroyImageView(data.device, data.imageView, nullptr);
        }
        if (data.targetImage) {
            vkDestroyImage(data.device, data.targetImage, nullptr);
        }
        if (data.device) {
            vkDestroyDevice(data.device, nullptr);
        }
        if (data.instance) {
            vkDestroyInstance(data.instance, nullptr);
        }

        ctxMap.erase(ctx);
        delete reinterpret_cast<int*>(ctx);
    }
}
extern "C" MG_EXPORT GLboolean OSMesaMakeCurrent(OSMesaContext ctx, void *buffer, GLenum type,
                                       GLsizei width, GLsizei height) {
    if (!ctx || !buffer || width <= 0 || height <= 0) return GL_FALSE;
    auto& data = ctxMap[ctx];
    data.currentWidth = width;
    data.currentHeight = height;
    data.frontBuffer = static_cast<GLubyte*>(buffer);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(data.device, &imageInfo, nullptr, &data.targetImage) != VK_SUCCESS) {
        return GL_FALSE;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = data.targetImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(data.device, &viewInfo, nullptr, &data.imageView) != VK_SUCCESS) {
        vkDestroyImage(data.device, data.targetImage, nullptr);
        return GL_FALSE;
    }
    currentContext = ctx;
    return GL_TRUE;
}
extern "C" MG_EXPORT OSMesaContext OSMesaGetCurrentContext() {
    return currentContext;
}
extern "C" MG_EXPORT void OSMesaPixelStore(GLint pname, GLint value) {
    if (pname == GL_PACK_ALIGNMENT && currentContext) {
        ctxMap[currentContext].packAlignment = value;
    }
}
extern "C" MG_EXPORT OSMESAproc OSMesaGetProcAddress( const char *funcName ) {
    return (OSMESAproc)MG_GL::GLX::GetProcAddress(funcName);
}