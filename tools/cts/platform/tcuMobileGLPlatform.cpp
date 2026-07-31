/*-------------------------------------------------------------------------
 * dEQP platform port for MobileGL (Android and desktop Linux)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *//*!
 * \file
 * \brief MobileGL platform.
 *
 * Modelled on the surfaceless platform, but adapted to MobileGL, which ships
 * its own EGL implementation inside libMobileGL.so:
 *
 *  - Every EGL call goes through the dynamically loaded library. The
 *    surfaceless port mixes wrapper calls with globally linked egl* symbols;
 *    doing that here would silently reach Android's system EGL instead.
 *  - Desktop-GL configs are selected with EGL_OPENGL_BIT. The surfaceless port
 *    always asks for an ES bit, which cannot satisfy a GL 3.3 core context.
 *  - A real surface is always created. MobileGL rejects EGL_NO_SURFACE with
 *    EGL_BAD_MATCH, and --deqp-surface-type=fbo asks the platform for
 *    SURFACETYPE_DONT_CARE, so "no surface" is not an option.
 *  - On Android, window surfaces are backed by an AImageReader rather than an
 *    Activity, which is what lets the suite run as a plain adb-shell binary.
 *    DirectVulkan needs this: its pbuffer path requires VK_EXT_headless_surface,
 *    which Adreno's Android driver does not expose.
 *  - On desktop Linux, only pbuffer surfaces are offered. DirectVulkan's
 *    pbuffer path works there because desktop Vulkan loaders expose
 *    VK_EXT_headless_surface.
 *
 * Environment:
 *   MOBILEGL_CTS_LIB      path/soname of the MobileGL library (default libMobileGL.so)
 *   MOBILEGL_CTS_SURFACE  "window" (Android default) or "pbuffer" (desktop default/only)
 *   MOBILEGL_BACKEND_TYPE read by MobileGL itself; set it before launching
 *//*--------------------------------------------------------------------*/

#include "tcuMobileGLPlatform.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "deDynamicLibrary.hpp"
#include "egluUtil.hpp"
#include "eglwEnums.hpp"
#include "eglwLibrary.hpp"
#include "gluPlatform.hpp"
#include "gluRenderConfig.hpp"
#include "gluRenderContext.hpp"
#include "glwInitFunctions.hpp"
#include "tcuCommandLine.hpp"
#include "tcuPixelFormat.hpp"
#include "tcuPlatform.hpp"
#include "tcuRenderTarget.hpp"

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImageReader.h>
#endif

using std::string;
using std::vector;

#if !defined(EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR)
#define EGL_CONTEXT_FLAGS_KHR 0x30FC
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR 0x00000002
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001
#define EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR 0x00000001
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR 0x00000002
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FD
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR 0x00000004
#endif

namespace tcu
{
namespace mobilegl
{

static string getLibraryName(void)
{
    const char *env = std::getenv("MOBILEGL_CTS_LIB");
    return (env && env[0]) ? string(env) : string("libMobileGL.so");
}

#if defined(__ANDROID__)
//! Window surfaces default on: they are the only kind DirectVulkan can use
//! on Android (its pbuffer path needs VK_EXT_headless_surface).
static bool useWindowSurface(void)
{
    const char *env = std::getenv("MOBILEGL_CTS_SURFACE");
    return !(env && string(env) == "pbuffer");
}
#else
//! Desktop: pbuffer only. VK_EXT_headless_surface is available there and no
//! Activity-free native window abstraction exists.
static bool useWindowSurface(void)
{
    return false;
}
#endif

#if defined(__ANDROID__)
/*--------------------------------------------------------------------*//*!
 * \brief A real ANativeWindow with no Activity behind it.
 *
 * AImageReader's window is an ordinary BufferQueue producer, so both
 * eglCreateWindowSurface and vkCreateAndroidSurfaceKHR accept it. The image
 * listener must drain the queue: without it the producer blocks once maxImages
 * buffers are in flight and the next swap deadlocks.
 *//*--------------------------------------------------------------------*/
class ImageReaderWindow
{
public:
    ImageReaderWindow(int width, int height) : m_reader(nullptr), m_window(nullptr)
    {
        const media_status_t status =
            AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_RGBA_8888,
                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                          AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                                      kMaxImages, &m_reader);
        if (status != AMEDIA_OK || m_reader == nullptr)
            throw tcu::ResourceError("AImageReader_newWithUsage() failed");

