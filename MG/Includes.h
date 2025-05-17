//
// Created by BZLZHH on 2025/3/15.
//

#include "Constants.h"
#define BACKEND_TYPE BACKEND_GLES
#include "Global.h"
#ifndef MOBILEGL_GLES_LOADER_H
#include "MG_GL/BackendLoader/GLES/Loader.h"
#endif
#ifndef MOBILEGL_GL_FRAMEBUFFER_H
#include "MG_GL/Implementations/GL/Framebuffer/GL_Framebuffer.h"
#endif
#ifndef MOBILEGL_GL_GETTER_H
#include "MG_GL/Implementations/GL/Getter/GL_Getter.h"
#endif
#ifndef MOBILEGL_BACKENDINFO_H
#include "MG_GL/Implementations/GL/Getter/BackendInfo.h"
#endif
#ifndef MOBILEGL_GL_BUFFER_H
#include "MG_GL/Implementations/GL/Buffer/GL_Buffer.h"
#endif
#ifndef MOBILEGL_GL_DRAWING_H
#include "MG_GL/Implementations/GL/Drawing/GL_Drawing.h"
#endif
#ifndef MOBILEGL_GL_PROGRAM_H
#include "MG_GL/Implementations/GL/Program/GL_Program.h"
#endif
#ifndef MOBILEGL_GL_TEXTURE_H
#include "MG_GL/Implementations/GL/Texture/GL_Texture.h"
#endif
#ifndef MOBILEGL_GL_VERTEXARRAY_H
#include "MG_GL/Implementations/GL/VertexArray/GL_VertexArray.h"
#endif
#ifndef MOBILEGL_GLSTATE_H
#include "MG_GL/State/Core/GLState.h"
#endif
#ifndef MOBILEGL_COMMONSTATE_H
#include "MG_GL/State/Common/CommonState.h"
#endif
#ifndef MOBILEGL_TEXTURESTATE_H
#include "MG_GL/State/Texture/TextureState.h"
#endif
#ifndef MOBILEGL_VERTEXARRAYSTATE_H
#include "MG_GL/State/VertexArray/VertexArrayState.h"
#endif
#ifndef MOBILEGL_BUFFERSTATE_H
#include "MG_GL/State/Buffer/BufferState.h"
#endif
#ifndef MOBILEGL_SHADEROBJECT_H
#include "MG_GL/State/Program/ShaderObject.h"
#endif
#ifndef MOBILEGL_PROGRAMSTATE_H
#include "MG_GL/State/Program/ProgramState.h"
#endif
#ifndef MOBILEGL_FRAMEBUFFERSTATE_H
#include "MG_GL/State/Framebuffer/FramebufferState.h"
#endif
#ifndef MOBILEGL_LOOKUP_H
#include "MG_GL/Implementations/GLX/LookUp/LookUp.h"
#endif
#ifndef MOBILEGL_DEBUG_H
#include "MG_UTIL/Debug/Debug.h"
#endif
#ifndef MOBILEGL_GLSLTOOL_H
#include "MG_UTIL/Program/GLSLTool.h"
#endif
#ifndef MOBILEGL_PROGRAM_DEBUGTOOL_H
#include "MG_UTIL/Program/DebugTool.h"
#endif
#ifndef MOBILEGL_GL_COMMON_H
#include "MG_GL/Implementations/GL/Common/GL_Common.h"
#endif
#ifndef MOBILEGL_EGL_VK_EMU_H
#include "MG_GL/Implementations/EGL/VK/EGL_EMU.h"
#endif
#ifndef MOBILEGL_EGL_GLES_EGL_WRAP_H
#include "MG_GL/Implementations/EGL/GLES/EGL_WRAP.h"
#endif
#ifndef MOBILEGL_TESTDRAWING_H
#include "MG_RHI/GLES/Test/TestDrawing.h"
#endif

#include <cstring>
#include <iostream>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <atomic>
#include <regex>
#include <strstream>
#include <algorithm>
#include <array>
#include <random>
#include <optional>
#include <unordered_map>
#include <queue>
#include <format>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/Types.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_cross/spirv_cross_c.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <ankerl/unordered_dense.h>
#include <GLES3/gl32.h>

#include "MG_Include/UncertainBool.hpp"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#elif _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <pthread.h>
#endif

#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
