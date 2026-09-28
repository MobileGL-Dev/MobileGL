// MobileGL - MetalANGLE GLES 3.2 Compatibility Shim
// Translates OpenGL ES 3.1 & 3.2 entry points to MetalANGLE native extensions and emulated paths.

#pragma once
#include <Includes.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MetalANGLE_Shim {
    // Stub for glShaderStorageBlockBinding (desktop GL 4.3 entry point needed by MobileGL program reflection)
    void GL_APIENTRY Stub_glShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding);

    // Install OpenGL ES 3.1 & 3.2 shims and wrappers into funcs
    void InstallGLES32Shims(MG_External::GLESFunctionsTable& funcs, MG_External::EGL::eglGetProcAddress_PTR procAddress);

    // Patch GLESCapabilities to reflect MetalANGLE + shim features
    void PatchMetalANGLECapabilities(MG_External::GLESCapabilities& caps, const MG_External::GLESFunctionsTable& funcs);
}
