// MobileGL - MobileGL/MG_Remote/Server/SharedImageRegistry.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SharedImageRegistry.h"

#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Server::SharedImages {

    namespace {
        std::string Format(const char* fmt, long long a, long long b = 0, long long c = 0) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), fmt, a, b, c);
            return buffer;
        }

        struct Registry {
            std::mutex mutex;
            std::atomic<Uint64> nextId{1};
            UnorderedMap<Uint64, std::weak_ptr<const Image>> byId;
            std::map<std::pair<Uint64, Uint64>, std::weak_ptr<const Image>> byIdentity;
            // Foreign YUV images, by the identity of the buffer they copy from.
            std::map<std::pair<Uint64, Uint64>, std::weak_ptr<const Image>> foreignByIdentity;
            // Set once two live images were seen with one identity: the kernel shares an inode
            // between dma-bufs, so a descriptor can no longer say which image it is.
            Bool identityAmbiguous = false;
        };

        Registry& Reg() {
            static Registry* registry = new Registry(); // outlives every holder at process exit
            return *registry;
        }

#if !defined(_WIN32)
        Bool IdentityOf(int fd, Uint64* dev, Uint64* ino) {
            struct stat st {};
            if (::fstat(fd, &st) != 0) return false;
            *dev = static_cast<Uint64>(st.st_dev);
            *ino = static_cast<Uint64>(st.st_ino);
            return true;
        }
#endif

