// MobileGL - MobileGL/Includes.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// Include significant project headers
#include "Defines.h"
#include "MG_Util/PlatformStubs.h"

// Include necessary standard headers
#include <bit>
#include <map>
#include <array>
#include <ctime>
#include <mutex>
#include <queue>
#include <regex>
#include <atomic>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <numeric>
#include <expected>
#include <iostream>
#include <optional>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <string_view>
#include <unordered_map>
#if __cplusplus >= 202302L && MOBILEGL_LOG_ENABLE_STACKTRACE
#include <stacktrace>
#endif

// Include ska::flat_hash_map
#include <ska/flat_hash_map.hpp>

// Include xxHash
#include <xxhash.h>

// Include spirv_cross
#include <spirv_cross/spirv_cross_c.h>

// Include OpenGL and EGL headers
#ifdef GLAPI
#undef GLAPI
#endif
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#ifndef NO_GL_H
#include "GL/gl.h"
#endif
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

#include <GL/glext.h>

#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl32.h>
#undef GL_GLES_PROTOTYPES

// Include glslang headers
#include <glslang/Include/Types.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Include/intermediate.h>
#include <glslang/MachineIndependent/localintermediate.h>

// Include headers for platform-specific functionality
#ifdef __linux__
#include <dlfcn.h>
#include <pthread.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <processthreadsapi.h>
#endif

#ifdef __ANDROID__
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <android/native_window.h>
#endif

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#elif _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#elif defined(__linux__)
#define VK_USE_PLATFORM_XLIB_KHR
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;
typedef unsigned long VisualID;
#else
#warning "VK_USE_PLATFORM_*_KHR not defined for this platform!"
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
#pragma push_macro("Bool")
#pragma push_macro("None")
#pragma push_macro("Always")
#pragma push_macro("Status")
#pragma push_macro("LSBFirst")
#pragma push_macro("DestroyAll")
#endif
#include <vulkan/vulkan.h>
#if defined(VK_USE_PLATFORM_XLIB_KHR)
#pragma pop_macro("DestroyAll")
#pragma pop_macro("LSBFirst")
#pragma pop_macro("Status")
#pragma pop_macro("Always")
#pragma pop_macro("None")
#pragma pop_macro("Bool")
#endif

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#define TRACY_ZONECOLOR_ENTRY 0xFF0000
#define TRACY_ZONECOLOR_FRONTEND 0x00FF00
#define TRACY_ZONECOLOR_BACKEND 0x00FF00
#endif

// Post-includes for significant project headers
#include "MG_Util/Debug/Log.h"
#include "MG_Util/Types.h"
