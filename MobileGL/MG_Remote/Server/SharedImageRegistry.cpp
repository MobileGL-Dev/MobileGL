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
#endif
#if !defined(_WIN32)
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

        Bool AllocateNative(Image& image, std::string& why) {
            AHardwareBuffer_Desc desc{};
            desc.width = image.Width;
            desc.height = image.Height;
            desc.layers = 1;
            desc.format = FourccIgnoresAlpha(image.Fourcc) ? AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
                                                          : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
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
            image.Stride = described.stride * 4;
            image.Offset = 0;
            return true;
        }

        void ReleaseNative(void* native) {
            if (native != nullptr) AHardwareBuffer_release(static_cast<AHardwareBuffer*>(native));
        }
#elif !defined(_WIN32)
        // A host has no AHardwareBuffer. A memfd stands in for the dma-buf so the registry, the
        // descriptor round trip and identification can be exercised by the unit tests; no backend
        // binds one.
        Bool AllocateNative(Image& image, std::string& why) {
            image.Stride = image.Width * 4;
            image.Offset = 0;
            const int fd = ::memfd_create("mobilegl-shared-image", MFD_CLOEXEC);
            if (fd < 0) {
                why = Format("memfd_create errno=%lld", errno);
                return false;
            }
            if (::ftruncate(fd, static_cast<off_t>(image.Stride) * image.Height) != 0) {
                why = Format("ftruncate errno=%lld", errno);
                ::close(fd);
                return false;
            }
            image.Fd = fd;
            return true;
        }

        void ReleaseNative(void*) {}
#else
        Bool AllocateNative(Image&, std::string& why) {
            why = "shared images need POSIX descriptors";
            return false;
        }

        void ReleaseNative(void*) {}
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
        }
#if !defined(_WIN32)
        if (Fd >= 0) ::close(Fd);
#endif
        ReleaseNative(Native);
    }

    Bool FourccSupported(Uint32 fourcc) {
        return fourcc == kFourccAbgr8888 || fourcc == kFourccXbgr8888 || fourcc == kFourccArgb8888 ||
               fourcc == kFourccXrgb8888;
    }

    Bool FourccIgnoresAlpha(Uint32 fourcc) { return fourcc == kFourccXbgr8888 || fourcc == kFourccXrgb8888; }

    ImageRef Allocate(Uint32 width, Uint32 height, Uint32 fourcc, std::string& why) {
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
        return image;
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