#if defined(__ANDROID__)
        // Every descriptor of the buffer's native handle, through the public NDK only: the
        // handle is written to a socket pair and read back, and only the SCM_RIGHTS payload is
        // looked at - never the flattened bytes, whose layout is the platform's.
        Bool NativeHandleFds(const AHardwareBuffer* ahb, Vector<int>& fds, std::string& why) {
            int pair[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
                why = Format("socketpair errno=%lld", errno);
                return false;
            }
            const int sent = AHardwareBuffer_sendHandleToUnixSocket(ahb, pair[0]);
            ::close(pair[0]);
            if (sent != 0) {
                why = Format("AHardwareBuffer_sendHandleToUnixSocket rc=%lld errno=%lld", sent, errno);
                ::close(pair[1]);
                return false;
            }
            for (;;) {
                Uint8 data[4096];
                alignas(cmsghdr) Uint8 control[CMSG_SPACE(sizeof(int) * 64)];
                iovec iov{data, sizeof(data)};
                msghdr msg{};
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = control;
                msg.msg_controllen = sizeof(control);
                const ssize_t got = ::recvmsg(pair[1], &msg, MSG_CMSG_CLOEXEC);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) break;
                for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
                    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                    const SizeT n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    const int* in = reinterpret_cast<const int*>(CMSG_DATA(c));
                    for (SizeT i = 0; i < n; ++i) fds.push_back(in[i]);
                }
            }
            ::close(pair[1]);
            if (fds.empty()) {
                why = "the buffer's native handle carries no descriptor";
                return false;
            }
            return true;
        }

        // The two AHardwareBuffer entry points newer than this library's minimum platform (29),
        // resolved at run time. Null on an older platform: YUV images are then not allocatable.
        struct AhbPlaneEntryPoints {
            using LockPlanesFn = int (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, AHardwareBuffer_Planes*);
            using IsSupportedFn = int (*)(const AHardwareBuffer_Desc*);
            LockPlanesFn LockPlanes = nullptr;
            IsSupportedFn IsSupported = nullptr;
        };
        const AhbPlaneEntryPoints& AhbPlanes() {
            static const AhbPlaneEntryPoints entry = [] {
                AhbPlaneEntryPoints e;
                void* lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_NOLOAD);
                if (lib == nullptr) lib = dlopen("libnativewindow.so", RTLD_NOW);
                if (lib != nullptr) {
                    e.LockPlanes =
                        reinterpret_cast<AhbPlaneEntryPoints::LockPlanesFn>(dlsym(lib, "AHardwareBuffer_lockPlanes"));
                    e.IsSupported =
                        reinterpret_cast<AhbPlaneEntryPoints::IsSupportedFn>(dlsym(lib, "AHardwareBuffer_isSupported"));
                }
                return e;
            }();
            return entry;
        }

        // A YUV image's buffer: sampled by the GPU, written by the CPU (a foreign image's copy). The
        // CPU usage also keeps the layout one the platform can lock plane by plane, which is how
        // every layout question about it is answered (AHardwareBuffer_lockPlanes) - never by
        // assuming one.
        constexpr uint64_t kYuvUsage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                       AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

        uint32_t AhbFormatOf(Uint32 fourcc) {
            if (fourcc == kFourccNv12) return AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
            if (fourcc == kFourccP010) return AHARDWAREBUFFER_FORMAT_YCbCr_P010;
            return FourccIgnoresAlpha(fourcc) ? AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
                                              : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        }

        Bool YuvAllocatableNative(Uint32 fourcc) {
            const AhbPlaneEntryPoints& entry = AhbPlanes();
            if (entry.LockPlanes == nullptr || entry.IsSupported == nullptr) return false;
            AHardwareBuffer_Desc desc{};
            desc.width = 64;
            desc.height = 64;
            desc.layers = 1;
            desc.format = AhbFormatOf(fourcc);
            desc.usage = kYuvUsage;
            return entry.IsSupported(&desc) == 1;
        }

        // The planes of a locked YUV buffer as the plane copy addresses them.
        Bool LockYuvPlanes(AHardwareBuffer* ahb, Yuv::PlanarLayout& out, std::string& why) {
            const AhbPlaneEntryPoints& entry = AhbPlanes();
            if (entry.LockPlanes == nullptr) {
                why = "AHardwareBuffer_lockPlanes is not available";
                return false;
            }
            AHardwareBuffer_Planes planes{};
            const int locked = entry.LockPlanes(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &planes);
            if (locked != 0) {
                why = Format("AHardwareBuffer_lockPlanes rc=%lld", locked);
                return false;
            }
            if (planes.planeCount < 3) {
                AHardwareBuffer_unlock(ahb, nullptr);
                why = Format("the locked buffer has %lld planes, not 3", planes.planeCount);
                return false;
            }
            const auto view = [](const AHardwareBuffer_Plane& plane) {
                return Yuv::PlaneView{static_cast<Uint8*>(plane.data), static_cast<Int64>(plane.rowStride),
                                      plane.pixelStride};
            };
            out.Y = view(planes.planes[0]);
            out.Cb = view(planes.planes[1]);
            out.Cr = view(planes.planes[2]);
            return true;
        }

        Bool AllocateNative(Image& image, std::string& why) {
            AHardwareBuffer_Desc desc{};
            desc.width = image.Width;
            desc.height = image.Height;
            desc.layers = 1;
            desc.format = AhbFormatOf(image.Fourcc);
            desc.usage = FourccIsYuv(image.Fourcc)
                             ? kYuvUsage
                             : AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
            AHardwareBuffer* ahb = nullptr;
            const int allocated = AHardwareBuffer_allocate(&desc, &ahb);
            if (allocated != 0 || ahb == nullptr) {
                why = Format("AHardwareBuffer_allocate(%lldx%lld) rc=%lld", image.Width, image.Height, allocated);
                return false;
            }
            AHardwareBuffer_Desc described{};
            AHardwareBuffer_describe(ahb, &described);
            Vector<int> fds;
            if (!NativeHandleFds(ahb, fds, why)) {
                AHardwareBuffer_release(ahb);
                return false;
            }
            image.Native = ahb;
            image.Fd = fds[0];
            for (SizeT i = 1; i < fds.size(); ++i) ::close(fds[i]);
            image.Offset = 0;
            if (!FourccIsYuv(image.Fourcc)) {
                image.Stride = described.stride * 4;
                return true;
            }
            // Plane positions as the platform reports them when the buffer is locked: offsets from
            // the luma plane's start, the Cb Cr plane at whichever of Cb and Cr comes first.
            const Uint32 sampleBytes = FourccYuvSampleBytes(image.Fourcc);
            image.Stride = described.stride * sampleBytes;
            image.Plane1Stride = image.Stride;
            image.Plane1Offset = image.Stride * image.Height;
            Yuv::PlanarLayout planes;
            std::string lockWhy;
            if (LockYuvPlanes(ahb, planes, lockWhy)) {
                const Uint8* chroma = std::min(planes.Cb.Data, planes.Cr.Data);
                if (planes.Y.RowStride > 0 && chroma > planes.Y.Data) {
                    image.Stride = static_cast<Uint32>(planes.Y.RowStride);
                    image.Plane1Stride = static_cast<Uint32>(planes.Cb.RowStride);
                    image.Plane1Offset = static_cast<Uint32>(chroma - planes.Y.Data);
                }
                AHardwareBuffer_unlock(ahb, nullptr);
            } else {
                MGLOG_W_ONCE("MG_Remote server: a YUV shared image's planes could not be located (%s); its "
                             "plane offsets are nominal",
                             lockWhy.c_str());
            }
            return true;
        }

        void ReleaseNative(void* native) {
            if (native != nullptr) AHardwareBuffer_release(static_cast<AHardwareBuffer*>(native));
        }
