//
// Created by BZLZHH on 2025/3/15.
//

#include "GL_Getter.h"

namespace MG_GL::GL {
    static std::string rendererString;
    static bool loggedMGInfo;
    const GLubyte* GetString(GLenum name) {
        if (!loggedMGInfo) {
            loggedMGInfo = true;
            MG_GL::Getter::LogMGInfo();
        }
        MG_Util::Debug::LogD("glGetString, name: %s", MG_Util::Debug::GLEnumToString(name));
        switch (name) {
            case GL_VENDOR: 
                return (const GLubyte *)"MobileGL-Dev";
            case GL_VERSION: {
                static std::string versionStr;
                if (versionStr.empty()) {
                    versionStr = std::to_string(MG_Global::GL::GLVersionMajor) + "."
                            +  std::to_string(MG_Global::GL::GLVersionMinor) + "."
                            +  std::to_string(MG_Global::GL::GLVersionRevision) 
                            + " "+ MG_GL::Getter::GetMGName() +
                            ", MobileGL Core, " + MG_GL::Getter::GetBackendName() + " Backend, Version "
                            + std::to_string(MG_Global::MG::VersionMajor) + "."
                            + std::to_string(MG_Global::MG::VersionMinor) + "."
                            + std::to_string(MG_Global::MG::VersionRevision);
                    if (MG_Global::MG::VersionPatch != 0)
                        versionStr += "." + std::to_string(MG_Global::MG::VersionPatch);
                    versionStr += MG_Global::MG::VersionSuffix;
                }
                return (const GLubyte *)versionStr.c_str();
            }

            case GL_RENDERER:
            {
                /*
                if (rendererString == std::string("")) {
                    const char* gpuName = getGpuName();
                    const char* glesName = getGLESName();
                    rendererString = std::string(gpuName) + " | " + std::string(glesName);
                }
                return (const GLubyte *)rendererString.c_str();
                 */
                return (const GLubyte *)(MG_GL::Getter::GetMGName() + " (MobileGL Core) Renderer").c_str();
            }
            case GL_SHADING_LANGUAGE_VERSION:
                return (const GLubyte *) "4.50 MobileGL with glslang";
            case GL_EXTENSIONS:
                return (const GLubyte *) "OpenGL11 "
                                         "OpenGL12 "
                                         "OpenGL13 "
                                         "OpenGL14 "
                                         "OpenGL15 "
                                         "OpenGL20 "
                                         "OpenGL21 "
                                         "OpenGL30 ";
            default:
                return (const GLubyte *) "Unknown enum";
        }
    }

    const GLubyte* GetStringi(GLenum name, GLuint index) {
        MG_Util::Debug::LogD("glGetStringi, name: %s, index: %d", MG_Util::Debug::GLEnumToString(name), index);
        if (name != GL_EXTENSIONS)
            return (const GLubyte*)"";
        typedef struct {
            GLenum name;
            const char** parts;
            GLuint count;
        } StringCache;
        static StringCache caches[] = {
                {GL_EXTENSIONS, nullptr, 0},
        };
        static int initialized = 0;
        if (!initialized) {
            for (auto & cache : caches) {
                GLenum target = cache.name;
                const GLubyte* str = nullptr;
                const char* delimiter = " ";
                str = GetString(GL_EXTENSIONS);
                
                char* copy = strdup((const char*)str);
                char* token = strtok(copy, delimiter);
                while (token) {
                    cache.parts = (const char**)realloc(cache.parts, (cache.count + 1) * sizeof(char*));
                    cache.parts[cache.count++] = strdup(token);
                    token = strtok(nullptr, delimiter);
                }
                free(copy);
            }
            initialized = 1;
        }

        for (auto & cache : caches) {
            if (cache.name == name) {
                if (index >= cache.count) {
                    return nullptr;
                }
                return (const GLubyte*)cache.parts[index];
            }
        }

        return nullptr;
    }

    GLenum GetError() {
        GLenum error = MG_State::GetError();
        MG_Util::Debug::LogD("glGetError -> %s", MG_Util::Debug::GLEnumToString(error));
        return error;
    }

