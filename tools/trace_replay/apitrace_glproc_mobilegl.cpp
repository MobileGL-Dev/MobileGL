#include "glproc.hpp"

#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

void *_libGlHandle = nullptr;

namespace {

void *LookupSymbol(const char *procName);

void *GetMobileGlHandle() {
    if (_libGlHandle != nullptr) {
        return _libGlHandle;
    }

    const char *library = std::getenv("MOBILEGL_TRACE_LIBRARY");
    if (library != nullptr && library[0] != '\0') {
        _libGlHandle = dlopen(library, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (_libGlHandle == nullptr) {
            _libGlHandle = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (_libGlHandle == nullptr) {
        _libGlHandle = dlopen("libMobileGL.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    }
    if (_libGlHandle == nullptr) {
        _libGlHandle = dlopen("libMobileGL.so", RTLD_NOW | RTLD_GLOBAL);
    }
    return _libGlHandle;
}

using GlReadBuffer = void (*)(GLenum mode);
using GlReadPixels = void (*)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                              void *pixels);
using GlGetIntegerv = void (*)(GLenum pname, GLint *data);
using GlGetError = GLenum (*)();
GlReadBuffer gRealGlReadBuffer = nullptr;
GlReadPixels gRealGlReadPixels = nullptr;
GlGetIntegerv gRealGlGetIntegerv = nullptr;
GlGetError gRealGlGetError = nullptr;
int gSuppressInvalidEnumCount = 0;

void MobileGLTraceReadBuffer(GLenum mode) {
    if (mode == GL_BACK) {
        ++gSuppressInvalidEnumCount;
        return;
    }
    if (gRealGlReadBuffer == nullptr) {
        gRealGlReadBuffer = reinterpret_cast<GlReadBuffer>(LookupSymbol("glReadBuffer"));
    }
    if (gRealGlReadBuffer != nullptr) {
        gRealGlReadBuffer(mode);
    }
}

void MobileGLTraceReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                             void *pixels) {
    if (gRealGlReadPixels == nullptr) {
        gRealGlReadPixels = reinterpret_cast<GlReadPixels>(LookupSymbol("glReadPixels"));
    }
    if (gRealGlReadPixels != nullptr) {
        gRealGlReadPixels(x, y, width, height, format, type, pixels);
    }
}

void MobileGLTraceGetIntegerv(GLenum pname, GLint *data) {
    if (pname == GL_READ_BUFFER) {
        if (data != nullptr) {
            *data = GL_BACK;
        }
        ++gSuppressInvalidEnumCount;
        return;
    }
    if (gRealGlGetIntegerv == nullptr) {
        gRealGlGetIntegerv = reinterpret_cast<GlGetIntegerv>(LookupSymbol("glGetIntegerv"));
    }
    if (gRealGlGetIntegerv != nullptr) {
        gRealGlGetIntegerv(pname, data);
    }
}

GLenum MobileGLTraceGetError() {
    if (gRealGlGetError == nullptr) {
        gRealGlGetError = reinterpret_cast<GlGetError>(LookupSymbol("glGetError"));
    }
    if (gRealGlGetError == nullptr) {
        return GL_NO_ERROR;
    }
    GLenum error = gRealGlGetError();
    if (error == GL_INVALID_ENUM && gSuppressInvalidEnumCount > 0) {
        --gSuppressInvalidEnumCount;
        return GL_NO_ERROR;
    }
    return error;
}

void *LookupSymbol(const char *procName) {
    void *mobileGl = GetMobileGlHandle();
    if (mobileGl != nullptr) {
        void *proc = dlsym(mobileGl, procName);
        if (proc != nullptr) {
            return proc;
        }
    }

    return dlsym(RTLD_DEFAULT, procName);
}

} // namespace

void *_getPublicProcAddress(const char *procName) {
    if (strcmp(procName, "glReadPixels") == 0) {
        return reinterpret_cast<void *>(&MobileGLTraceReadPixels);
    }
    if (strcmp(procName, "glReadBuffer") == 0) {
        return reinterpret_cast<void *>(&MobileGLTraceReadBuffer);
    }
    if (strcmp(procName, "glGetIntegerv") == 0) {
        return reinterpret_cast<void *>(&MobileGLTraceGetIntegerv);
    }
    if (strcmp(procName, "glGetError") == 0) {
        return reinterpret_cast<void *>(&MobileGLTraceGetError);
    }
    return LookupSymbol(procName);
}

void *_getPrivateProcAddress(const char *procName) {
    void *proc = LookupSymbol(procName);
    if (proc != nullptr) {
        return proc;
    }

    if (_eglGetProcAddress != nullptr) {
        return reinterpret_cast<void *>(_eglGetProcAddress(procName));
    }

    return nullptr;
}