#elif !defined(_WIN32)
        // A host has no AHardwareBuffer. A memfd stands in for the dma-buf so the registry, the
        // descriptor round trip and identification can be exercised by the unit tests; no backend
        // binds one.
        Bool YuvAllocatableNative(Uint32) { return true; }

        Bool AllocateNative(Image& image, std::string& why) {
            Uint64 bytes = 0;
            image.Offset = 0;
            if (FourccIsYuv(image.Fourcc)) {
                // A tight semi-planar layout stands in for the platform's.
                const Uint32 sampleBytes = FourccYuvSampleBytes(image.Fourcc);
                image.Stride = image.Width * sampleBytes;
                image.Plane1Stride = ((image.Width + 1) / 2) * 2 * sampleBytes;
                image.Plane1Offset = image.Stride * image.Height;
                bytes = static_cast<Uint64>(image.Plane1Offset) +
                        static_cast<Uint64>(image.Plane1Stride) * ((image.Height + 1) / 2);
            } else {
                image.Stride = image.Width * 4;
                bytes = static_cast<Uint64>(image.Stride) * image.Height;
            }
            const int fd = ::memfd_create("mobilegl-shared-image", MFD_CLOEXEC);
            if (fd < 0) {
                why = Format("memfd_create errno=%lld", errno);
                return false;
            }
            if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
                why = Format("ftruncate errno=%lld", errno);
                ::close(fd);
                return false;
            }
            image.Fd = fd;
            return true;
        }

        void ReleaseNative(void*) {}
#else
        Bool YuvAllocatableNative(Uint32) { return false; }

        Bool AllocateNative(Image&, std::string& why) {
            why = "shared images need POSIX descriptors";
            return false;
        }

        void ReleaseNative(void*) {}
#endif

