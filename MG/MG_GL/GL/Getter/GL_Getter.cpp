//
// Created by BZLZHH on 2025/3/15.
//

#include "GL_Getter.h"

namespace MG_GL::GL {
    static std::string rendererString;
    const GLubyte* GetString(GLenum name) {
        MG_Util::Debug::LogD("glGetString, name: %s", MG_Util::Debug::GLEnumToString(name));
        switch (name) {
            case GL_VENDOR: 
                return (const GLubyte *)"MobileGL-Dev";
            case GL_VERSION: {
                static std::string versionStr;
                if (versionStr.empty()) {
                    versionStr == std::to_string(MG_Global::GL::GLVersionMajor) + "."
                                    +  std::to_string(MG_Global::GL::GLVersionMinor) + "."
                                    +  std::to_string(MG_Global::GL::GLVersionRevision) 
                                    + " MobileGL (" + MG_GL::Getter::GetBackendName() + ") ";

                    versionStr += std::to_string(MG_Global::MG::VersionMajor) + "."
                                    +  std::to_string(MG_Global::MG::VersionMinor) + "."
                                    +  std::to_string(MG_Global::MG::VersionRevision);
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
                return (const GLubyte *) "MobileGL";
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
        typedef struct {
            GLenum name;
            const char** parts;
            GLuint count;
        } StringCache;
        static StringCache caches[] = {
                {GL_EXTENSIONS, nullptr, 0},
                {GL_VENDOR, nullptr, 0},
                {GL_VERSION, nullptr, 0},
                {GL_SHADING_LANGUAGE_VERSION, nullptr, 0}
        };
        static int initialized = 0;
        if (!initialized) {
            for (auto & cache : caches) {
                GLenum target = cache.name;
                const GLubyte* str = nullptr;
                const char* delimiter = " ";

                switch (target) {
                    case GL_VENDOR:
                        str = (const GLubyte*)"MobileGL-Dev";
                        delimiter = ", ";
                        break;
                    case GL_VERSION:
                        str = (const GLubyte*)"3.0.0 MobileGL";
                        delimiter = " .";
                        break;
                    case GL_SHADING_LANGUAGE_VERSION:
                        str = (const GLubyte*)"4.50 MobileGL glslang";
                        break;
                    case GL_EXTENSIONS:
                        str = GetString(GL_EXTENSIONS);
                        break;
                    default:
                        return (const GLubyte *) "Unknown enum";
                }

                if (!str) continue;

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
        MG_Util::Debug::LogD("glGetError");
        return GL_NO_ERROR;
    }

    void GetIntegerv(GLenum pname, GLint *params) {
        MG_Util::Debug::LogD("glGetIntegerv, pname: %s", MG_Util::Debug::GLEnumToString(pname));
        switch (pname) {
            case GL_CONTEXT_PROFILE_MASK:
                (*params) = GL_CONTEXT_CORE_PROFILE_BIT;
                break;
            case GL_NUM_EXTENSIONS:
                static GLint num_extensions = -1;
                if (num_extensions == -1) {
                    const GLubyte* ext_str = glGetString(GL_EXTENSIONS);
                    if (ext_str) {
                        char* copy = strdup((const char*)ext_str);
                        char* token = strtok(copy, " ");
                        num_extensions = 0;
                        while (token) {
                            num_extensions++;
                            token = strtok(nullptr, " ");
                        }
                        free(copy);
                    } else {
                        num_extensions = 0;
                    }
                }
                (*params) = num_extensions;
                break;
            case GL_MAJOR_VERSION:
                (*params) = MG_Global::GL::GLVersionMajor;
                break;
            case GL_MINOR_VERSION:
                (*params) = MG_Global::GL::GLVersionMinor;
                break;
            default:
                break;
        }
    }
}