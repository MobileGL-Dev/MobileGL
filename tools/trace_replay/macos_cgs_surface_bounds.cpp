// MobileGL - tools/trace_replay/macos_cgs_surface_bounds.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Carbon/Carbon.h>

using CGSConnectionID = void*;
using CGSSurfaceID = int;

extern "C" bool mobilegl_trace_get_drawable_bounds(int* width, int* height);

extern "C" OSStatus CGSGetSurfaceBounds(CGSConnectionID, CGWindowID, CGSSurfaceID, CGRect* rect) {
    if (rect == nullptr) {
        return -1;
    }

    int width = 0;
    int height = 0;
    if (!mobilegl_trace_get_drawable_bounds(&width, &height) || width <= 0 || height <= 0) {
        return -1;
    }

    rect->origin.x = 0.0;
    rect->origin.y = 0.0;
    rect->size.width = static_cast<CGFloat>(width);
    rect->size.height = static_cast<CGFloat>(height);
    return 0;
}