#if !defined(_WIN32)
        // The planes of a YUV image as the CPU writes them, held until the guard goes: the locked
        // AHardwareBuffer on Android, a mapping of the stand-in memfd on a host.
        class YuvWriteAccess {
        public:
            YuvWriteAccess(const Image& image, std::string& why) : m_image(image) {
#if defined(__ANDROID__)
                auto* ahb = static_cast<AHardwareBuffer*>(image.Native);
                m_ok = ahb != nullptr && LockYuvPlanes(ahb, m_planes, why);
#else
                const SizeT bytes = static_cast<SizeT>(image.Plane1Offset) +
                                    static_cast<SizeT>(image.Plane1Stride) * ((image.Height + 1) / 2);
                void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, image.Fd, 0);
                if (map == MAP_FAILED) {
                    why = Format("mmap of the stand-in buffer errno=%lld", errno);
                    return;
                }
                m_map = map;
                m_mapSize = bytes;
                const Uint32 sampleBytes = FourccYuvSampleBytes(image.Fourcc);
                auto* base = static_cast<Uint8*>(map);
                m_planes.Y = {base, image.Stride, sampleBytes};
                m_planes.Cb = {base + image.Plane1Offset, image.Plane1Stride, 2 * sampleBytes};
                m_planes.Cr = {base + image.Plane1Offset + sampleBytes, image.Plane1Stride, 2 * sampleBytes};
                m_ok = true;
#endif
            }
            ~YuvWriteAccess() {
#if defined(__ANDROID__)
                if (m_ok) AHardwareBuffer_unlock(static_cast<AHardwareBuffer*>(m_image.Native), nullptr);
#else
                if (m_map != nullptr) ::munmap(m_map, m_mapSize);
#endif
            }
            YuvWriteAccess(const YuvWriteAccess&) = delete;
            YuvWriteAccess& operator=(const YuvWriteAccess&) = delete;
            Bool Ok() const { return m_ok; }
            const Yuv::PlanarLayout& Planes() const { return m_planes; }

        private:
            const Image& m_image;
            Yuv::PlanarLayout m_planes;
            Bool m_ok = false;
#if !defined(__ANDROID__)
            void* m_map = nullptr;
            SizeT m_mapSize = 0;
#endif
        };

        // DMA_BUF_IOCTL_SYNC around a CPU read of a foreign buffer: the producer's CPU-side writes
        // are made visible (cache maintenance) and its implicit fences waited for. A buffer that
        // does not take the ioctl (not a dma-buf: a host test's memfd) is read as it is.
        void DmaBufSync(int fd, Bool start) {
            dma_buf_sync sync{};
            sync.flags = DMA_BUF_SYNC_READ | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END);
            while (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0 && (errno == EINTR || errno == EAGAIN)) {
            }
        }
