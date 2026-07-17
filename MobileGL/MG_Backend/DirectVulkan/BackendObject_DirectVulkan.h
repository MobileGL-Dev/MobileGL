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
        Bool InitWindowSurface() override;
        Bool InitCapabilities() override;
        Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) override;
        Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) override;
        Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) override;
        Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) override;
        Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) override;
        Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) override;
        void ReleaseEGLSurface(EGLSurface surface) override;
        void ReleaseEGLResources() override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;
        void ApplyVulkanCapabilitiesForTesting(const MG_External::VulkanCapabilities& capabilities);

    private:
        Bool InitPbufferSurface(EGLint width, EGLint height) override;
        void OnEGLSurfaceReleased(EGLSurface surface) override;
        void UpdateAdvertisedExtensions();
        void UpdateDynamicBackendParameters();

        Bool m_initialized = false;
        DynamicBackendParameters m_dynamicParameters;
        MG_External::VulkanCapabilities m_vulkanCaps;
        RendererInfo m_rendererInfo;
    };

    // Single-source-of-truth helpers shared with the driver POST
    // (MG_Util/SelfTest/DriverPost.cpp), so the identity strings and extension list
    // MobileGL reports to applications on this backend cannot drift from what the
    // POST screen shows.

    // Static identity of the Magma renderer (renderer/backend names, target GL/GLSL
    // versions, ExtraVendor) with the baseline extension advertisement (no shader
    // subgroup, no timer queries). A live backend copies this in its constructor and
    // reconciles the Extensions in UpdateAdvertisedExtensions once real capabilities
    // exist; callers that need the advertised list for a known capability set must
    // use BuildAdvertisedExtensions instead.
    const RendererInfo& GetRendererIdentity();

    // The full OpenGL extension list Magma advertises (glGetString(GL_EXTENSIONS)) for
    // a device with the given raw capabilities. The MOBILEGL_DISABLE_SUBGROUP and
    // MOBILEGL_DISABLE_TIMERQUERY escape hatches are applied inside, so callers pass
    // the detected device support (passing an already-gated value is harmless).
    Vector<GLExtension> BuildAdvertisedExtensions(Bool shaderSubgroupSupported, Bool timerQueriesSupported,
                                                  Bool anisotropicFilteringSupported);

    // Format: <GPU Name>, Vulkan <Vulkan Version>, Driver <Driver Version> — the exact
    // string an initialized backend returns from GetBackendAPIVersionString (and that
    // ends up inside the application-visible GL_RENDERER string).
    String FormatBackendAPIVersionString(const String& deviceName, const String& vulkanApiVersionString,
                                         const String& driverVersionString);
} // namespace MobileGL::MG_Backend::DirectVulkan
