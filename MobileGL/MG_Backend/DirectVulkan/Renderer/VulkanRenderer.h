// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "Config.h"
#include "FrameContext.h"
#include "PipelineFactory.h"
#include "ProgramFactory.h"
#include "SwapchainObject.h"
#include "UniformManager.h"
#include "VertexInputStateFactory.h"
#include "VkBufferObject.h"
#include "VkBufferManager.h"
#include "VkClearManager.h"
#include "VkRenderPassManager.h"
#include "VkSamplerManager.h"
#include "VkTextureManager.h"
#include "MG_Util/Math/VectorTypes.h"
#include <Includes.h>
#include <vk_mem_alloc.h>

#include "../VkIncludes.h"

namespace MobileGL::MG_State::GLState {
    class FramebufferObject;
    class ProgramObject;
    class SamplerObject;
    class VertexArrayObject;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class DrawSetupAspect: Uint8 {
        FramebufferObject  = 1 << 0,
        VertexArrayObject  = 1 << 1,
        UniformBuffer      = 1 << 2,
        VertexBuffer       = 1 << 3,
        IndexBuffer        = 1 << 4,
        IndirectDrawBuffer = 1 << 5,
        Viewport           = 1 << 6,
        Scissor            = 1 << 7,
    };

    struct DrawCmdParam {
        Uint32 vertexCount = 0;
        Uint32 instanceCount = 1;
        Uint32 firstVertex = 0;
        Uint32 firstInstance = 0;
    };

    struct DrawIndexedCmdParam {
        Uint32 indexCount = 0;
        Uint32 instanceCount = 1;
        Uint32 firstIndex = 0;
        Int32 vertexOffset = 0;
        Int32 firstInstance = 0;
    };

    struct DrawCmd {
        GLenum mode = GL_TRIANGLES;
        DrawCmdParam params;
    };

    struct IndexBufferView {
        GLenum indexType = GL_UNSIGNED_SHORT;
        SizeT indexByteOffset = 0;
        SizeT indexByteSize = 0;
    };

    struct DrawIndexedCmd {
        GLenum mode = GL_TRIANGLES;
        IndexBufferView indexBufferView;

        DrawIndexedCmdParam params;
    };

    struct MultiDrawIndexedCmd {
        GLenum mode = GL_TRIANGLES;
        IndexBufferView indexBufferView;

        Uint32 drawCount = 0;
        DrawIndexedCmdParam* pParams = nullptr;
    };

    struct QueueFamilyIndices {
        Int32 graphicsFamily = -1;
        Int32 presentFamily = -1;
    };

    struct PhysicalDevice {
        QueueFamilyIndices queueFamilies;
        VkPhysicalDeviceProperties properties;
        VkPhysicalDevice handle = VK_NULL_HANDLE;

        Bool IsComplete() const {
            return handle != VK_NULL_HANDLE && queueFamilies.graphicsFamily != -1 && queueFamilies.presentFamily != -1;
        }
    };

    class VulkanRenderer {
    public:
        VulkanRenderer(NativeWindowType window, const VulkanRendererConfig& cfg = {});
        ~VulkanRenderer();

        void Initialize();
        void Shutdown();

        Bool SetupDraw(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                       const DrawCmdParam& drawParams,
                       const IndexBufferView* pIndexBufferView = nullptr);
        void ClearAttachmentsOnActiveRenderPass(VkCommandBuffer commandBuffer,
                                                const RenderPassEntry& compatibleRenderPassEntry);