#endif
    } // namespace

    Image::~Image() {
        Registry& reg = Reg();
        {
            const std::lock_guard<std::mutex> lock(reg.mutex);
            // Only this image's own entries: the weak pointer is already expired, but a newer
            // image may have taken the identity over (the kernel recycles inodes).
            if (auto it = reg.byId.find(Id); it != reg.byId.end() && it->second.expired()) reg.byId.erase(it);
            const auto key = std::make_pair(IdentityDev, IdentityIno);
            if (auto it = reg.byIdentity.find(key); it != reg.byIdentity.end() && it->second.expired())
                reg.byIdentity.erase(it);
            if (Foreign != nullptr) {
                const auto source = std::make_pair(Foreign->IdentityDev, Foreign->IdentityIno);
                if (auto it = reg.foreignByIdentity.find(source);
                    it != reg.foreignByIdentity.end() && it->second.expired())
                    reg.foreignByIdentity.erase(it);
            }
        }
#if !defined(_WIN32)
        if (Foreign != nullptr) {
            if (Foreign->Map != nullptr) ::munmap(Foreign->Map, Foreign->MapSize);
            if (Foreign->Fd >= 0) ::close(Foreign->Fd);
        }
        if (Fd >= 0) ::close(Fd);
        if (WriteFence >= 0) ::close(WriteFence);
        for (const int fence : ReadFences) ::close(fence);
#endif
        ReleaseNative(Native);
    }

    Bool FourccIsYuv(Uint32 fourcc) { return fourcc == kFourccNv12 || fourcc == kFourccP010; }

    Uint32 FourccYuvSampleBytes(Uint32 fourcc) {
        if (fourcc == kFourccNv12) return 1;
        if (fourcc == kFourccP010) return 2;
        return 0;
    }

    Bool YuvFourccAllocatable(Uint32 fourcc) {
        if (!FourccIsYuv(fourcc)) return false;
        static std::mutex mutex;
        static std::map<Uint32, Bool> answers;
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = answers.find(fourcc);
        if (it != answers.end()) return it->second;
        const Bool answer = YuvAllocatableNative(fourcc);
        answers.emplace(fourcc, answer);
        return answer;
    }

    Bool FourccSupported(Uint32 fourcc) {
        return fourcc == kFourccAbgr8888 || fourcc == kFourccXbgr8888 || fourcc == kFourccArgb8888 ||
               fourcc == kFourccXrgb8888 || YuvFourccAllocatable(fourcc);
    }

    Bool FourccIgnoresAlpha(Uint32 fourcc) { return fourcc == kFourccXbgr8888 || fourcc == kFourccXrgb8888; }

    namespace {
        // Allocate's body. `foreign`, when given, is on the image before anything can find it.
        ImageRef AllocateImage(Uint32 width, Uint32 height, Uint32 fourcc, UniquePtr<Image::ForeignSource> foreign,
                               std::string& why) {
            if (width == 0 || height == 0 || width > 16384 || height > 16384) {
                why = Format("%lldx%lld is not an image size", width, height);
                return nullptr;
            }
            if (!FourccSupported(fourcc)) {
                why = Format("fourcc 0x%08llx is not a shared-image format", fourcc);
                return nullptr;
            }
            Registry& reg = Reg();
            auto image = std::make_shared<Image>();
            image->Id = reg.nextId.fetch_add(1, std::memory_order_relaxed);
            image->Width = width;
            image->Height = height;
            image->Fourcc = fourcc;
            image->Foreign = std::move(foreign);
            if (!AllocateNative(*image, why)) return nullptr;
#if !defined(_WIN32)
            if (!IdentityOf(image->Fd, &image->IdentityDev, &image->IdentityIno)) {
                why = Format("fstat on the exported descriptor errno=%lld", errno);
                return nullptr;
            }
#endif
            const std::lock_guard<std::mutex> lock(reg.mutex);
            reg.byId[image->Id] = image;
            const auto key = std::make_pair(image->IdentityDev, image->IdentityIno);
            if (auto it = reg.byIdentity.find(key); it != reg.byIdentity.end() && !it->second.expired()) {
                if (!reg.identityAmbiguous) {
                    MGLOG_E("MG_Remote server: two live shared images have one dma-buf identity (dev %llu ino %llu); "
                            "this kernel does not keep dma-buf inodes unique, so a returned descriptor cannot name its "
                            "image and every shared-image import is refused from here on",
                            static_cast<unsigned long long>(key.first), static_cast<unsigned long long>(key.second));
                }
                reg.identityAmbiguous = true;
            }
            reg.byIdentity[key] = image;
            if (image->Foreign != nullptr) {
                reg.foreignByIdentity[std::make_pair(image->Foreign->IdentityDev, image->Foreign->IdentityIno)] =
                    image;
            }
            return image;
        }
    } // namespace

    ImageRef Allocate(Uint32 width, Uint32 height, Uint32 fourcc, std::string& why) {
        return AllocateImage(width, height, fourcc, nullptr, why);
    }

    ImageRef ImportForeignYuv(int fd, Uint32 width, Uint32 height, Uint32 fourcc, const PlaneLayout& layout,
                              std::string& why) {
#if defined(_WIN32)
        (void)fd, (void)width, (void)height, (void)fourcc, (void)layout;
        why = "shared images need POSIX descriptors";
        return nullptr;
#else
        if (!FourccIsYuv(fourcc) || !YuvFourccAllocatable(fourcc)) {
            why = Format("fourcc 0x%08llx is not a YUV format this server can hold", fourcc);
            return nullptr;
        }
        if (width == 0 || height == 0 || width > 16384 || height > 16384) {
            why = Format("%lldx%lld is not an image size", width, height);
            return nullptr;
        }
        if (layout.Count != 2) {
            why = Format("a YUV import needs 2 planes, not %lld", layout.Count);
            return nullptr;
        }
        Uint64 dev = 0, ino = 0;
        if (!IdentityOf(fd, &dev, &ino)) {
            why = Format("fstat on the imported descriptor errno=%lld", errno);
            return nullptr;
        }
        Registry& reg = Reg();
        {
            const std::lock_guard<std::mutex> lock(reg.mutex);
            const auto it = reg.foreignByIdentity.find(std::make_pair(dev, ino));
            ImageRef existing = it != reg.foreignByIdentity.end() ? it->second.lock() : nullptr;
            if (existing != nullptr && existing->Width == width && existing->Height == height &&
                existing->Fourcc == fourcc && existing->Foreign->Layout.Offset[0] == layout.Offset[0] &&
                existing->Foreign->Layout.Offset[1] == layout.Offset[1] &&
                existing->Foreign->Layout.Pitch[0] == layout.Pitch[0] &&
                existing->Foreign->Layout.Pitch[1] == layout.Pitch[1])
                return existing;
        }
        // The planes must fit the buffer: every row the copy reads is inside the mapping.
        const Uint32 sampleBytes = FourccYuvSampleBytes(fourcc);
        const off_t end = ::lseek(fd, 0, SEEK_END);
        if (end <= 0) {
            why = Format("the buffer's size is unknown (lseek errno=%lld)", errno);
            return nullptr;
        }
        const SizeT size = static_cast<SizeT>(end);
        const Uint32 chromaHeight = (height + 1) / 2;
        const Uint64 chromaRow = static_cast<Uint64>((width + 1) / 2) * 2 * sampleBytes;
        const Uint64 lumaEnd = static_cast<Uint64>(layout.Offset[0]) +
                               static_cast<Uint64>(layout.Pitch[0]) * (height - 1) +
                               static_cast<Uint64>(width) * sampleBytes;
        const Uint64 chromaEnd =
            static_cast<Uint64>(layout.Offset[1]) + static_cast<Uint64>(layout.Pitch[1]) * (chromaHeight - 1) + chromaRow;
        if (layout.Pitch[0] < width * sampleBytes || layout.Pitch[1] < chromaRow || lumaEnd > size ||
            chromaEnd > size) {
            why = Format("its planes do not fit the %lld-byte buffer", static_cast<long long>(size));
            return nullptr;
        }
        void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            why = Format("the buffer cannot be mapped for reading (errno=%lld)", errno);
            return nullptr;
        }
        auto foreign = MakeUnique<Image::ForeignSource>();
        foreign->Fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        foreign->Layout = layout;
        foreign->Map = map;
        foreign->MapSize = size;
        foreign->IdentityDev = dev;
        foreign->IdentityIno = ino;
        if (foreign->Fd < 0) {
            ::munmap(map, size);
            why = Format("dup errno=%lld", errno);
            return nullptr;
        }
        // A failed allocation drops the half-built image, and ~Image unmaps and closes the source.
        ImageRef image = AllocateImage(width, height, fourcc, std::move(foreign), why);
        if (image == nullptr) return nullptr;
        MGLOG_I("MG_Remote server: a foreign %ux%u %s dma-buf is sampled through image %llu, refreshed by a CPU "
                "copy (the fallback for a buffer this platform cannot import)",
                width, height, fourcc == kFourccNv12 ? "NV12" : "P010", static_cast<unsigned long long>(image->Id));
        return image;