        AImageReader_ImageListener listener = {this, onImageAvailable};
        AImageReader_setImageListener(m_reader, &listener);

        if (AImageReader_getWindow(m_reader, &m_window) != AMEDIA_OK || m_window == nullptr)
        {
            AImageReader_delete(m_reader);
            m_reader = nullptr;
            throw tcu::ResourceError("AImageReader_getWindow() failed");
        }
        ANativeWindow_acquire(m_window);
    }

    ~ImageReaderWindow(void)
    {
        if (m_window != nullptr)
            ANativeWindow_release(m_window);
        if (m_reader != nullptr)
        {
            AImageReader_setImageListener(m_reader, nullptr);
            AImageReader_delete(m_reader);
        }
    }

    ANativeWindow *getWindow(void) const
    {
        return m_window;
    }

private:
    static const int kMaxImages = 4;

    static void onImageAvailable(void *, AImageReader *reader)
    {
        AImage *image = nullptr;
        if (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK && image != nullptr)
            AImage_delete(image);
    }

    ImageReaderWindow(const ImageReaderWindow &);
    ImageReaderWindow &operator=(const ImageReaderWindow &);

    AImageReader *m_reader;
    ANativeWindow *m_window;
};
#else
//! Never instantiated on desktop; keeps EglRenderContext's member deletable.
class ImageReaderWindow
{
};
#endif

class GetProcFuncLoader : public glw::FunctionLoader
{
public:
    GetProcFuncLoader(const eglw::Library &egl) : m_egl(egl)
    {
    }

    glw::GenericFuncType get(const char *name) const
    {
        return (glw::GenericFuncType)m_egl.getProcAddress(name);
    }

protected:
    const eglw::Library &m_egl;
};

class EglRenderContext : public glu::RenderContext
{
public:
    EglRenderContext(const glu::RenderConfig &config, const tcu::CommandLine &cmdLine,
                     const glu::RenderContext *sharedContext);
    ~EglRenderContext(void);

    glu::ContextType getType(void) const
    {
        return m_contextType;
    }
    eglw::EGLContext getEglContext(void) const
    {
        return m_eglContext;
    }
    const glw::Functions &getFunctions(void) const
    {
        return m_glFunctions;
    }
    const tcu::RenderTarget &getRenderTarget(void) const
    {
        return m_renderTarget;
    }
    void postIterate(void);
    void makeCurrent(void);

    glw::GenericFuncType getProcAddress(const char *name) const
    {
        return (glw::GenericFuncType)m_egl.getProcAddress(name);
    }

private:
    const eglw::DefaultLibrary m_egl;
    const glu::ContextType m_contextType;
    eglw::EGLDisplay m_eglDisplay;
    eglw::EGLContext m_eglContext;
    eglw::EGLSurface m_eglSurface;
    ImageReaderWindow *m_window;
    glw::Functions m_glFunctions;
    tcu::RenderTarget m_renderTarget;
    eglw::EGLContext m_sharedEglContext;
};

class ContextFactory : public glu::ContextFactory
{
public:
    ContextFactory(void) : glu::ContextFactory("default", "MobileGL EGL context")
    {
    }

    glu::RenderContext *createContext(const glu::RenderConfig &config, const tcu::CommandLine &cmdLine,
                                      const glu::RenderContext *sharedContext) const
    {
        return new EglRenderContext(config, cmdLine, sharedContext);
    }
};

class Platform : public tcu::Platform, public glu::Platform
{
public:
    Platform(void)
    {
        m_contextFactoryRegistry.registerFactory(new ContextFactory());
    }

