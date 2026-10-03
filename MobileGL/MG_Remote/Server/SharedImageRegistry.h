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

        // SYNCHRONISATION, as sync_file descriptors (-1 = nothing pending). An image has one
        // writer at a time (the client session presenting into it) and any number of readers (a
        // compositor session sampling it). Guarded by SyncMutex; reached only through the
        // functions below, which is why these are mutable on an otherwise immutable record.
        mutable std::mutex SyncMutex;
        mutable int WriteFence = -1;
        mutable Uint64 WriteGeneration = 0;
        mutable Vector<int> ReadFences;

        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        ~Image();
    };
    using ImageRef = SharedPtr<const Image>;

    // A new image of `width` x `height` `fourcc`, registered. Null with `why` on failure.
    ImageRef Allocate(Uint32 width, Uint32 height, Uint32 fourcc, std::string& why);

    // The live image the descriptor was exported from, or null (why says which test failed): a
    // descriptor this server never exported, or one no live image matches.
    //
    // WHICH IDENTITY A DESCRIPTOR CARRIES IS THE KERNEL'S TO SAY. (st_dev, st_ino) is the
    // honest answer, and the only one on kernels that give every dma-buf its own inode. Some
    // do not: there, every descriptor reports one identity (measured on a 4.14 vendor kernel:
    // dev 12, ino 10658, st_size 0, and /proc/self/fdinfo carries nothing else either), so the
    // descriptor cannot name its image. The import then names it by what it DOES carry - the
    // extent and the format it declares - and the images the server exported under that
    // identity are taken in allocation order, each claimable once, which is the order the
    // producer hands them over. A kernel whose descriptors are ambiguous degrades to that,
    // and says so once.
    ImageRef Identify(int fd, Uint32 width, Uint32 height, Uint32 fourcc, std::string& why);

    // The live image with this id, or null.
    ImageRef Find(Uint64 id);

    // Live images, for tests and diagnostics.
    SizeT LiveCount();

    // ---- synchronisation (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md) ---------------
    //
    // THE SERVER ORDERS THE IMAGE BETWEEN ITS SESSIONS, the way a kernel's implicit sync would:
    // a writer publishes the fence of its write and moves on; a reader that finds the generation
    // moved waits for that fence on its GPU before it samples; at the end of each frame a reader
    // publishes that frame's fence, and the next writer waits for it before it overwrites what
    // is still being read. Every fence is a sync_file, so no two backends need to agree on more.

    // The writer's fence: the copy just recorded into `image`. Takes ownership of `fence` (-1:
    // the write has already completed). Moves the write generation.
    void PublishWrite(const Image& image, int fence);
    // The current write generation and a duplicate of its still-pending fence (-1 when there is
    // none or it has signaled). The caller closes the descriptor.
    int DupWriteFence(const Image& image, Uint64* generation);
    // A reader's fence: a frame that sampled `image`. Takes ownership of `fence`.
    void PublishRead(const Image& image, int fence);
    // Duplicates of the read fences still pending; signaled ones are dropped on the way. The
    // caller closes each.
    Vector<int> DupPendingReadFences(const Image& image);
    // CPU waits, for a backend that cannot wait on its GPU. False on timeout.
    Bool WaitForReads(const Image& image, Uint32 timeoutMs);

    namespace SyncFile {
        // Whether `fd` (a sync_file) has signaled; an invalid descriptor counts as signaled.
        Bool Signaled(int fd);
        // Waits for it. False on timeout.
        Bool Wait(int fd, Uint32 timeoutMs);
    } // namespace SyncFile

    // One reader's frame: the images it sampled since its last frame boundary. A compositor
    // session notes each image as it binds it and, at its present, publishes the frame's fence
    // to all of them at once.
    class ReadTracker {
    public:
        void NoteRead(const ImageRef& image);
        // Takes ownership of `fence` (a sync_file for the frame, -1 if it has completed).
        void PublishFrame(int fence);
        SizeT Pending() const { return m_images.size(); }

    private:
        UnorderedMap<Uint64, ImageRef> m_images;
    };

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