#endif
    }

    Bool RefreshForeign(const Image& image, std::string& why) {
#if defined(_WIN32)
        (void)image;
        why = "shared images need POSIX descriptors";
        return false;
#else
        if (image.Foreign == nullptr) return true;
        Image::ForeignSource& source = *image.Foreign;
        // WRITE AFTER READ: what a frame still samples of the old content finishes first. The copy
        // is the CPU's, so this is a CPU wait - bounded, like every wait on another session.
        if (!WaitForReads(image, 1000)) {
            MGLOG_W_ONCE("MG_Remote server: a foreign image's readers did not finish within 1 s; refreshed anyway");
        }
        const std::lock_guard<std::mutex> lock(source.Mutex);
        YuvWriteAccess access(image, why);
        if (!access.Ok()) return false;
        const auto* base = static_cast<const Uint8*>(source.Map);
        DmaBufSync(source.Fd, true);
        const Bool copied = Yuv::CopySemiPlanar(base + source.Layout.Offset[0], source.Layout.Pitch[0],
                                                base + source.Layout.Offset[1], source.Layout.Pitch[1], image.Width,
                                                image.Height, FourccYuvSampleBytes(image.Fourcc), access.Planes());
        DmaBufSync(source.Fd, false);
        if (!copied) {
            why = "the locked buffer's planes cannot hold the samples";
            return false;
        }
        ++source.Copies;
        return true;
#endif
    }

    void SetYuvHints(const Image& image, const Yuv::Hints& hints) {
        const std::lock_guard<std::mutex> lock(image.SyncMutex);
        image.YuvHints = hints;
    }

    Yuv::Hints GetYuvHints(const Image& image) {
        const std::lock_guard<std::mutex> lock(image.SyncMutex);
        return image.YuvHints;
    }

    ImageRef Identify(int fd, std::string& why) {
#if defined(_WIN32)
        (void)fd;
        why = "shared images need POSIX descriptors";
        return nullptr;
#else
        Uint64 dev = 0, ino = 0;
        if (!IdentityOf(fd, &dev, &ino)) {
            why = Format("fstat on the imported descriptor errno=%lld", errno);
            return nullptr;
        }
        Registry& reg = Reg();
        const std::lock_guard<std::mutex> lock(reg.mutex);
        if (reg.identityAmbiguous) {
            why = "dma-buf identities are not unique on this kernel";
            return nullptr;
        }
        const auto it = reg.byIdentity.find(std::make_pair(dev, ino));
        ImageRef image = it != reg.byIdentity.end() ? it->second.lock() : nullptr;
        if (image == nullptr) {
            why = Format("the descriptor (dev %llu ino %llu) was not exported by this server", static_cast<long long>(dev),
                         static_cast<long long>(ino));
        }
        return image;
#endif
    }

    ImageRef Find(Uint64 id) {
        Registry& reg = Reg();
        const std::lock_guard<std::mutex> lock(reg.mutex);
        const auto it = reg.byId.find(id);
        return it != reg.byId.end() ? it->second.lock() : nullptr;
    }

    SizeT LiveCount() {
        Registry& reg = Reg();
        const std::lock_guard<std::mutex> lock(reg.mutex);
        SizeT live = 0;
        for (const auto& [id, weak] : reg.byId) live += weak.expired() ? 0 : 1;
        return live;
    }

    // ---- synchronisation ------------------------------------------------------------------------

    namespace SyncFile {
        Bool Signaled(int fd) { return Wait(fd, 0); }

        Bool Wait(int fd, Uint32 timeoutMs) {
#if defined(_WIN32)
            (void)fd, (void)timeoutMs;
            return true;
#else
            if (fd < 0) return true;
            pollfd p{fd, POLLIN, 0};
            for (;;) {
                const int ready = ::poll(&p, 1, static_cast<int>(timeoutMs));
                if (ready > 0) return true;
                if (ready == 0) return false;
                if (errno != EINTR) return true; // not a fence one can wait on: nothing to order
            }
#endif
        }
    } // namespace SyncFile

    namespace {
        void CloseFd(int fd) {
#if !defined(_WIN32)
            if (fd >= 0) ::close(fd);
#endif
        }

        int Dup(int fd) {
#if defined(_WIN32)
            (void)fd;
            return -1;
#else
            return fd >= 0 ? ::fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
#endif
        }

        // Signaled read fences are history; a reader that keeps sampling a still image publishes
        // one per frame, so the list is pruned whenever it is touched and bounded besides.
        constexpr SizeT kMaxReadFences = 8;
        void PruneReads(const Image& image) {
            auto& fences = image.ReadFences;
            for (SizeT i = 0; i < fences.size();) {
                if (SyncFile::Signaled(fences[i])) {
                    CloseFd(fences[i]);
                    fences[i] = fences.back();
                    fences.pop_back();
                } else {
                    ++i;
                }
            }
        }
    } // namespace

    void PublishWrite(const Image& image, int fence) {
        const std::lock_guard<std::mutex> lock(image.SyncMutex);
        CloseFd(image.WriteFence);
        image.WriteFence = fence;
        ++image.WriteGeneration;
    }

    int DupWriteFence(const Image& image, Uint64* generation) {
        const std::lock_guard<std::mutex> lock(image.SyncMutex);
        if (generation != nullptr) *generation = image.WriteGeneration;
        if (image.WriteFence >= 0 && SyncFile::Signaled(image.WriteFence)) {
            CloseFd(image.WriteFence);
            image.WriteFence = -1;
        }
        return Dup(image.WriteFence);
    }

    void PublishRead(const Image& image, int fence) {
        if (fence < 0) return;
        const std::lock_guard<std::mutex> lock(image.SyncMutex);
        PruneReads(image);
        if (image.ReadFences.size() >= kMaxReadFences) {
            // A reader whose frames never complete: the oldest is waited out rather than dropped,
            // since dropping it would let a writer overwrite what that frame still reads.
            SyncFile::Wait(image.ReadFences.front(), 1000);
            CloseFd(image.ReadFences.front());
            image.ReadFences.erase(image.ReadFences.begin());
        }
        image.ReadFences.push_back(fence);
    }

    Vector<int> DupPendingReadFences(const Image& image) {
        const std::lock_guard<std::mutex> lock(image.SyncMutex);
        PruneReads(image);
        Vector<int> out;
        out.reserve(image.ReadFences.size());
        for (const int fence : image.ReadFences) {
            const int copy = Dup(fence);
            if (copy >= 0) out.push_back(copy);
        }
        return out;
    }

    Bool WaitForReads(const Image& image, Uint32 timeoutMs) {
        Bool all = true;
        for (const int fence : DupPendingReadFences(image)) {
            all = SyncFile::Wait(fence, timeoutMs) && all;
            CloseFd(fence);
        }
        return all;
    }

    void ReadTracker::NoteRead(const ImageRef& image) {
        if (image != nullptr) m_images.emplace(image->Id, image);
    }

    void ReadTracker::PublishFrame(int fence) {
        for (const auto& [id, image] : m_images) PublishRead(*image, Dup(fence));
        CloseFd(fence);
        m_images.clear();
    }

    void ReadTracker::PublishFrameAsWrite(int fence) {
        for (const auto& [id, image] : m_images) {
            PublishWrite(*image, Dup(fence));
            PublishRead(*image, Dup(fence));
        }
        CloseFd(fence);
        m_images.clear();
    }

    void SessionHolder::Hold(const ImageRef& image) {
        if (image == nullptr) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_images[image->Id] = image;
    }

    Bool SessionHolder::Release(Uint64 id) {
        ImageRef dropped;
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_images.find(id);
        if (it == m_images.end()) return false;
        dropped = std::move(it->second); // destroyed after the unlock: ~Image takes the registry lock
        m_images.erase(it);
        return true;
    }

    ImageRef SessionHolder::Get(Uint64 id) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_images.find(id);
        return it != m_images.end() ? it->second : nullptr;
    }

    void SessionHolder::Clear() {
        UnorderedMap<Uint64, ImageRef> dropped;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            dropped.swap(m_images);
        }
    }

    SizeT SessionHolder::Count() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_images.size();
    }

} // namespace MobileGL::MG_Remote::Server::SharedImages
