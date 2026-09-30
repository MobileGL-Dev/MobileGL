# glvnd vendor ABI headers

'libeglabi.h' and 'GLdispatchABI.h' from libglvnd (MIT), copied verbatim with their own licence
headers intact.

They are here because MobileGL implements the *vendor* half of glvnd's EGL ABI - the side a library
exposes so the system's 'libEGL.so.1', which IS glvnd, can dispatch to it.  The distribution ships
the runtime but not these headers in the base image, and a vendored copy of two MIT headers is a
smaller dependency than a build-time download of a source tree.

Upstream ABI version implemented: 'EGL_VENDOR_ABI_MAJOR_VERSION' 0 (the header pins it).
