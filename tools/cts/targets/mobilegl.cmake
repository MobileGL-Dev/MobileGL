#-------------------------------------------------------------------------
# VK-GL-CTS target: MobileGL on Android
#
# Builds a standalone arm64 ELF that reaches OpenGL exclusively through
# libMobileGL.so, loaded at runtime. Nothing here links libEGL or libGLESv*:
# the whole point is that the system GL stack must not be reachable, so that a
# conformance result is unambiguously MobileGL's.
#-------------------------------------------------------------------------

message("*** Using MobileGL target")

set(DEQP_TARGET_NAME "MobileGL")

# Build the modules as standalone executables instead of the libdeqp.so an APK
# would load. The suite runs from adb shell, with no Activity.
set(DEQP_ANDROID_EXE ON)

# EGL comes from libMobileGL.so via the eglw dynamic wrapper, so the support
# flag is on but no import library is supplied.
set(DEQP_SUPPORT_EGL ON)
set(DEQP_EGL_LIBRARIES)
set(DEQP_GLES2_LIBRARIES)
set(DEQP_GLES3_LIBRARIES)

set(TCUTIL_PLATFORM_SRCS
	mobilegl/tcuMobileGLPlatform.cpp
	mobilegl/tcuMobileGLPlatform.hpp
	)

find_library(LOG_LIBRARY NAMES log)
find_library(ANDROID_LIBRARY NAMES android)
find_library(MEDIANDK_LIBRARY NAMES mediandk)

# libmediandk supplies AImageReader, which is how a process with no Activity
# gets a real ANativeWindow.
list(APPEND TCUTIL_PLATFORM_LIBS ${ANDROID_LIBRARY} ${MEDIANDK_LIBRARY} ${LOG_LIBRARY})
