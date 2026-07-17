// MobileGL - MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../BackendObject.h"
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Backend::DirectGLES {
    // Populates the same format-capability cache used by backend startup. The caller
    // must keep the supplied GLES context current for the duration of this call.
    void PopulateFormatCapabilities(const MG_External::GLESFunctionsTable& gl,
                                    const MG_External::GLESCapabilities& capabilities,
                                    FormatCapabilityCache& cache);

    class BackendObject_DirectGLES : public BackendObject {
    public:
        ~BackendObject_DirectGLES() override;

        void Initialize() override;
        Bool InitCapabilities() override;
        Bool InitWindowSurface() override;
        Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) override;
        Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) override;
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

        const MG_External::GLESFunctionsTable& GetGLESFunctions() const;
        const MG_External::EGLFunctionsTable& GetEGLFunctions() const;

    private:
        void UpdateDynamicBackendParameters();
        Bool InitPbufferSurface(EGLint width, EGLint height) override;
        void OnEGLSurfaceReleased(EGLSurface surface) override;

        Bool m_initialized = false;
        MG_External::EGLFunctionsTable m_EGLFunctions;
        MG_External::GLESFunctionsTable m_GLESFunctions;
        MG_External::GLESCapabilities m_GLESCapabilities;
        DynamicBackendParameters m_dynamicParameters;
    };

    // Single-source-of-truth helpers shared with the driver POST
    // (MG_Util/SelfTest/DriverPost.cpp), so the identity strings and extension list
    // MobileGL reports to applications on this backend cannot drift from what the
    // POST screen shows.

    // Static identity of the Espryt renderer (renderer/backend names, target GL/GLSL
    // versions, ExtraVendor). The Extensions vector inside is live backend state that
    // is reconciled after capability init; callers that need the advertised list for
    // a known capability set must use BuildAdvertisedExtensions instead.
    const RendererInfo& GetRendererIdentity();

    // The full OpenGL extension list Espryt advertises (glGetString(GL_EXTENSIONS))
    // for a device whose timer queries / anisotropic filtering are (or are not) usable.
    // The MOBILEGL_DISABLE_TIMERQUERY escape hatch is applied inside.
    Vector<GLExtension> BuildAdvertisedExtensions(Bool timerQueriesSupported, Bool anisotropicFilteringSupported);

    // Format: <OpenGL ES Renderer>, OpenGL ES <Major>.<Minor> — the exact string an
    // initialized backend returns from GetBackendAPIVersionString (and that ends up
    // inside the application-visible GL_RENDERER string).
    String FormatBackendAPIVersionString(const String& glesRendererString, Int glesMajor, Int glesMinor);
} // namespace MobileGL::MG_Backend::DirectGLES
