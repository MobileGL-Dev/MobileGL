// MobileGL - MobileGL/MG_Remote/Client/DmaBufImport.h
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// TELLING THE RENDER SERVER WHICH TEXTURE A dma-buf BACKS.
//
// The compositor's scene reaches the display daemon's buffers through glEGLImageTargetTexture2DOES:
// eglCreateImageKHR already holds a dup() of every plane's descriptor, and a texture bound to that
// image has NO storage on the server - so a framebuffer built over it is not complete and kwin aborts
// on its own RenderTarget assertion (measured, twice: "taking target 0x8d65 over an EGLImage of
// 1440x3200 fourcc 0x34324241 ... the render server has NOT been told which texture that dma-buf
// backs yet").  This is the client's half of closing that: one message per plane, each carrying one
// descriptor in the SCM_RIGHTS control data of the session transport's aux socket (ITransport::ShareFd,
// FdPassing.h - "a dedicated AF_UNIX SOCK_DGRAM socketpair, so one sendmsg is exactly one recvmsg").
//
// The byte layout is restated from the server's reader rather than shared with it, for the same reason
// the GBM frame shapes are (MG_Backend/GbmFrameChannel.h): the two ends are built into different
// libraries from different build configurations. A change on either side is a protocol change, and the
// magic is what makes a mismatched pair fail loudly instead of being read as plausible numbers.

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Remote::Client {

    inline constexpr Uint32 kDmaBufImportRequestMagic = 0x4947474Du; // 'M','G','G','I' in memory order
    inline constexpr Uint32 kDmaBufImportVersion = 1u;
    inline constexpr Uint32 kDmaBufImportMaxPlanes = 4u;

    struct DmaBufImportRequest {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Target;      // the GL texture target the image was bound to (0x8D65 = GL_TEXTURE_EXTERNAL_OES)
        Uint32 TexName;     // THIS side's texture name; the server resolves its twin
        Uint32 Width;
        Uint32 Height;
        Uint32 FourCC;
        Uint32 PlaneCount;
        Int32 Stride[kDmaBufImportMaxPlanes];
        Int32 Offset[kDmaBufImportMaxPlanes];
        Uint64 Modifier[kDmaBufImportMaxPlanes];
        Uint32 HasModifier[kDmaBufImportMaxPlanes];
    } __attribute__((packed));

    // Shares one plane's descriptor with the server, describing the buffer in the same fields
    // drmModeAddFB2 takes.  Answers false when the session has no descriptor channel (auxFd == -1:
    // SocketTransport answers MOBILEGL_ERR_UNSUPPORTED) or when the send failed - the caller keeps
    // treating the texture as having no storage, which is the honest state, rather than rendering
    // into nothing.
    Bool ShareImportedDmaBuf(Uint32 target, Uint32 texName, Uint32 width, Uint32 height, Uint32 fourCC,
                             Uint32 planeIndex, Uint32 planeCount, Int planeFd, Int stride, Int offset,
                             Uint64 modifier, Bool hasModifier);
} // namespace MobileGL::MG_Remote::Client