        void Clear(GLbitfield mask);
        void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
        void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
        void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                     GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                     GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                             GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                             GLbitfield mask, GLenum filter);
        void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFbo,
                                  const SharedPtr<MG_State::GLState::FramebufferObject>& drawFbo,
                                  GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                  GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                  GLbitfield mask, GLenum filter);
        void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLint x, GLint y, GLsizei width, GLsizei height);
        void CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                              GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                              const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                              GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                              GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
        void GenerateMipmap(GLenum target);
        void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
        void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
        void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                             TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                             GLsizei bufSize, GLvoid* pixels);
        void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
        void DispatchComputeIndirect(GLintptr indirect);
        void MemoryBarrier(GLbitfield barriers);
        static VkMemoryBarrier BuildMemoryBarrierForGlBarriers(GLbitfield barriers);
        void DrawArrays(const DrawCmd& payload);
        void DrawElements(const DrawIndexedCmd& payload);
        void MultiDrawElements(const MultiDrawIndexedCmd& payloads);
        void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                            GLsizei maxdrawcount, GLsizei stride);
        void Present();

        const PhysicalDevice& GetPhysicalDevice() const;
        VkInstance GetInstance() const;
        Bool IsDrawIndirectCountExtensionEnabled() const;

        void RecreateSwapchain();

    private:
        struct BlitUniformData {
            float srcRect[4] = {0.f, 0.f, 1.f, 1.f};
            float dstRect[4] = {0.f, 0.f, 1.f, 1.f};
            Int surfaceTransform = 0;
            Int padding[3] = {0, 0, 0};
        };

        struct BlitResources {
            SharedPtr<MG_State::GLState::ProgramObject> program;
            SharedPtr<MG_State::GLState::SamplerObject> nearestSampler;
            SharedPtr<MG_State::GLState::SamplerObject> linearSampler;
            Int srcRectLocation = -1;
            Int dstRectLocation = -1;
            Int surfaceTransformLocation = -1;
            Uint32 samplerBinding = 0;
        };

        struct DepthMipmapResources {
            SharedPtr<MG_State::GLState::ProgramObject> program;
            Int srcRectLocation = -1;
            Int dstRectLocation = -1;
            Int surfaceTransformLocation = -1;
            Int srcTexelSizeLocation = -1;
            Uint32 samplerBinding = 0;
        };

        struct DeferredDepthMipmapCleanup {
            Vector<VkImageView> imageViews;
            Vector<VkFramebuffer> framebuffers;
            Vector<VkRenderPass> renderPasses;
            Vector<VkPipeline> pipelines;
        };

        void QueueClearBufferPayload(GLenum buffer, GLint drawbuffer, const ClearAttachmentPayload& clearPayload);
        void QueueClearBufferPayloadForFramebuffer(const MG_State::GLState::FramebufferObject& framebuffer,
                                                  GLenum buffer, GLint drawbuffer,
                                                  const ClearAttachmentPayload& clearPayload);

        NativeWindowType m_window = 0;
        void* m_platformDisplay = nullptr;
        void* m_platformLibrary = nullptr;
        void* m_platformCloseDisplay = nullptr;
        VulkanRendererConfig m_config;

        // Vulkan objects
        Bool m_validationLayersEnabled = false;
        Vector<VkExtensionProperties> m_extensions;
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        PhysicalDevice m_physicalDevice;
        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = nullptr;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        SwapchainObject m_swapchainObject;

        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;
        Bool m_drawIndirectCountExtensionEnabled = false;
        Bool m_indexTypeUint8ExtensionEnabled = false;
        Bool m_logicOpFeatureEnabled = false;
        using PFNDrawIndexedIndirectCountFunc = void(VKAPI_PTR*)(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                 VkDeviceSize offset, VkBuffer countBuffer,
                                                                 VkDeviceSize countBufferOffset, Uint32 maxDrawCount,
                                                                 Uint32 stride);
        static inline PFNDrawIndexedIndirectCountFunc s_vkCmdDrawIndexedIndirectCount = nullptr;

        VkCommandPool m_commandPool = VK_NULL_HANDLE;

        struct TransientBufferSliceCacheEntry {
            BufferSlice slice;
            Uint64 changeSerial = 0;
            SizeT size = 0;
        };

        VkBufferManager m_bufferManager;
        UnorderedMap<const MG_State::GLState::BufferObject*, TransientBufferSliceCacheEntry>
            m_transientVertexIndexBufferSlicesThisFrame;

        Uint m_imageIndexAcquired = 0;
        FrameContext m_frameContext;

        UniquePtr<PipelineFactory> m_pipelineFactory;
        UnorderedMap<ProgramFactory::HashType, VkPipeline> m_computePipelines;
        UniquePtr<ProgramFactory> m_programFactory;
        UniquePtr<UniformManager> m_uniformManager;
        UniquePtr<VertexInputStateFactory> m_vertexInputStateFactory;
        UniquePtr<VkClearManager> m_clearManager;
        UniquePtr<VkRenderPassManager> m_renderPassManager;
        UniquePtr<VkTextureManager> m_textureManager;
        UniquePtr<VkSamplerManager> m_samplerManager;
        BlitResources m_blitResources;
        DepthMipmapResources m_depthMipmapResources;
        Vector<DeferredDepthMipmapCleanup> m_deferredDepthMipmapCleanup;

        void CreateInstance();
        VkResult SetupDebugMessenger();
        VkResult DestroyDebugMessenger();
        VkDebugUtilsMessengerCreateInfoEXT PopulateDebugMessengerCreateInfo();
        void CreateSurface();
        void PickPhysicalDevice();
        void CreateLogicalDeviceAndQueues();
        void CreateAllocator();
        void DestroyAllocator();
        void CreateSwapchain();
        void CreateCommandPool();

        VkPipeline GetOrCreatePipeline(
            GLenum mode,
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj,
            ProgramFactory::CompileOptionFlags transformFlags,
            const MG_State::GLState::VertexArrayObject& vao,
            const RenderPassEntry& renderPassEntry);
        VkPipeline GetOrCreateComputePipeline(const ProgramFactory::VkProgramObject& programObj);
        void DestroyComputePipelines();

        Bool UploadAndBindVertexBuffers(VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject& vao,
                                        const DrawCmdParam& drawParams);
        Bool UploadAndBindIndexBuffer(FrameContext::FrameData& frame,
                                     const MG_State::GLState::VertexArrayObject& vao,
                                      const IndexBufferView* pIndexBufferView = nullptr);
        Bool InitializeBlitResources();
        Bool InitializeDepthMipmapResources();
        void ShutdownBlitResources();
        void ShutdownDepthMipmapResources();
        void CollectDeferredDepthMipmapCleanup(Uint32 frameIndex);
        void DestroyDeferredDepthMipmapCleanup();
        Bool TryBlitToDefaultFramebufferWithShader(FrameContext::FrameData& frame,
                                                   MG_State::GLState::FramebufferObject& readFbo,
                                                   MG_State::GLState::FramebufferObject& drawFbo,
                                                   GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                                   GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                                   GLenum filter);
        Bool MaterializePendingClearForTexture(VkCommandBuffer commandBuffer,
                                               MG_State::GLState::ITextureObject& texture);
        VkPipeline GetOrCreateBlitPipeline(const RenderPassEntry& renderPassEntry);
        Bool GenerateDepthMipmapWithShader(FrameContext::FrameData& frame,
                                           MG_State::GLState::ITextureObject& texture,
                                           VkTextureManager::TextureResource& resource,
                                           Uint32 baseMipLevel,
                                           Uint32 generateMipLevelCount,
                                           const IntVec3& storageBaseTexelSize,
                                           VkImageLayout originalLayout,
                                           VkImageLayout finalLayout);
        Bool SubmitReadbackCommandsAndWait(FrameContext::FrameData& frame);

        void ShutdownSwapchain();

        // Static functions
        static Int GetPresentQueueFamilyIndex(const PhysicalDevice& physicalDevice, VkSurfaceKHR surface,
                                              const Vector<VkQueueFamilyProperties>& queueFamilies,
                                              Int preferredFamilyIndex = -1);
        static Vector<VkQueueFamilyProperties> GetQueueFamilyFromPhysicalDevice(VkPhysicalDevice device);
        static Int GetQueueFamilyIndex(const Vector<VkQueueFamilyProperties>& queueFamilies, VkQueueFlagBits flag);
        static Vector<VkExtensionProperties> EnumerateInstanceExtensions();
        static Vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice device);
        static Bool IsExtensionSupported(const Vector<VkExtensionProperties>& availableExtensions,
                                         const char* extensionName);
        static Bool IsExtensionAlreadyEnabled(const Vector<const char*>& enabledExtensions, const char* extensionName);
        static Bool EnableOptionalDeviceExtension(const Vector<VkExtensionProperties>& availableExtensions,
                                                  Vector<const char*>& inOutEnabledExtensions,
                                                  const char* extensionName);
        void ResolveOptionalDeviceExtensions(const Vector<VkExtensionProperties>& availableExtensions,
                                             Vector<const char*>& inOutEnabledExtensions);
        static Bool IsNecessaryDeviceExtensionSupported(VkPhysicalDevice device);
        static Bool GetMoreCapablePhysicalDevice(VkPhysicalDevice newVkDevice, VkSurfaceKHR surface,
                                                 const PhysicalDevice& compareWithDevice,
                                                 PhysicalDevice& outBetterDevice);
        static constexpr const char* s_validationLayerNames[] = {"VK_LAYER_KHRONOS_validation"};
        static constexpr const char* s_deviceExtensionNames[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        static Bool CheckValidationLayerSupport();

        static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                            VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                            void* pUserData);
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
