// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/TmpImpl.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "Renderer/VkCommon.h"
#include "Renderer/VulkanContext.h"
#include "Renderer/SwapchainManager.h"
#include "Renderer/FrameContext.h"
#include "Managers/ProgramManager.h"
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Backend::DirectVulkanTMP::TmpImpl {
    namespace DV = MobileGL::MG_Backend::DirectVulkanTMP::VkManager;
    using BufferObject = MobileGL::MG_State::GLState::BufferObject;
    using ProgramObject = MobileGL::MG_State::GLState::ProgramObject;
    using VertexArrayObject = MobileGL::MG_State::GLState::VertexArrayObject;
    using FramebufferObject = MobileGL::MG_State::GLState::FramebufferObject;
    using FramebufferAttachmentObject = MobileGL::MG_State::GLState::FramebufferAttachmentObject;
    using ITextureObject = MobileGL::MG_State::GLState::ITextureObject;
    using RenderbufferObject = MobileGL::MG_State::GLState::RenderbufferObject;
    using SamplerObject = MobileGL::MG_State::GLState::SamplerObject;
    using TextureObjectMipmap = MobileGL::MG_State::GLState::TextureObjectMipmap;
    using TrashImage = MobileGL::MG_Backend::DirectVulkanTMP::TrashImage;

    struct BufferResource {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceSize offset = 0;
        VkBufferUsageFlags usage = 0;
        VkMemoryPropertyFlags props = 0;
        void* mapped = nullptr;
        Bool fromRing = false;
        Uint32 lastUsedFrame = ~0u;
    };

    struct BufferResourceSet {
        Vector<BufferResource> perFrame;
    };

    struct PendingBufferCopyBatch {
        VkBuffer src = VK_NULL_HANDLE;
        VkBuffer dst = VK_NULL_HANDLE;
        Vector<VkBufferCopy> regions;
    };

    struct StagingRing {
        BufferResource buffer;
        VkDeviceSize capacity = 0;
        VkDeviceSize head = 0;
    };

    struct TextureResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        UnorderedMap<Uint32, VkImageView> mipViews;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent{0, 0, 1};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        Uint32 mipLevels = 1;
        Uint16 paramsVersion = 0;
        bool valid = false;
    };

    struct RenderbufferResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{0, 0};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool valid = false;
    };

    struct SamplerResource {
        VkSampler sampler = VK_NULL_HANDLE;
        Uint16 version = 0;
        SamplerParameters params{};
    };

    struct PendingClearInfo {
        GLbitfield mask = 0;
        FloatVec4 clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
        Float clearDepth = 1.0f;
    };

    struct UniformBindingInfo {
        String name;
        Uint32 binding = 0;
        Uint32 set = 0;
        Uint32 blockIndex = 0xFFFFFFFFu;
        VkShaderStageFlags stages = 0;
    };

    struct SamplerBindingInfo {
        String name;
        Uint32 binding = 0;
        Uint32 set = 0;
        VkShaderStageFlags stages = 0;
    };

    struct ProgramResource {
        ProgramObject* program = nullptr;
        Uint64 spvHash = 0;
        DV::ProgramManager::HashType programHash = 0;
        Vector<VkPipelineShaderStageCreateInfo>* shaderStages = nullptr;

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

        struct UboPool {
            Vector<BufferResource> buffers;
            Uint32 cursor = 0;
        };
        Vector<UboPool> uboPools;
        SizeT uboSize = 0;
        Int32 uboBinding = -1;
        Uint32 uniformDescriptorCount = 0;
        Uint32 samplerDescriptorCount = 0;

        Vector<UniformBindingInfo> uniformBindings;
        Vector<SamplerBindingInfo> samplerBindings;
        UnorderedMap<String, Uint32> samplerBindingByName;

        UnorderedMap<Uint64, VkPipeline> pipelines;
    };

    struct DescriptorPoolConfig {
        Uint32 maxSets = 0;
        Uint32 uniformCount = 0;
        Uint32 samplerCount = 0;
    };

    struct FrameDescriptorPools {
        Vector<VkDescriptorPool> pools;
        DescriptorPoolConfig config;
        Uint32 activePool = 0;
    };

    struct VertexAttribKey {
        Uint8 enabled = 0;
        Uint8 size = 0;
        Uint8 type = 0;
        Uint8 normalized = 0;
        Uint8 isInteger = 0;
        Uint8 divisor = 0;
        Uint16 pad = 0;
        Uint32 stride = 0;
        Uint64 offset = 0;
    };

    struct AttachmentInfo {
        FramebufferAttachmentType type = FramebufferAttachmentType::None;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = 0;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ITextureObject* texture = nullptr;
        RenderbufferObject* renderbuffer = nullptr;
    };

    struct BackendFramebufferObject {
        VkExtent2D extent{0, 0};
        Uint32 colorAttachmentCount = 0;
        Bool hasDepth = false;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkFormat stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        Uint64 renderingCompatHash = 0;

        Vector<AttachmentInfo> attachments;
        Vector<VkFormat> colorAttachmentFormats;
        Array<Int32, static_cast<SizeT>(FramebufferAttachmentType::FramebufferAttachmentTypeCount)> attachmentIndex =
            {};

        FramebufferObject::FramebufferAttachmentVersionArray syncedAttachmentVersions = {0};
        FramebufferObject::FramebufferAttachmentArray frontendDrawBuffers = {FramebufferAttachmentType::None};
        Array<Int32, FramebufferObject::MAX_DRAW_BUFFERS> drawBufferAttachmentIndices = {};
        FramebufferAttachmentType frontendReadBuffer = FramebufferAttachmentType::Color0;
        Uint64 configHash = 0;
        Uint16 objectVersion = 0;

        Bool IsValid() const { return !attachments.empty() && extent.width > 0 && extent.height > 0; }
    };

    struct VulkanState {
        UniquePtr<DV::VulkanContext> ctx;
        UniquePtr<DV::SwapchainManager> swapchain;
        UniquePtr<DV::ProgramManager> programMgr;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        Vector<UniquePtr<DV::FrameContext>> frames;
        Vector<FrameDescriptorPools> frameDescriptorPools;
        Vector<BufferResource> stagingPool;
        Vector<Vector<BufferResource>> stagingPending;
        Uint32 currentFrame = 0;
        Uint32 maxFramesInFlight = 2;
        Bool initialized = false;
        UintVec2 viewportSize{0, 0};

        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        Uint64 defaultRenderingCompatHash = 0;

        BufferResource nullUbo;

        TextureResource defaultTexture;
        SamplerResource defaultSampler;

        UnorderedMap<const BufferObject*, BufferResourceSet> buffers;
        UnorderedMap<const ITextureObject*, TextureResource> textures;
        UnorderedMap<const RenderbufferObject*, RenderbufferResource> renderbuffers;
        UnorderedMap<const SamplerObject*, SamplerResource> samplers;
        UnorderedMap<const ProgramObject*, ProgramResource> programs;
        UnorderedMap<SharedPtr<MG_State::GLState::FramebufferObject>, SharedPtr<BackendFramebufferObject>> framebuffers;
        Vector<PendingBufferCopyBatch> pendingBufferCopies;
        Vector<StagingRing> stagingRings;

        VkExtent2D activeExtent{0, 0};
        Vector<VkImageView> activeColorViews;
        Vector<VkImageLayout> activeColorLayouts;
        Vector<VkFormat> activeColorFormats;
        Vector<Int32> activeColorAttachmentIndices;
        VkImageView activeDepthView = VK_NULL_HANDLE;
        VkImageLayout activeDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageView activeStencilView = VK_NULL_HANDLE;
        VkImageLayout activeStencilLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Uint32 activeSwapchainImageIndex = ~0u;
        Uint32 activeColorAttachmentCount = 1;
        Bool activeHasDepth = false;
        VkFormat activeDepthFormat = VK_FORMAT_UNDEFINED;
        VkFormat activeDepthAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkFormat activeStencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        Uint64 activeRenderingCompatHash = 0;
        Uint64 activeRenderingConfigHash = 0;
        SharedPtr<MG_State::GLState::FramebufferObject> activeStateFBO = nullptr;
        SharedPtr<BackendFramebufferObject> activeBackendFBO = nullptr;
        Bool activeIsDefault = true;
        Bool activeDefaultColorWrites = true;

        Uint64 recordingRenderingConfigHash = 0;
        Bool cmdBufferBegun = false;

        Bool recording = false;
        Bool frameSubmitted = false;
        FloatVec4 clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
        Float clearDepth = 1.0f;

        UnorderedMap<SharedPtr<FramebufferObject>, PendingClearInfo> pendingClears;

        Vector<VkImageLayout> swapchainImageLayouts;

        PFN_vkCmdBeginRendering pfnCmdBeginRendering = nullptr;
        PFN_vkCmdEndRendering pfnCmdEndRendering = nullptr;
        PFN_vkWaitSemaphores pfnWaitSemaphores = nullptr;
        VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
        Uint64 timelineValue = 0;
    };

    void Present();
    void FrameBegin();
    void InitVulkan(ANativeWindow* window);
    const VulkanState& GetVulkanState();
    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
    void Clear(GLbitfield mask);
    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex);
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount);
    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex);
    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex);
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance);
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex);
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance);
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect);
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance);
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void DrawArraysIndirect(GLenum mode, const void* indirect);
    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter);
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border);
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height);
    void GenerateMipmap(GLenum target);
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::TmpImpl
