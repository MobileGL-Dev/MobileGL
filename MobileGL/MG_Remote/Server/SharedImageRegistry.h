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

#include "SharedImageYuv.h"

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
    // YUV 4:2:0, two planes: Y, then Cb Cr interleaved at half resolution (8-bit samples; P010 the
    // same with 10 significant bits in the high end of 16). A YUV image is never rendered to: it is
    // SAMPLED, converted into RGBA by whoever binds it (SharedImageYuv.h).
    inline constexpr Uint32 kFourccNv12 = FourCC('N', 'V', '1', '2');
    inline constexpr Uint32 kFourccP010 = FourCC('P', '0', '1', '0');
    // "The layout is the allocator's": what an AHardwareBuffer is to anybody but its allocator.
    inline constexpr Uint64 kModifierInvalid = 0x00ffffffffffffffull;
    inline constexpr Uint64 kModifierLinear = 0;

    // Whether images of this fourcc can be allocated (and so imported) at all.
    Bool FourccSupported(Uint32 fourcc);
    // The X formats: alpha reads as 1 whatever is stored.
    Bool FourccIgnoresAlpha(Uint32 fourcc);
    // NV12 / P010.
    Bool FourccIsYuv(Uint32 fourcc);
    // Bytes of one sample of a YUV fourcc (1, or 2 for P010); 0 for anything else.
    Uint32 FourccYuvSampleBytes(Uint32 fourcc);
    // Whether this server can allocate images of a YUV fourcc (the platform's allocator takes the
    // format for sampling). Asked once per format, cached.
    Bool YuvFourccAllocatable(Uint32 fourcc);

    // Where an imported dma-buf's planes are: byte offset and row pitch of each.
    struct PlaneLayout {
        Uint32 Count = 1;
        Uint32 Offset[2] = {0, 0};
        Uint32 Pitch[2] = {0, 0};
    };

    struct Image {
        Uint64 Id = 0;
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Fourcc = 0;
        Uint32 Stride = 0; // bytes
        Uint32 Offset = 0;
        Uint64 Modifier = kModifierInvalid;
        // A YUV image's second plane (Cb Cr): same descriptor, this offset and pitch. The layout is
        // still the allocator's - these are what it reported when the buffer was locked, so a
        // client can name the plane, not a promise that the descriptor maps that way.
        Uint32 Plane1Stride = 0;
        Uint32 Plane1Offset = 0;
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

        // FOREIGN YUV - THE CPU-COPY FALLBACK. A YUV dma-buf this server did not allocate (a video
        // decoder's frame) cannot be imported vendor-neutrally: neither the platform's EGL nor its
        // Vulkan takes a raw dma-buf. Such an import becomes a YUV image of this server's (Native,
        // an AHardwareBuffer the CPU can write) whose content is COPIED from a CPU mapping of the
        // foreign buffer (RefreshForeign) at a reader's first use of it in each of its frames - the
        // moments the producer's content can have moved for that reader. Every other image is
        // zero-copy; this one costs a frame's worth of memcpy per refresh, and its log says so.
        struct ForeignSource {
            int Fd = -1; // a duplicate, owned
            PlaneLayout Layout;
            void* Map = nullptr;
            SizeT MapSize = 0;
            std::mutex Mutex; // one refresh at a time
            Uint64 Copies = 0;
            // The foreign buffer's identity, under which a later import of it finds this image.
            Uint64 IdentityDev = 0;
            Uint64 IdentityIno = 0;
        };
        UniquePtr<ForeignSource> Foreign;
        Bool IsForeign() const { return Foreign != nullptr; }

        // How the samples of a YUV image convert to RGB: the EGL_EXT_image_dma_buf_import hints
        // of its last import. Guarded by SyncMutex.
        mutable Yuv::Hints YuvHints;

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

    // A YUV import of a dma-buf this server did not export (Image::ForeignSource): the live
    // foreign image already made from the same buffer with the same shape, or a new one, which
    // keeps a duplicate of `fd`. Null with `why` when the buffer cannot be mapped, its planes do
    // not fit in it, or no YUV image can be allocated to copy it into.
    ImageRef ImportForeignYuv(int fd, Uint32 width, Uint32 height, Uint32 fourcc, const PlaneLayout& layout,
                              std::string& why);

    // A foreign image's content brought up to date from its source (the copy). Waits for the
    // image's pending reads first, so a frame still sampling the old content keeps it. True for an
    // image that is not foreign: there is nothing to do.
    Bool RefreshForeign(const Image& image, std::string& why);

    // The colour hints a YUV image is converted with.
    void SetYuvHints(const Image& image, const Yuv::Hints& hints);
    Yuv::Hints GetYuvHints(const Image& image);

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
        // IMPLICIT SYNC BY FLUSH: a session that renders into images as well as sampling them, with
        // no swap and no explicit write (an X server's glamor) ends its frame at glFlush. Every image
        // it used since its last boundary gets `fence` both as a write - the generation moves, so the
        // next reader waits for it - and as a read, so the next writer waits for it too. Takes
        // ownership of `fence` (-1: the work has completed; the generation still moves).
        void PublishFrameAsWrite(int fence);
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
