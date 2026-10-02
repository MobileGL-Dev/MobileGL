// MobileGL - MobileGL/MG_Remote/Server/SharedImageRegistry.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SHARED IMAGES: colour buffers the server allocates and hands to clients as dma-buf descriptors,
// so a frame one client renders can be sampled by another (a Wayland window by its compositor)
// without leaving GPU memory (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md).
//
// THE SERVER IS THE ALLOCATOR AND THE ONLY IMPORTER. An image is an AHardwareBuffer; what leaves
// the process is a duplicate of its first native-handle descriptor, which on every Android kernel
// with dma-buf heaps is a dma-buf. A descriptor that comes BACK (a compositor importing a client's
// wl_buffer) is never imported as raw memory - its layout is the allocator's business - it is
// IDENTIFIED: the kernel gives each dma-buf its own inode, so (st_dev, st_ino) names the image it
// was exported from, and the backends bind that AHardwareBuffer through the Android import paths.
// Nothing here reads a vendor's handle layout or assumes a memory layout.
//
// LIFETIME. The registry holds weak references only. Whoever still needs an image holds an
// ImageRef: the session that allocated or imported it (until its release or the session's end)
// and a backend object built on it (an EGLImage, a VkImage) for as long as that object lives.

#pragma once
#include <Includes.h>

#include <mutex>
#include <string>

namespace MobileGL::MG_Remote::Server::SharedImages {

    // DRM fourcc codes (drm_fourcc.h). Little-endian packed: ABGR8888 is R, G, B, A in memory,
    // which is GL's RGBA8 and AHardwareBuffer's R8G8B8A8.
    inline constexpr Uint32 FourCC(char a, char b, char c, char d) {
        return static_cast<Uint32>(static_cast<Uint8>(a)) | (static_cast<Uint32>(static_cast<Uint8>(b)) << 8) |
               (static_cast<Uint32>(static_cast<Uint8>(c)) << 16) | (static_cast<Uint32>(static_cast<Uint8>(d)) << 24);
    }
    inline constexpr Uint32 kFourccAbgr8888 = FourCC('A', 'B', '2', '4');
    inline constexpr Uint32 kFourccXbgr8888 = FourCC('X', 'B', '2', '4');
    // ARGB8888 / XRGB8888 name B, G, R, A in memory. An image of either is still stored R, G, B, A:
    // its layout is the allocator's (the modifier is INVALID and no CPU mapping is offered), every
    // reader and writer is this server, and they all address channels LOGICALLY - so what a
    // client renders as red samples as red, whichever of the four names it allocated under.
    inline constexpr Uint32 kFourccArgb8888 = FourCC('A', 'R', '2', '4');
    inline constexpr Uint32 kFourccXrgb8888 = FourCC('X', 'R', '2', '4');
    // "The layout is the allocator's": what an AHardwareBuffer is to anybody but its allocator.
    inline constexpr Uint64 kModifierInvalid = 0x00ffffffffffffffull;
    inline constexpr Uint64 kModifierLinear = 0;

    // Whether images of this fourcc can be allocated (and so imported) at all.
    Bool FourccSupported(Uint32 fourcc);
    // The X formats: alpha reads as 1 whatever is stored.
    Bool FourccIgnoresAlpha(Uint32 fourcc);

    struct Image {
        Uint64 Id = 0;
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Fourcc = 0;
        Uint32 Stride = 0; // bytes
        Uint32 Offset = 0;
        Uint64 Modifier = kModifierInvalid;
        // AHardwareBuffer* on Android; null on a host, where the memory is a memfd (unit tests).
        void* Native = nullptr;
        // The descriptor clients receive duplicates of. Owned.
        int Fd = -1;
        Uint64 IdentityDev = 0;
        Uint64 IdentityIno = 0;

        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        ~Image();
    };
    using ImageRef = SharedPtr<const Image>;

    // A new image of `width` x `height` `fourcc`, registered. Null with `why` on failure.
    ImageRef Allocate(Uint32 width, Uint32 height, Uint32 fourcc, std::string& why);

    // The live image `fd` was exported from, or null (`why` says which test failed): a descriptor
    // this server never exported, or one whose identity the kernel does not keep unique.
    ImageRef Identify(int fd, std::string& why);

    // The live image with this id, or null.
    ImageRef Find(Uint64 id);

    // Live images, for tests and diagnostics.
    SizeT LiveCount();

    // One session's references: what it allocated or imported and has not released yet. Dropping
    // the holder (the session ending) drops every one of them.
    class SessionHolder {
    public:
        void Hold(const ImageRef& image);
        Bool Release(Uint64 id);
        ImageRef Get(Uint64 id) const;
        void Clear();
        SizeT Count() const;

    private:
        mutable std::mutex m_mutex;
        UnorderedMap<Uint64, ImageRef> m_images;
    };

} // namespace MobileGL::MG_Remote::Server::SharedImages