    void GetIntegerv(GLenum pname, GLint *params) {
        MG_Util::Debug::LogD("glGetIntegerv, pname: %s", MG_Util::Debug::GLEnumToString(pname));
        if (!params) {
            MG_State::SetError(GL_INVALID_VALUE);
            return;
        }

        switch (pname) {
            // General
            case GL_CONTEXT_PROFILE_MASK:
                params[0] = GL_CONTEXT_CORE_PROFILE_BIT;
                break;
            case GL_NUM_EXTENSIONS:
                static GLint num_extensions = -1;
                if (num_extensions == -1) {
                    const GLubyte* ext_str = MG_GL::GL::GetString(GL_EXTENSIONS);
                    char *copy = strdup((const char *) ext_str);
                    char *token = strtok(copy, " ");
                    num_extensions = 0;
                    while (token) {
                        num_extensions++;
                        token = strtok(nullptr, " ");
                    }
                    free(copy);
                }
                params[0] = num_extensions;
                break;
            case GL_MAJOR_VERSION:
                params[0] = MG_Global::GL::GLVersionMajor;
                break;
            case GL_MINOR_VERSION:
                params[0] = MG_Global::GL::GLVersionMinor;
                break;
            
            // Viewport and scissor
            case GL_VIEWPORT:
                memcpy(params, MG_State_T::commonState->viewport, 4 * sizeof(GLint));
                break;
            case GL_SCISSOR_BOX:
                // TODO: memcpy(params, MG_State_T::commonState->scissorBox, 4 * sizeof(GLint));
                break;
            case GL_MAX_VIEWPORT_DIMS: {
                static const GLint maxDims[2] = {16384, 16384};
                memcpy(params, maxDims, sizeof(maxDims));
                break;
            }

            // Buffer bindings
            case GL_ARRAY_BUFFER_BINDING:
                params[0] = static_cast<GLint>(MG_State_T::bufferState->GetCurrentBinding(GL_ARRAY_BUFFER));
                break;

            case GL_ELEMENT_ARRAY_BUFFER_BINDING:
                params[0] = static_cast<GLint>(MG_State_T::vertexArrayState->GetBoundElementBuffer());
                break;
            case GL_DRAW_FRAMEBUFFER_BINDING:
                params[0] = static_cast<GLint>(MG_State_T::framebufferState->currentBindings_[GL_DRAW_FRAMEBUFFER]);
                break;
            case GL_READ_FRAMEBUFFER_BINDING:
                params[0] = static_cast<GLint>(MG_State_T::framebufferState->currentBindings_[GL_READ_FRAMEBUFFER]);
                break;
            case GL_RENDERBUFFER_BINDING:
                // TODO: params[0] = static_cast<GLint>(MG_State_T::framebufferState->currentRenderbuffer_);
                break;

            // Program state
            case GL_CURRENT_PROGRAM:
                params[0] = static_cast<GLint>(MG_State_T::programState->currentProgram_);
                break;
            case GL_MAX_VERTEX_ATTRIBS:
                // TODO: Check the real value
                params[0] = 64;
                break;
                
            // Texture state
            case GL_TEXTURE_BINDING_2D: {
                GLuint unit = MG_State_T::textureState->activeTextureUnit_;
                auto& textures = MG_State_T::textureState->textureUnits_[unit].boundTextures;
                params[0] = textures.count(GL_TEXTURE_2D) ? static_cast<GLint>(textures[GL_TEXTURE_2D]) : 0;
                break;
            }
            case GL_ACTIVE_TEXTURE:
                params[0] = GL_TEXTURE0 + MG_State_T::textureState->activeTextureUnit_;
                break;
            case GL_MAX_TEXTURE_SIZE:
                params[0] = 8192;
                break;

            // Blend state
            case GL_BLEND_SRC_RGB:
                params[0] = static_cast<GLint>(MG_State_T::commonState->blendSrcRGB);
                break;
            case GL_BLEND_DST_RGB:
                params[0] = static_cast<GLint>(MG_State_T::commonState->blendDstRGB);
                break;
            case GL_BLEND_SRC_ALPHA:
                params[0] = static_cast<GLint>(MG_State_T::commonState->blendSrcAlpha);
                break;
            case GL_BLEND_DST_ALPHA:
                params[0] = static_cast<GLint>(MG_State_T::commonState->blendDstAlpha);
                break;
                
            // Depth and stencil
            case GL_DEPTH_FUNC:
                params[0] = static_cast<GLint>(MG_State_T::commonState->depthFunc);
                break;
            case GL_DEPTH_WRITEMASK:
                params[0] = static_cast<GLint>(MG_State_T::commonState->depthMask);
                break;
            case GL_STENCIL_REF:
                // TODO: params[0] = MG_State_T::commonState->stencilRef;
                break;

            // Capability enables
            case GL_BLEND:
                params[0] = MG_State_T::commonState->capabilities[GL_BLEND] ? GL_TRUE : GL_FALSE;
                break;
            case GL_DEPTH_TEST:
                params[0] = MG_State_T::commonState->capabilities[GL_DEPTH_TEST] ? GL_TRUE : GL_FALSE;
                break;
            case GL_SCISSOR_TEST:
                params[0] = MG_State_T::commonState->capabilities[GL_SCISSOR_TEST] ? GL_TRUE : GL_FALSE;
                break;

            // Pixel operations
            case GL_PACK_ALIGNMENT:
                params[0] = MG_State_T::commonState->QueryPixelStoreInt(GL_PACK_ALIGNMENT);
                break;
            case GL_UNPACK_ALIGNMENT:
                params[0] = MG_State_T::commonState->QueryPixelStoreInt(GL_UNPACK_ALIGNMENT);
                break;

            // Hardware limits, TODO: Check the real values
            case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
                params[0] = 32;
                break;
            case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
                params[0] = 32;
                break;
            case GL_MAX_TEXTURE_IMAGE_UNITS:
                params[0] = 32;
                break;
            case GL_MAX_RENDERBUFFER_SIZE:
                params[0] = 65537;
                break;
            case GL_MAX_DRAW_BUFFERS:
                params[0] = 32;
                break;
            case GL_MAX_COLOR_ATTACHMENTS:
                params[0] = 32;
                break;

            // Color state
            case GL_COLOR_CLEAR_VALUE: {
                const auto& c = MG_State_T::commonState->clearColor;
                params[0] = static_cast<GLint>(c[0] * 0x7FFF);
                params[1] = static_cast<GLint>(c[1] * 0x7FFF);
                params[2] = static_cast<GLint>(c[2] * 0x7FFF);
                params[3] = static_cast<GLint>(c[3] * 0x7FFF);
                break;
            }
            case GL_COLOR_WRITEMASK:
                params[0] = MG_State_T::commonState->colorMask[0];
                params[1] = MG_State_T::commonState->colorMask[1];
                params[2] = MG_State_T::commonState->colorMask[2];
                params[3] = MG_State_T::commonState->colorMask[3];
                break;

            // Stencil operations
            case GL_STENCIL_CLEAR_VALUE:
                // TODO: params[0] = MG_State_T::commonState->stencilClearValue;
                break;
            case GL_STENCIL_BITS:
                // TODO: Check the real value
                params[0] = 8;
                break;

           // Implementation limits
            case GL_SUBPIXEL_BITS:
                // TODO: Check the real value
                params[0] = 4;
                break;
            case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
                // TODO: params[0] = static_cast<GLint>(MG_State_T::textureState->compressedFormats_.size());
                break;
            case GL_COMPRESSED_TEXTURE_FORMATS:
                // TODO: Implement GL_COMPRESSED_TEXTURE_FORMATS
                break;

           // Polygon state
            case GL_POLYGON_OFFSET_FACTOR:
                // TODO: params[0] = static_cast<GLint>(MG_State_T::commonState->polygonOffsetFactor);
                break;
            case GL_POLYGON_OFFSET_UNITS:
                // TODO: params[0] = static_cast<GLint>(MG_State_T::commonState->polygonOffsetUnits);
                break;

            // Others...
            default:
                MG_Util::Debug::LogE("glGetIntegerv: Invalid enum %s (0x%X)", MG_Util::Debug::GLEnumToString(pname), pname);
                MG_State::SetError(GL_INVALID_ENUM);
                break;
        }
    }

}