    const glu::Platform &getGLPlatform(void) const
    {
        return *this;
    }
};

EglRenderContext::EglRenderContext(const glu::RenderConfig &config, const tcu::CommandLine &cmdLine,
                                   const glu::RenderContext *sharedContext)
    : m_egl(getLibraryName().c_str())
    , m_contextType(config.type)
    , m_eglDisplay(EGL_NO_DISPLAY)
    , m_eglContext(EGL_NO_CONTEXT)
    , m_eglSurface(EGL_NO_SURFACE)
    , m_window(nullptr)
    , m_renderTarget(config.width, config.height,
                     tcu::PixelFormat(config.redBits, config.greenBits, config.blueBits, config.alphaBits),
                     config.depthBits, config.stencilBits, config.numSamples)
    , m_sharedEglContext(EGL_NO_CONTEXT)
{
    DE_UNREF(cmdLine);

    const glu::ContextType &contextType = config.type;
    const bool isES                     = glu::isContextTypeES(contextType);
    eglw::EGLint eglMajorVersion        = 0;
    eglw::EGLint eglMinorVersion        = 0;

    m_eglDisplay = m_egl.getDisplay(EGL_DEFAULT_DISPLAY);
    EGLU_CHECK_MSG(m_egl, "eglGetDisplay()");
    if (m_eglDisplay == EGL_NO_DISPLAY)
        throw tcu::ResourceError("eglGetDisplay() failed");

    EGLU_CHECK_CALL(m_egl, initialize(m_eglDisplay, &eglMajorVersion, &eglMinorVersion));

    // MobileGL cannot make a context current without a surface, so
    // SURFACETYPE_DONT_CARE (which is what --deqp-surface-type=fbo requests)
    // still gets a real one.
    bool wantWindow = false;
    switch (config.surfaceType)
    {
    case glu::RenderConfig::SURFACETYPE_WINDOW:
        wantWindow = true;
        break;
    case glu::RenderConfig::SURFACETYPE_OFFSCREEN_NATIVE:
    case glu::RenderConfig::SURFACETYPE_OFFSCREEN_GENERIC:
        wantWindow = false;
        break;
    case glu::RenderConfig::SURFACETYPE_DONT_CARE:
        wantWindow = useWindowSurface();
        break;
    default:
        TCU_CHECK_INTERNAL(false);
    }

    const int width  = (config.width == glu::RenderConfig::DONT_CARE) ? 256 : config.width;
    const int height = (config.height == glu::RenderConfig::DONT_CARE) ? 256 : config.height;

    vector<eglw::EGLint> cfgAttribs;
    cfgAttribs.push_back(EGL_RENDERABLE_TYPE);
    if (isES)
    {
        switch (contextType.getMajorVersion())
        {
        case 3:
            cfgAttribs.push_back(EGL_OPENGL_ES3_BIT);
            break;
        case 2:
            cfgAttribs.push_back(EGL_OPENGL_ES2_BIT);
            break;
        default:
            cfgAttribs.push_back(EGL_OPENGL_ES_BIT);
        }
    }
    else
    {
        // Desktop GL, which is the whole point of this port.
        cfgAttribs.push_back(EGL_OPENGL_BIT);
    }

    cfgAttribs.push_back(EGL_SURFACE_TYPE);
    cfgAttribs.push_back(wantWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT);

    static const struct
    {
        eglw::EGLint attrib;
        int glu::RenderConfig::*field;
    } s_sizeAttribs[] = {
        {EGL_RED_SIZE, &glu::RenderConfig::redBits},     {EGL_GREEN_SIZE, &glu::RenderConfig::greenBits},
        {EGL_BLUE_SIZE, &glu::RenderConfig::blueBits},   {EGL_ALPHA_SIZE, &glu::RenderConfig::alphaBits},
        {EGL_DEPTH_SIZE, &glu::RenderConfig::depthBits}, {EGL_STENCIL_SIZE, &glu::RenderConfig::stencilBits},
        {EGL_SAMPLES, &glu::RenderConfig::numSamples},
    };
    for (size_t ndx = 0; ndx < DE_LENGTH_OF_ARRAY(s_sizeAttribs); ndx++)
    {
        const int value = config.*(s_sizeAttribs[ndx].field);
        if (value != glu::RenderConfig::DONT_CARE)
        {
            cfgAttribs.push_back(s_sizeAttribs[ndx].attrib);
            cfgAttribs.push_back(value);
        }
    }
    cfgAttribs.push_back(EGL_NONE);

    eglw::EGLConfig eglConfig = nullptr;
    eglw::EGLint numConfigs   = 0;
    EGLU_CHECK_CALL(m_egl, chooseConfig(m_eglDisplay, &cfgAttribs[0], &eglConfig, 1, &numConfigs));
    if (numConfigs < 1)
        throw tcu::NotSupportedError("No matching EGL config for the requested context");

    if (wantWindow)
    {
#if defined(__ANDROID__)
        m_window = new ImageReaderWindow(width, height);

        eglw::EGLint visualId = 0;
        if (m_egl.getConfigAttrib(m_eglDisplay, eglConfig, EGL_NATIVE_VISUAL_ID, &visualId) && visualId != 0)
            ANativeWindow_setBuffersGeometry(m_window->getWindow(), width, height, visualId);

        m_eglSurface = m_egl.createWindowSurface(m_eglDisplay, eglConfig,
                                                 (eglw::EGLNativeWindowType)m_window->getWindow(), nullptr);
        EGLU_CHECK_MSG(m_egl, "eglCreateWindowSurface()");
#else
        throw tcu::NotSupportedError("Window surfaces are not supported by the desktop MobileGL platform");
#endif
    }
    else
    {
        const eglw::EGLint surfaceAttribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        m_eglSurface                        = m_egl.createPbufferSurface(m_eglDisplay, eglConfig, surfaceAttribs);
        EGLU_CHECK_MSG(m_egl, "eglCreatePbufferSurface()");
    }

    if (m_eglSurface == EGL_NO_SURFACE)
        throw tcu::ResourceError("Failed to create EGL surface");

    vector<eglw::EGLint> ctxAttribs;
    ctxAttribs.push_back(EGL_CONTEXT_MAJOR_VERSION_KHR);
    ctxAttribs.push_back(contextType.getMajorVersion());
    ctxAttribs.push_back(EGL_CONTEXT_MINOR_VERSION_KHR);
    ctxAttribs.push_back(contextType.getMinorVersion());

    switch (contextType.getProfile())
    {
    case glu::PROFILE_ES:
        EGLU_CHECK_CALL(m_egl, bindAPI(EGL_OPENGL_ES_API));
        break;
    case glu::PROFILE_CORE:
        EGLU_CHECK_CALL(m_egl, bindAPI(EGL_OPENGL_API));
        ctxAttribs.push_back(EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR);
        ctxAttribs.push_back(EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR);
        break;
    case glu::PROFILE_COMPATIBILITY:
        EGLU_CHECK_CALL(m_egl, bindAPI(EGL_OPENGL_API));
        ctxAttribs.push_back(EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR);
        ctxAttribs.push_back(EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR);
        break;
    default:
        TCU_CHECK_INTERNAL(false);
    }

    eglw::EGLint flags = 0;
    if ((contextType.getFlags() & glu::CONTEXT_DEBUG) != 0)
        flags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
    if ((contextType.getFlags() & glu::CONTEXT_ROBUST) != 0)
        flags |= EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
    if ((contextType.getFlags() & glu::CONTEXT_FORWARD_COMPATIBLE) != 0)
        flags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
    if (flags != 0)
    {
        ctxAttribs.push_back(EGL_CONTEXT_FLAGS_KHR);
        ctxAttribs.push_back(flags);
    }
    ctxAttribs.push_back(EGL_NONE);

    const EglRenderContext *sharedEglRenderContext = dynamic_cast<const EglRenderContext *>(sharedContext);
    m_sharedEglContext = sharedEglRenderContext ? sharedEglRenderContext->getEglContext() : EGL_NO_CONTEXT;

    m_eglContext = m_egl.createContext(m_eglDisplay, eglConfig, m_sharedEglContext, &ctxAttribs[0]);
    EGLU_CHECK_MSG(m_egl, "eglCreateContext()");
    if (!m_eglContext)
        throw tcu::ResourceError("eglCreateContext() failed");

    // MobileGL requires draw == read.
    EGLU_CHECK_CALL(m_egl, makeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext));

