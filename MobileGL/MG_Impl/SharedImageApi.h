/* MobileGL - MobileGL/MG_Impl/SharedImageApi.h
 * Copyright (c) 2025-2026 MobileGL-Dev
 * Licensed under the GNU Lesser General Public License v3.0:
 *   https://www.gnu.org/licenses/gpl-3.0.txt
 *   https://www.gnu.org/licenses/lgpl-3.0.txt
 * SPDX-License-Identifier: LGPL-3.0-only
 * End of Source File Header */

/* SHARED IMAGES, AS A C ABI (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md).
 *
 * libMobileGL.so exports these on Linux (not Android) in split builds, so a component that is not
 * a GL entry point - the GBM backend (MG_Gbm), above all - can ask the server for an image and
 * name one it was handed, through the process's own MobileGL client session. The component
 * resolves them with dlsym; nothing links against them.
 *
 * Images are server-allocated colour buffers exported as dma-buf descriptors. Their memory layout
 * is the allocator's: the modifier reported is DRM_FORMAT_MOD_INVALID, and the only meaningful
 * thing to do with a descriptor is to hand it back (an import, here or through EGL) - not to map
 * it and assume rows.
 *
 * Errors are negative errno values:
 *   -EINVAL   a bad argument or an unsupported fourcc
 *   -ENODEV   no MobileGL client session (release only; the others bring one up)
 *   -EIO      the session refused or the server declined (its log says why)
 *
 * THREADS. The calls are serialized against each other, and each one is a round trip on the
 * process's client session - the same ring its GL calls ride - so like any GL call they must not
 * race the GL calls of another thread. */

#ifndef MOBILEGL_SHARED_IMAGE_API_H
#define MOBILEGL_SHARED_IMAGE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOBILEGL_SHARED_IMAGE_ABI_VERSION 1u

/* DRM fourccs (little-endian packed). ABGR8888 is R, G, B, A in memory - GL's RGBA8. */
#define MOBILEGL_SHARED_IMAGE_FOURCC_ABGR8888 0x34324241u /* 'AB24' */
#define MOBILEGL_SHARED_IMAGE_FOURCC_XBGR8888 0x34324258u /* 'XB24' */
/* ARGB8888 / XRGB8888: allocatable too. The layout of every shared image is the server's own
 * (modifier INVALID, never mapped), and channels are addressed logically by the GL that reads and
 * writes them, so the name an image is allocated under never changes its colours. */
#define MOBILEGL_SHARED_IMAGE_FOURCC_ARGB8888 0x34325241u /* 'AR24' */
#define MOBILEGL_SHARED_IMAGE_FOURCC_XRGB8888 0x34325258u /* 'XR24' */

/* One allocated image. VERSIONED BY SIZE: the caller sets struct_size to the sizeof it was built
 * against and the library writes no byte past it, so fields are only ever appended. */
struct mobilegl_shared_image {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint64_t id;       /* the server's name for the image; pass to release */
    int32_t fd;        /* the exported dma-buf, owned by the caller; -1 on failure */
    uint32_t stride;   /* plane 0, bytes */
    uint32_t offset;   /* plane 0, bytes */
    uint32_t reserved; /* zero */
    uint64_t modifier; /* DRM format modifier of the layout (DRM_FORMAT_MOD_INVALID today) */
};

/* MOBILEGL_SHARED_IMAGE_ABI_VERSION of the library. */
uint32_t mobilegl_shared_image_abi_version(void);

/* The fourccs images can be allocated in. Writes min(capacity, total) entries and returns the
 * total (formats may be null when capacity is 0). */
int mobilegl_shared_image_formats(uint32_t* formats, uint32_t capacity);

/* A new width x height image. Brings the process's MobileGL client session up if no EGL/GLX entry
 * point has yet (exactly as the first eglGetDisplay would). */
int mobilegl_shared_image_allocate(uint32_t width, uint32_t height, uint32_t fourcc,
                                   struct mobilegl_shared_image* out);

/* Names the image `fd` was exported from (the caller keeps `fd`), taking a reference to it.
 * -EIO: not one of the server's images, or the size/format disagree. */
int mobilegl_shared_image_import(int fd, uint32_t width, uint32_t height, uint32_t fourcc, uint64_t* out_id);

/* Drops this process's reference (an allocation's or an import's). */
int mobilegl_shared_image_release(uint64_t id);

/* 1 when this process can be served: its session is up, or the server it is configured to dial
 * answers (one bounded non-blocking connect, cached once true); 0 when not. Never brings the
 * session up. A component that a system loader picked on MobileGL's behalf (the GBM backend) asks
 * this first and declines, so the loader moves on to another implementation. Optional: a library
 * without it is assumed available. */
int mobilegl_server_available(void);

/* dlsym-friendly signatures. */
typedef int (*PFN_mobilegl_server_available)(void);
typedef uint32_t (*PFN_mobilegl_shared_image_abi_version)(void);
typedef int (*PFN_mobilegl_shared_image_formats)(uint32_t*, uint32_t);
typedef int (*PFN_mobilegl_shared_image_allocate)(uint32_t, uint32_t, uint32_t, struct mobilegl_shared_image*);
typedef int (*PFN_mobilegl_shared_image_import)(int, uint32_t, uint32_t, uint32_t, uint64_t*);
typedef int (*PFN_mobilegl_shared_image_release)(uint64_t);

#ifdef __cplusplus
}
#endif

#endif /* MOBILEGL_SHARED_IMAGE_API_H */
