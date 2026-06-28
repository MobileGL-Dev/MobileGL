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