    // MobileGL advertises EGL 1.5, so eglGetProcAddress resolves core entry
    // points too; there is no separate GL library to dlopen.
    GetProcFuncLoader funcLoader(m_egl);
    glu::initCoreFunctions(&m_glFunctions, &funcLoader, contextType.getAPI());
    glu::initExtensionFunctions(&m_glFunctions, &funcLoader, contextType.getAPI());
}

EglRenderContext::~EglRenderContext(void)
{
    try
    {
        if (m_eglDisplay != EGL_NO_DISPLAY)
        {
            m_egl.makeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

            if (m_eglContext != EGL_NO_CONTEXT)
                m_egl.destroyContext(m_eglDisplay, m_eglContext);

            if (m_eglSurface != EGL_NO_SURFACE)
                m_egl.destroySurface(m_eglDisplay, m_eglSurface);

            if (m_sharedEglContext == EGL_NO_CONTEXT)
                m_egl.terminate(m_eglDisplay);
        }
    }
    catch (...)
    {
    }

    delete m_window;
}

void EglRenderContext::makeCurrent(void)
{
    EGLU_CHECK_CALL(m_egl, makeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext));
}

void EglRenderContext::postIterate(void)
{
    m_glFunctions.finish();
}

} // namespace mobilegl
} // namespace tcu

tcu::Platform *createPlatform(void)
{
    return new tcu::mobilegl::Platform();
}
