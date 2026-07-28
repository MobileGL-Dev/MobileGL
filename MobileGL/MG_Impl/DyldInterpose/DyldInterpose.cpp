// MobileGL - MobileGL/MG_Impl/DyldInterpose/DyldInterpose.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>

#if defined(__APPLE__)

#include "MG_Impl/CGLImpl/CGLImpl.h"
#include "MG_Impl/GetProcAddress.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CVDisplayLink.h>
#include <cstdint>
#include <dlfcn.h>

namespace {
    struct DyldInterposeEntry {
        const void* Replacement;
        const void* Replacee;
    };

    bool IsGLProcName(const char* name) {
        if (name == nullptr) {
            return false;
        }

        if (strncmp(name, "CGL", 3) == 0) {
            return true;
        }

        if (strncmp(name, "gl", 2) != 0) {
            return false;
        }

        // Avoid stealing glfw*/glib*/glX*/global application symbols.
        return name[2] >= 'A' && name[2] <= 'Z' && name[2] != 'X';
    }

    void* MobileGLDlsym(void* handle, const char* symbol) {
        if (IsGLProcName(symbol)) {
            if (void* proc = MobileGL::MG_Impl::GetProcAddress(symbol)) {
                return proc;
            }
        }

        return dlsym(handle, symbol);
    }

    CGDirectDisplayID DisplayForMask(GLint displayMask) {
        constexpr std::uint32_t MaxDisplays = sizeof(CGOpenGLDisplayMask) * 8;
        CGDirectDisplayID displays[MaxDisplays] = {};
        std::uint32_t displayCount = 0;
        if (displayMask != 0 &&
            CGGetActiveDisplayList(MaxDisplays, displays, &displayCount) == kCGErrorSuccess) {
            const auto mask = static_cast<CGOpenGLDisplayMask>(displayMask);
            for (std::uint32_t i = 0; i < displayCount; ++i) {
                if ((CGDisplayIDToOpenGLDisplayMask(displays[i]) & mask) != 0) {
                    return displays[i];
                }
            }
        }
        return CGMainDisplayID();
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVReturn MobileGLCVDisplayLinkSetCurrentCGDisplayFromOpenGLContext(
        CVDisplayLinkRef displayLink,
        CGLContextObj context,
        CGLPixelFormatObj pixelFormat) {
        GLint virtualScreen = 0;
        if (MobileGL::MG_Impl::CGLImpl::GetVirtualScreen(context, &virtualScreen) == kCGLNoError) {
            GLint displayMask = 0;
            if (!displayLink ||
                MobileGL::MG_Impl::CGLImpl::DescribePixelFormat(
                    pixelFormat, virtualScreen, kCGLPFADisplayMask, &displayMask) != kCGLNoError) {
                return kCVReturnInvalidArgument;
            }
            return CVDisplayLinkSetCurrentCGDisplay(displayLink, DisplayForMask(displayMask));
        }

        using OriginalFunction = CVReturn (*)(CVDisplayLinkRef, CGLContextObj, CGLPixelFormatObj);
        static const auto original = reinterpret_cast<OriginalFunction>(
            dlsym(RTLD_NEXT, "CVDisplayLinkSetCurrentCGDisplayFromOpenGLContext"));
        return original ? original(displayLink, context, pixelFormat) : kCVReturnError;
    }

    __attribute__((used)) static const DyldInterposeEntry kMobileGLDyldInterpose[]
        __attribute__((section("__DATA,__interpose"))) = {
            {reinterpret_cast<const void*>(MobileGLDlsym), reinterpret_cast<const void*>(dlsym)},
            {reinterpret_cast<const void*>(MobileGLCVDisplayLinkSetCurrentCGDisplayFromOpenGLContext),
             reinterpret_cast<const void*>(CVDisplayLinkSetCurrentCGDisplayFromOpenGLContext)},
        };
#pragma clang diagnostic pop
} // namespace

#endif
