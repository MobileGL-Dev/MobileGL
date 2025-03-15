//
// Created by BZLZHH on 2025/3/15.
//

#include "Constants.h"
#ifndef MOBILEGL_GLOBAL_H
#include "Global.h"
#endif
#ifndef MOBILEGL_GL_FRAMEBUFFER_H
#include "MG_GL/GL/Framebuffer/GL_Framebuffer.h"
#endif
#ifndef MOBILEGL_GL_GETTER_H
#include "MG_GL/GL/Getter/GL_Getter.h"
#endif
#ifndef MOBILEGL_BACKENDINFO_H
#include "MG_GL/GL/Getter/BackendInfo.h"
#endif
#ifndef MOBILEGL_LOOKUP_H
#include "MG_GL/GLX/LookUp/LookUp.h"
#endif
#ifndef MOBILEGL_DEBUG_H
#include "MG_UTIL/Debug/Debug.h"
#endif
#ifndef MOBILEGL_MESAEMU_H
#include "MG_GL/TEMP_MESA/MesaEmu.h"
#endif

#include <cstring>
#include <iostream>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <vulkan/vulkan.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#elif _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <pthread.h>
#endif

#include <GL/gl.h>
#include <GL/glext.h>
