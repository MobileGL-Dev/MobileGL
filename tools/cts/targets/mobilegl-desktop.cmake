# MobileGL - tools/cts/targets/mobilegl-desktop.cmake
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

#-------------------------------------------------------------------------
# VK-GL-CTS target: MobileGL on desktop Linux
#
# Builds glcts as a normal host executable that reaches OpenGL exclusively
# through libMobileGL.so, loaded at runtime via the eglw dynamic wrapper.
# Nothing here links libEGL or libGL: a conformance result must be
# unambiguously MobileGL's, never the system GL stack's.
#
# The platform port only offers pbuffer surfaces on desktop; DirectVulkan's
# pbuffer path works because desktop Vulkan exposes VK_EXT_headless_surface.
#-------------------------------------------------------------------------

message("*** Using MobileGL desktop target")

set(DEQP_TARGET_NAME "MobileGL")

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

list(APPEND TCUTIL_PLATFORM_LIBS dl pthread)
