// MobileGL - MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../BackendObject.h"
#include "MagmaSession.h"
#include <MG_Util/BackendLoaders/Vulkan/Loader.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // Populates the same format-capability cache used by backend startup. Passing the
    // instance-resolved function keeps standalone callers independent of global loader
    // initialization; the physical device must remain valid for the duration of the call.
    void PopulateFormatCapabilities(VkPhysicalDevice physicalDevice,
                                    PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties,
                                    const MG_External::VulkanCapabilities& capabilities,
                                    FormatCapabilityCache& cache);

    class BackendObject_DirectVulkan : public BackendObject {
    public:
        BackendObject_DirectVulkan();
        ~BackendObject_DirectVulkan() override;

        void Initialize() override;
        void BindSessionStateToThisThread() override;
        void OnClientContextBound(Uint64 token) override;
        Bool InitWindowSurface() override;
        Bool InitCapabilities() override;
        Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) override;
        Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) override;
        Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) override;
        Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) override;
        Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) override;
        Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) override;
        void SetEGLSwapInterval(Int interval) override;
        Bool BlitDefaultFramebufferToSharedImage(const SharedImageView& image,
                                                 const MG_Util::Damage::Region& region) override;
        // The active surface target's swapchain image ages (VulkanRenderer::CurrentDrawBufferAge).
        Int32 QueryCurrentBufferAge() override;
        void ReleaseEGLSurface(EGLSurface surface) override;
        void ReleaseEGLResources() override;
        // The server window going and coming back: the window surface's target (VkSurfaceKHR,
        // swapchain) is destroyed and a pbuffer target of the same extent stands in for it, and back.
        Bool SuspendServerWindow(void* window) override;
        Bool ResumeServerWindow(void* window, Uint32 width, Uint32 height) override;
        Bool ServerWindowResized(void* window, Uint32 width, Uint32 height) override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;
        void ApplyVulkanCapabilitiesForTesting(const MG_External::VulkanCapabilities& capabilities);

    private:
        Bool InitPbufferSurface(EGLint width, EGLint height) override;
        void OnEGLSurfaceReleased(EGLSurface surface) override;
        void OnEGLSurfaceForgotten(EGLSurface surface) override;
        // A served session keeps one renderer across all its client's surfaces and switches the
        // renderer's surface target instead (VulkanRenderer::ActivateSurfaceTarget).
        Bool UsesSurfaceTargets() const;
        static Uint64 SurfaceTargetKey(EGLSurface surface) { return reinterpret_cast<Uint64>(surface); }
        void UpdateAdvertisedExtensions();
        void UpdateDynamicBackendParameters();

        Bool m_initialized = false;
        DynamicBackendParameters m_dynamicParameters;
        MG_External::VulkanCapabilities m_vulkanCaps;
        RendererInfo m_rendererInfo;
        // A served session's own copy of Magma's per-process state (MagmaSession.h), bound to its
        // apply thread. Null in the monolith and every other single-session shape.
        UniquePtr<MagmaSession> m_magmaSession;
        // The binding each client context was last made current with, for OnClientContextBound.
        UnorderedMap<EGLContext, EGLCurrentState> m_contextBindings;
        // Window surfaces whose window is gone (SuspendServerWindow), with the extent their
        // placeholder pbuffer target is built at - the window's, so the default framebuffer the
        // client knows keeps its size.
        struct SuspendedWindowSurface {
            Uint32 Width = 1;
            Uint32 Height = 1;
        };
        UnorderedMap<EGLSurface, SuspendedWindowSurface> m_suspendedWindowSurfaces;
        // The placeholder pbuffer target of a suspended surface, made active. False: it could not be built.
        Bool ActivateSuspendedPlaceholder(EGLSurface surface, const SuspendedWindowSurface& extent);
    };

    // Single-source-of-truth helpers shared with the driver POST
    // (MG_Util/SelfTest/DriverPost.cpp), so the identity strings and extension list
    // MobileGL reports to applications on this backend cannot drift from what the
    // POST screen shows.

    // Static identity of the Magma renderer (renderer/backend names, target GL/GLSL
    // versions, ExtraVendor) with the baseline extension advertisement (no runtime-gated
    // capabilities). A live backend copies this in its constructor and
    // reconciles the Extensions in UpdateAdvertisedExtensions once real capabilities
    // exist; callers that need the advertised list for a known capability set must
    // use BuildAdvertisedExtensions instead.
    const RendererInfo& GetRendererIdentity();

    // The full OpenGL extension list Magma advertises (glGetString(GL_EXTENSIONS)) for
    // a device with the given raw capabilities. The MOBILEGL_MAGMA_DISABLE_SUBGROUP and
    // MOBILEGL_DISABLE_TIMERQUERY escape hatches are applied inside, so callers pass
    // the detected device support (passing an already-gated value is harmless).
    Vector<GLExtension> BuildAdvertisedExtensions(Bool shaderSubgroupSupported, Bool timerQueriesSupported,
                                                  Bool anisotropicFilteringSupported,
                                                  Bool nonZeroIndirectBaseInstanceSupported,
                                                  Bool cubeMapArraySupported);

    // Format: <GPU Name>, Vulkan <Vulkan Version>, Driver <Driver Version> — the exact
    // string an initialized backend returns from GetBackendAPIVersionString (and that
    // ends up inside the application-visible GL_RENDERER string).
    String FormatBackendAPIVersionString(const String& deviceName, const String& vulkanApiVersionString,
                                         const String& driverVersionString);
} // namespace MobileGL::MG_Backend::DirectVulkan
