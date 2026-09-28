// MobileGL - MetalANGLE GLES 3.2 Compatibility Shim
// Translates OpenGL ES 3.1 & 3.2 entry points to MetalANGLE native extensions and emulated paths.

#include "MetalANGLE_GLES32_Shim.h"
#include <cstring>
#include <algorithm>

namespace MobileGL::MetalANGLE_Shim {

    namespace {
        // Saved original functions table
        MG_External::GLESFunctionsTable s_origFuncs{};

        // Extension function prototypes
        using FnDrawElementsBaseVertexEXT = void (GL_APIENTRY*)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex);
        using FnDrawRangeElementsBaseVertexEXT = void (GL_APIENTRY*)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex);
        using FnDrawElementsInstancedBaseVertexEXT = void (GL_APIENTRY*)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex);
        using FnBlendFunciEXT = void (GL_APIENTRY*)(GLuint buf, GLenum src, GLenum dst);
        using FnBlendFuncSeparateiEXT = void (GL_APIENTRY*)(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
        using FnBlendEquationiEXT = void (GL_APIENTRY*)(GLuint buf, GLenum mode);
        using FnBlendEquationSeparateiEXT = void (GL_APIENTRY*)(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
        using FnFramebufferTextureEXT = void (GL_APIENTRY*)(GLenum target, GLenum attachment, GLuint texture, GLint level);
        using FnCopyImageSubDataEXT = void (GL_APIENTRY*)(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
        using FnTexStorage2DMultisampleANGLE = void (GL_APIENTRY*)(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);

        // Extension function pointers
        static FnDrawElementsBaseVertexEXT s_glDrawElementsBaseVertexEXT = nullptr;
        static FnDrawRangeElementsBaseVertexEXT s_glDrawRangeElementsBaseVertexEXT = nullptr;
        static FnDrawElementsInstancedBaseVertexEXT s_glDrawElementsInstancedBaseVertexEXT = nullptr;
        static FnBlendFunciEXT s_glBlendFunciEXT = nullptr;
        static FnBlendFuncSeparateiEXT s_glBlendFuncSeparateiEXT = nullptr;
        static FnBlendEquationiEXT s_glBlendEquationiEXT = nullptr;
        static FnBlendEquationSeparateiEXT s_glBlendEquationSeparateiEXT = nullptr;
        static FnFramebufferTextureEXT s_glFramebufferTextureEXT = nullptr;
        static FnCopyImageSubDataEXT s_glCopyImageSubDataEXT = nullptr;
        static FnTexStorage2DMultisampleANGLE s_glTexStorage2DMultisampleANGLE = nullptr;

        // Command structures for indirect draws
        struct DrawArraysIndirectCommand {
            GLuint count;
            GLuint instanceCount;
            GLuint first;
            GLuint baseInstance;
        };

        struct DrawElementsIndirectCommand {
            GLuint count;
            GLuint instanceCount;
            GLuint firstIndex;
            GLint  baseVertex;
            GLuint baseInstance;
        };

        inline size_t IndexSize(GLenum type) {
            switch (type) {
                case GL_UNSIGNED_BYTE:  return 1;
                case GL_UNSIGNED_SHORT: return 2;
                case GL_UNSIGNED_INT:   return 4;
                default:                return 2;
            }
        }
    } // namespace

    // Stub for glShaderStorageBlockBinding
    void GL_APIENTRY Stub_glShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding) {
        (void)program;
        (void)storageBlockIndex;
        (void)storageBlockBinding;
        // No-op stub: Desktop GL 4.3 entry point not natively present in GLES drivers.
        // MobileGL program state tracks storage block bindings independently during transpilation.
    }

    // Base Vertex Draw Shims
    static void GL_APIENTRY Shim_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
        if (s_glDrawElementsBaseVertexEXT) {
            s_glDrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
        } else if (s_origFuncs.glDrawElementsBaseVertex) {
            s_origFuncs.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
        } else {
            s_origFuncs.glDrawElements(mode, count, type, indices);
        }
    }

    static void GL_APIENTRY Shim_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
        (void)start;
        (void)end;
        if (s_glDrawRangeElementsBaseVertexEXT) {
            s_glDrawRangeElementsBaseVertexEXT(mode, start, end, count, type, indices, basevertex);
        } else if (s_origFuncs.glDrawRangeElementsBaseVertex) {
            s_origFuncs.glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
        } else if (s_glDrawElementsBaseVertexEXT) {
            s_glDrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
        } else {
            s_origFuncs.glDrawRangeElements(mode, start, end, count, type, indices);
        }
    }

    static void GL_APIENTRY Shim_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex) {
        if (s_glDrawElementsInstancedBaseVertexEXT) {
            s_glDrawElementsInstancedBaseVertexEXT(mode, count, type, indices, instancecount, basevertex);
        } else if (s_origFuncs.glDrawElementsInstancedBaseVertex) {
            s_origFuncs.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
        } else if (s_glDrawElementsBaseVertexEXT && instancecount == 1) {
            s_glDrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
        } else {
            s_origFuncs.glDrawElementsInstanced(mode, count, type, indices, instancecount);
        }
    }

    // Indirect Draw Emulation
    static void GL_APIENTRY Shim_glDrawArraysIndirect(GLenum mode, const void* indirect) {
        GLint indirectBuf = 0;
        s_origFuncs.glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &indirectBuf);
        DrawArraysIndirectCommand cmd{};
        if (indirectBuf != 0) {
            const void* ptr = s_origFuncs.glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, reinterpret_cast<GLintptr>(indirect), sizeof(cmd), GL_MAP_READ_BIT);
            if (ptr) {
                std::memcpy(&cmd, ptr, sizeof(cmd));
                s_origFuncs.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
            }
        } else if (indirect != nullptr) {
            std::memcpy(&cmd, indirect, sizeof(cmd));
        }

        if (cmd.count == 0 || cmd.instanceCount == 0) return;

        if (cmd.baseInstance != 0 && s_origFuncs.glDrawArraysInstancedBaseInstanceEXT) {
            s_origFuncs.glDrawArraysInstancedBaseInstanceEXT(mode, cmd.first, cmd.count, cmd.instanceCount, cmd.baseInstance);
        } else {
            s_origFuncs.glDrawArraysInstanced(mode, cmd.first, cmd.count, cmd.instanceCount);
        }
    }

    static void GL_APIENTRY Shim_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        GLint indirectBuf = 0;
        s_origFuncs.glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &indirectBuf);
        DrawElementsIndirectCommand cmd{};
        if (indirectBuf != 0) {
            const void* ptr = s_origFuncs.glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, reinterpret_cast<GLintptr>(indirect), sizeof(cmd), GL_MAP_READ_BIT);
            if (ptr) {
                std::memcpy(&cmd, ptr, sizeof(cmd));
                s_origFuncs.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
            }
        } else if (indirect != nullptr) {
            std::memcpy(&cmd, indirect, sizeof(cmd));
        }

        if (cmd.count == 0 || cmd.instanceCount == 0) return;

        const void* indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd.firstIndex * IndexSize(type)));

        if (cmd.baseVertex != 0) {
            if (cmd.baseInstance != 0 && s_origFuncs.glDrawElementsInstancedBaseVertexBaseInstanceEXT) {
                s_origFuncs.glDrawElementsInstancedBaseVertexBaseInstanceEXT(mode, cmd.count, type, indices, cmd.instanceCount, cmd.baseVertex, cmd.baseInstance);
            } else if (s_glDrawElementsInstancedBaseVertexEXT) {
                s_glDrawElementsInstancedBaseVertexEXT(mode, cmd.count, type, indices, cmd.instanceCount, cmd.baseVertex);
            } else if (s_glDrawElementsBaseVertexEXT && cmd.instanceCount == 1) {
                s_glDrawElementsBaseVertexEXT(mode, cmd.count, type, indices, cmd.baseVertex);
            } else {
                s_origFuncs.glDrawElementsInstanced(mode, cmd.count, type, indices, cmd.instanceCount);
            }
        } else {
            if (cmd.baseInstance != 0 && s_origFuncs.glDrawElementsInstancedBaseInstanceEXT) {
                s_origFuncs.glDrawElementsInstancedBaseInstanceEXT(mode, cmd.count, type, indices, cmd.instanceCount, cmd.baseInstance);
            } else {
                s_origFuncs.glDrawElementsInstanced(mode, cmd.count, type, indices, cmd.instanceCount);
            }
        }
    }

    // Color Mask & Blend Shims
    static void GL_APIENTRY Shim_glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
        if (s_origFuncs.glColorMaskiEXT) {
            s_origFuncs.glColorMaskiEXT(index, r, g, b, a);
        } else if (s_origFuncs.glColorMaskiOES) {
            s_origFuncs.glColorMaskiOES(index, r, g, b, a);
        } else if (index == 0) {
            s_origFuncs.glColorMask(r, g, b, a);
        }
    }

    static void GL_APIENTRY Shim_glBlendFunci(GLuint buf, GLenum src, GLenum dst) {
        if (s_glBlendFunciEXT) {
            s_glBlendFunciEXT(buf, src, dst);
        } else if (buf == 0) {
            s_origFuncs.glBlendFunc(src, dst);
        }
    }

    static void GL_APIENTRY Shim_glBlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
        if (s_glBlendFuncSeparateiEXT) {
            s_glBlendFuncSeparateiEXT(buf, srcRGB, dstRGB, srcAlpha, dstAlpha);
        } else if (buf == 0) {
            s_origFuncs.glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
        }
    }

    static void GL_APIENTRY Shim_glBlendEquationi(GLuint buf, GLenum mode) {
        if (s_glBlendEquationiEXT) {
            s_glBlendEquationiEXT(buf, mode);
        } else if (buf == 0) {
            s_origFuncs.glBlendEquation(mode);
        }
    }

    static void GL_APIENTRY Shim_glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha) {
        if (s_glBlendEquationSeparateiEXT) {
            s_glBlendEquationSeparateiEXT(buf, modeRGB, modeAlpha);
        } else if (buf == 0) {
            s_origFuncs.glBlendEquationSeparate(modeRGB, modeAlpha);
        }
    }

    static void GL_APIENTRY Shim_glEnablei(GLenum target, GLuint index) {
        if (index == 0) s_origFuncs.glEnable(target);
    }

    static void GL_APIENTRY Shim_glDisablei(GLenum target, GLuint index) {
        if (index == 0) s_origFuncs.glDisable(target);
    }

    static GLboolean GL_APIENTRY Shim_glIsEnabledi(GLenum target, GLuint index) {
        if (index == 0) return s_origFuncs.glIsEnabled(target);
        return GL_FALSE;
    }

    // Multisample Storage & Framebuffer Shims
    static void GL_APIENTRY Shim_glTexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                           GLsizei width, GLsizei height, GLboolean fixedsamplelocations) {
        if (s_glTexStorage2DMultisampleANGLE) {
            s_glTexStorage2DMultisampleANGLE(target, samples, internalformat, width, height, fixedsamplelocations);
        } else if (s_origFuncs.glTexStorage2DMultisample) {
            s_origFuncs.glTexStorage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
        }
    }

    // TEXTURE_2D_MULTISAMPLE_ARRAY allocation (glTexStorage3DMultisample) has no
    // ES 3.0 equivalent: no EXT/OES alias exists and the array target cannot be
    // emulated with plain 2D storage. Deliberate no-op safety net — the format
    // probe gate (IsTextureMultisampleArraySupported) keeps this target out of
    // the caps on ES 3.0 drivers, so this must never fire in practice; if it
    // does (a missed gate), declining noiselessly beats faulting inside ANGLE's
    // non-null stub (SIGSEGV, see hs_err_pid34339).
    static void GL_APIENTRY Shim_glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                           GLsizei width, GLsizei height, GLsizei depth,
                                                           GLboolean fixedsamplelocations) {
        (void)target; (void)samples; (void)internalformat;
        (void)width; (void)height; (void)depth; (void)fixedsamplelocations;
    }

    static void GL_APIENTRY Shim_glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
        if (s_glFramebufferTextureEXT) {
            s_glFramebufferTextureEXT(target, attachment, texture, level);
        } else {
            s_origFuncs.glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, texture, level);
        }
    }

    static void GL_APIENTRY Shim_glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                                    GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                                    GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        if (s_glCopyImageSubDataEXT) {
            s_glCopyImageSubDataEXT(srcName, srcTarget, srcLevel, srcX, srcY, srcZ,
                                    dstName, dstTarget, dstLevel, dstX, dstY, dstZ,
                                    srcWidth, srcHeight, srcDepth);
        }
    }

    // Texture and Sampler Integer Parameters
    static void GL_APIENTRY Shim_glTexParameterIiv(GLenum target, GLenum pname, const GLint *params) {
        if (s_origFuncs.glTexParameteriv) s_origFuncs.glTexParameteriv(target, pname, params);
    }

    static void GL_APIENTRY Shim_glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *params) {
        if (s_origFuncs.glTexParameteriv) s_origFuncs.glTexParameteriv(target, pname, reinterpret_cast<const GLint*>(params));
    }

    static void GL_APIENTRY Shim_glGetTexParameterIiv(GLenum target, GLenum pname, GLint *params) {
        if (s_origFuncs.glGetTexParameteriv) s_origFuncs.glGetTexParameteriv(target, pname, params);
    }

    static void GL_APIENTRY Shim_glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint *params) {
        if (s_origFuncs.glGetTexParameteriv) s_origFuncs.glGetTexParameteriv(target, pname, reinterpret_cast<GLint*>(params));
    }

    static void GL_APIENTRY Shim_glSamplerParameterIiv(GLuint sampler, GLenum pname, const GLint *params) {
        if (s_origFuncs.glSamplerParameteriv) s_origFuncs.glSamplerParameteriv(sampler, pname, params);
    }

    static void GL_APIENTRY Shim_glSamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint *params) {
        if (s_origFuncs.glSamplerParameteriv) s_origFuncs.glSamplerParameteriv(sampler, pname, reinterpret_cast<const GLint*>(params));
    }

    static void GL_APIENTRY Shim_glGetSamplerParameterIiv(GLuint sampler, GLenum pname, GLint *params) {
        if (s_origFuncs.glGetSamplerParameteriv) s_origFuncs.glGetSamplerParameteriv(sampler, pname, params);
    }

    static void GL_APIENTRY Shim_glGetSamplerParameterIuiv(GLuint sampler, GLenum pname, GLuint *params) {
        if (s_origFuncs.glGetSamplerParameteriv) s_origFuncs.glGetSamplerParameteriv(sampler, pname, reinterpret_cast<GLint*>(params));
    }

    // Utility & Minor GLES 3.2 APIs
    static GLenum GL_APIENTRY Shim_glGetGraphicsResetStatus() {
        return GL_NO_ERROR;
    }

    static void GL_APIENTRY Shim_glReadnPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void *data) {
        (void)bufSize;
        if (s_origFuncs.glReadPixels) s_origFuncs.glReadPixels(x, y, width, height, format, type, data);
    }

    static void GL_APIENTRY Shim_glGetnUniformfv(GLuint program, GLint location, GLsizei bufSize, GLfloat *params) {
        (void)bufSize;
        if (s_origFuncs.glGetUniformfv) s_origFuncs.glGetUniformfv(program, location, params);
    }

    static void GL_APIENTRY Shim_glGetnUniformiv(GLuint program, GLint location, GLsizei bufSize, GLint *params) {
        (void)bufSize;
        if (s_origFuncs.glGetUniformiv) s_origFuncs.glGetUniformiv(program, location, params);
    }

    static void GL_APIENTRY Shim_glGetnUniformuiv(GLuint program, GLint location, GLsizei bufSize, GLuint *params) {
        (void)bufSize;
        if (s_origFuncs.glGetUniformuiv) s_origFuncs.glGetUniformuiv(program, location, params);
    }

    static void GL_APIENTRY Shim_glMinSampleShading(GLfloat value) {
        (void)value;
    }

    static void GL_APIENTRY Shim_glPatchParameteri(GLenum pname, GLint value) {
        (void)pname;
        (void)value;
    }

    static void GL_APIENTRY Shim_glPrimitiveBoundingBox(GLfloat minX, GLfloat minY, GLfloat minZ, GLfloat minW,
                                                        GLfloat maxX, GLfloat maxY, GLfloat maxZ, GLfloat maxW) {
        (void)minX; (void)minY; (void)minZ; (void)minW;
        (void)maxX; (void)maxY; (void)maxZ; (void)maxW;
    }

    void InstallGLES32Shims(MG_External::GLESFunctionsTable& funcs, MG_External::EGL::eglGetProcAddress_PTR procAddress) {
        s_origFuncs = funcs;

        // Query extension entry points via procAddress
        if (procAddress) {
            s_glDrawElementsBaseVertexEXT = reinterpret_cast<FnDrawElementsBaseVertexEXT>(
                procAddress("glDrawElementsBaseVertexEXT"));
            if (!s_glDrawElementsBaseVertexEXT) {
                s_glDrawElementsBaseVertexEXT = reinterpret_cast<FnDrawElementsBaseVertexEXT>(
                    procAddress("glDrawElementsBaseVertexOES"));
            }

            s_glDrawRangeElementsBaseVertexEXT = reinterpret_cast<FnDrawRangeElementsBaseVertexEXT>(
                procAddress("glDrawRangeElementsBaseVertexEXT"));
            if (!s_glDrawRangeElementsBaseVertexEXT) {
                s_glDrawRangeElementsBaseVertexEXT = reinterpret_cast<FnDrawRangeElementsBaseVertexEXT>(
                    procAddress("glDrawRangeElementsBaseVertexOES"));
            }

            s_glDrawElementsInstancedBaseVertexEXT = reinterpret_cast<FnDrawElementsInstancedBaseVertexEXT>(
                procAddress("glDrawElementsInstancedBaseVertexEXT"));
            if (!s_glDrawElementsInstancedBaseVertexEXT) {
                s_glDrawElementsInstancedBaseVertexEXT = reinterpret_cast<FnDrawElementsInstancedBaseVertexEXT>(
                    procAddress("glDrawElementsInstancedBaseVertexOES"));
            }

            s_glBlendFunciEXT = reinterpret_cast<FnBlendFunciEXT>(
                procAddress("glBlendFunciEXT"));
            if (!s_glBlendFunciEXT) {
                s_glBlendFunciEXT = reinterpret_cast<FnBlendFunciEXT>(
                    procAddress("glBlendFunciOES"));
            }

            s_glBlendFuncSeparateiEXT = reinterpret_cast<FnBlendFuncSeparateiEXT>(
                procAddress("glBlendFuncSeparateiEXT"));
            if (!s_glBlendFuncSeparateiEXT) {
                s_glBlendFuncSeparateiEXT = reinterpret_cast<FnBlendFuncSeparateiEXT>(
                    procAddress("glBlendFuncSeparateiOES"));
            }

            s_glBlendEquationiEXT = reinterpret_cast<FnBlendEquationiEXT>(
                procAddress("glBlendEquationiEXT"));
            if (!s_glBlendEquationiEXT) {
                s_glBlendEquationiEXT = reinterpret_cast<FnBlendEquationiEXT>(
                    procAddress("glBlendEquationiOES"));
            }

            s_glBlendEquationSeparateiEXT = reinterpret_cast<FnBlendEquationSeparateiEXT>(
                procAddress("glBlendEquationSeparateiEXT"));
            if (!s_glBlendEquationSeparateiEXT) {
                s_glBlendEquationSeparateiEXT = reinterpret_cast<FnBlendEquationSeparateiEXT>(
                    procAddress("glBlendEquationSeparateiOES"));
            }

            s_glFramebufferTextureEXT = reinterpret_cast<FnFramebufferTextureEXT>(
                procAddress("glFramebufferTextureEXT"));
            if (!s_glFramebufferTextureEXT) {
                s_glFramebufferTextureEXT = reinterpret_cast<FnFramebufferTextureEXT>(
                    procAddress("glFramebufferTextureOES"));
            }

            s_glCopyImageSubDataEXT = reinterpret_cast<FnCopyImageSubDataEXT>(
                procAddress("glCopyImageSubDataEXT"));
            if (!s_glCopyImageSubDataEXT) {
                s_glCopyImageSubDataEXT = reinterpret_cast<FnCopyImageSubDataEXT>(
                    procAddress("glCopyImageSubDataOES"));
            }

            s_glTexStorage2DMultisampleANGLE = reinterpret_cast<FnTexStorage2DMultisampleANGLE>(
                procAddress("glTexStorage2DMultisampleANGLE"));
        }

        // Install stubs and wrappers
        if (!funcs.glShaderStorageBlockBinding) {
            funcs.glShaderStorageBlockBinding = &Stub_glShaderStorageBlockBinding;
        }

        // Base vertex draw redirects
        funcs.glDrawElementsBaseVertex = &Shim_glDrawElementsBaseVertex;
        funcs.glDrawRangeElementsBaseVertex = &Shim_glDrawRangeElementsBaseVertex;
        funcs.glDrawElementsInstancedBaseVertex = &Shim_glDrawElementsInstancedBaseVertex;

        // Indirect draw emulation
        funcs.glDrawArraysIndirect = &Shim_glDrawArraysIndirect;
        funcs.glDrawElementsIndirect = &Shim_glDrawElementsIndirect;

        // Color mask & blend
        funcs.glColorMaski = &Shim_glColorMaski;
        funcs.glBlendFunci = &Shim_glBlendFunci;
        funcs.glBlendFuncSeparatei = &Shim_glBlendFuncSeparatei;
        funcs.glBlendEquationi = &Shim_glBlendEquationi;
        funcs.glBlendEquationSeparatei = &Shim_glBlendEquationSeparatei;
        funcs.glEnablei = &Shim_glEnablei;
        funcs.glDisablei = &Shim_glDisablei;
        funcs.glIsEnabledi = &Shim_glIsEnabledi;

        // Multisample & Framebuffer
        funcs.glTexStorage2DMultisample = &Shim_glTexStorage2DMultisample;
        // 3D-multisample (2D array) has no ES 3.0 path: unconditional no-op so a
        // missed gate can never reach ANGLE's faulting stub (see above).
        funcs.glTexStorage3DMultisample = &Shim_glTexStorage3DMultisample;
        funcs.glFramebufferTexture = &Shim_glFramebufferTexture;
        funcs.glCopyImageSubData = &Shim_glCopyImageSubData;

        // Texture & Sampler integer parameters
        funcs.glTexParameterIiv = &Shim_glTexParameterIiv;
        funcs.glTexParameterIuiv = &Shim_glTexParameterIuiv;
        funcs.glGetTexParameterIiv = &Shim_glGetTexParameterIiv;
        funcs.glGetTexParameterIuiv = &Shim_glGetTexParameterIuiv;
        funcs.glSamplerParameterIiv = &Shim_glSamplerParameterIiv;
        funcs.glSamplerParameterIuiv = &Shim_glSamplerParameterIuiv;
        funcs.glGetSamplerParameterIiv = &Shim_glGetSamplerParameterIiv;
        funcs.glGetSamplerParameterIuiv = &Shim_glGetSamplerParameterIuiv;

        // Reset status & readn
        funcs.glGetGraphicsResetStatus = &Shim_glGetGraphicsResetStatus;
        funcs.glReadnPixels = &Shim_glReadnPixels;
        funcs.glGetnUniformfv = &Shim_glGetnUniformfv;
        funcs.glGetnUniformiv = &Shim_glGetnUniformiv;
        funcs.glGetnUniformuiv = &Shim_glGetnUniformuiv;
        funcs.glMinSampleShading = &Shim_glMinSampleShading;
        funcs.glPatchParameteri = &Shim_glPatchParameteri;
        funcs.glPrimitiveBoundingBox = &Shim_glPrimitiveBoundingBox;
    }

    void PatchMetalANGLECapabilities(MG_External::GLESCapabilities& caps, const MG_External::GLESFunctionsTable& funcs) {
        // MetalANGLE exposes ES 3.0 only. The shims installed above emulate the
        // STATELESS 3.1/3.2 entry points (indirect draws via MapBuffer+Instanced,
        // baseVertex via EXT/OES alias or non-base fallback, indexed blend/mask
        // via EXT/OES alias or buf0 fallback, texStorageMS/framebufferTexture/
        // copyImage via EXT alias, integer tex/sampler params via iv alias,
        // readn/getn via non-n alias). Those caps are forced true: the shim IS
        // the implementation.
        //
        // What is deliberately NOT forced true (cannot be emulated on ES 3.0):
        //  - compute (glDispatchCompute/glMemoryBarrier/...): needs a real
        //    compute stage. Stays pointer-gated -> false on MetalANGLE, so the
        //    frontend declines compute dispatches (see DirectGLES null-guards).
        //    Mods that require compute (Voxy geometry, some Sodium paths) need
        //    VulkanANGLE (ES 3.2) instead.
        //  - tessellation / geometry stages (GL_PATCHES, TCS/TES/GS): need real
        //    pipeline stages; PatchParameteri/PrimitiveBoundingBox shims are
        //    no-ops and tess/geometry programs will fail to link -> frontend
        //    falls back. Iris tessellation/shaders need VulkanANGLE.
        //  - texture buffer (GL_TEXTURE_BUFFER): tier-gated to None on 3.0.
        // Enable features backed by the MetalANGLE compatibility shim
        caps.SupportsDrawElementsBaseVertex = true;
        caps.SupportsDrawIndirect = true;
        caps.SupportsComputeShader = (funcs.glDispatchCompute != nullptr && funcs.glMemoryBarrier != nullptr);
        caps.SupportsIndexedColorMask = (funcs.glColorMaski != nullptr);
        caps.SupportsDualSourceBlend = true;

        if (caps.ShaderStorageBufferOffsetAlignment == 0) {
            caps.ShaderStorageBufferOffsetAlignment = 256;
        }
    }
}
