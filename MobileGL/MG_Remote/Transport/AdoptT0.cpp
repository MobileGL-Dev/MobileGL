// MobileGL - MobileGL/MG_Remote/Transport/AdoptT0.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "AdoptT0.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif
#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Transport::AdoptT0 {

    namespace {
        std::atomic<const PlatformForTest*> g_testPlatform{nullptr};

        std::string Format(const char* fmt, long long a, long long b = 0) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), fmt, a, b);
            return buffer;
        }

#if defined(__ANDROID__)

        bool RealHasAhb() { return true; }

        bool RealAllocateHeld(std::uint64_t size, const void* seed, HeldStore& out, std::string& why) {
            out = HeldStore{};
            if (size == 0 || size > 0xFFFFFFFFull) {
                why = Format("a BLOB AHardwareBuffer is at most 4 GiB wide and this store is %lld bytes",
                             static_cast<long long>(size));
                return false;
            }
            AHardwareBuffer_Desc desc{};
            desc.width = static_cast<std::uint32_t>(size);
            desc.height = 1;
            desc.layers = 1;
            desc.format = AHARDWAREBUFFER_FORMAT_BLOB;
            desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                         AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
            AHardwareBuffer* ahb = nullptr;
            const int allocated = AHardwareBuffer_allocate(&desc, &ahb);
            if (allocated != 0 || ahb == nullptr) {
                why = Format("AHardwareBuffer_allocate(%lld bytes) rc=%lld", static_cast<long long>(size), allocated);
                return false;
            }
            void* ptr = nullptr;
            const int locked = AHardwareBuffer_lock(
                ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
            if (locked != 0 || ptr == nullptr) {
                why = Format("AHardwareBuffer_lock rc=%lld", locked);
                AHardwareBuffer_release(ahb);
                return false;
            }
            if (seed != nullptr) {
                std::memcpy(ptr, seed, static_cast<std::size_t>(size));
            } else {
                std::memset(ptr, 0, static_cast<std::size_t>(size));
            }
            out.ahb = ahb;
            out.ptr = ptr;
            out.size = size;
            return true;
        }

        int RealMakeHop(const HeldStore* store, std::string& why) {
            int pair[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
                why = Format("socketpair errno=%lld", errno);
                return -1;
            }
            if (store != nullptr && store->ahb != nullptr) {
                const int sent = AHardwareBuffer_sendHandleToUnixSocket(
                    static_cast<const AHardwareBuffer*>(store->ahb), pair[0]);
                if (sent != 0) {
                    why = Format("AHardwareBuffer_sendHandleToUnixSocket rc=%lld errno=%lld", sent, errno);
                    ::close(pair[0]);
                    ::close(pair[1]);
                    return -1;
                }
            }
            // The written message (and the descriptors it carries) stays queued on pair[1].
            ::close(pair[0]);
            return pair[1];
        }

        void RealReleaseHeld(HeldStore& store) {
            if (store.ahb != nullptr) {
                auto* ahb = static_cast<AHardwareBuffer*>(store.ahb);
                if (store.ptr != nullptr) (void)AHardwareBuffer_unlock(ahb, nullptr);
                AHardwareBuffer_release(ahb);
            }
            store = HeldStore{};
        }

        bool RealReceiveFromHop(int hopFd, std::uint64_t size, void** outAhb, std::string& why) {
            *outAhb = nullptr;
            AHardwareBuffer* ahb = nullptr;
            const int received = AHardwareBuffer_recvHandleFromUnixSocket(hopFd, &ahb);
            if (received != 0 || ahb == nullptr) {
                why = Format("AHardwareBuffer_recvHandleFromUnixSocket rc=%lld errno=%lld", received, errno);
                return false;
            }
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(ahb, &desc);
            if (desc.format != AHARDWAREBUFFER_FORMAT_BLOB || desc.width != size || desc.height != 1) {
                why = Format("the handle describes as format 0x%llx width %lld, not the BLOB its record's store needs",
                             static_cast<long long>(desc.format), static_cast<long long>(desc.width));
                AHardwareBuffer_release(ahb);
                return false;
            }
            *outAhb = ahb;
            return true;
        }

        void RealAcquireImported(void* ahb) {
            if (ahb != nullptr) AHardwareBuffer_acquire(static_cast<AHardwareBuffer*>(ahb));
        }

        void RealReleaseImported(void* ahb) {
            if (ahb != nullptr) AHardwareBuffer_release(static_cast<AHardwareBuffer*>(ahb));
        }

#else

        // No AHardwareBuffer: the server never grants T0 here, so a client never allocates. The
        // empty hop still works, so a flags-0 Offer is real on every POSIX host.
        bool RealHasAhb() { return false; }

        bool RealAllocateHeld(std::uint64_t, const void*, HeldStore& out, std::string& why) {
            out = HeldStore{};
            why = "no AHardwareBuffer on this platform";
            return false;
        }

        int RealMakeHop(const HeldStore* store, std::string& why) {
#if defined(_WIN32)
            (void)store;
            why = "no AF_UNIX socketpair on this platform";
            return -1;
#else
            if (store != nullptr && store->ahb != nullptr) {
                why = "no AHardwareBuffer on this platform";
                return -1;
            }
            int pair[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
                why = Format("socketpair errno=%lld", errno);
                return -1;
            }
            ::close(pair[0]);
            return pair[1];
#endif
        }

        void RealReleaseHeld(HeldStore& store) { store = HeldStore{}; }

        bool RealReceiveFromHop(int, std::uint64_t, void** outAhb, std::string& why) {
            *outAhb = nullptr;
            why = "no AHardwareBuffer on this platform";
            return false;
        }

        void RealAcquireImported(void*) {}
        void RealReleaseImported(void*) {}

#endif
    } // namespace

    bool PlatformHasAhb() {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        return test != nullptr ? test->HasAhb() : RealHasAhb();
    }

    namespace {
        std::atomic<bool> g_peerInAnotherProcessForTest{false};
    } // namespace

    bool PeerIsThisProcess(bool inProcessTransport) {
        return inProcessTransport && !g_peerInAnotherProcessForTest.load(std::memory_order_acquire);
    }

    void SetPeerInAnotherProcessForTest(bool asIfCrossProcess) {
        g_peerInAnotherProcessForTest.store(asIfCrossProcess, std::memory_order_release);
    }

    bool AllocateHeld(std::uint64_t size, const void* seed, HeldStore& out, std::string& why) {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        return test != nullptr ? test->AllocateHeld(size, seed, out, why) : RealAllocateHeld(size, seed, out, why);
    }

    int MakeHop(const HeldStore* store, std::string& why) {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        return test != nullptr ? test->MakeHop(store, why) : RealMakeHop(store, why);
    }

    void ReleaseHeld(HeldStore& store) {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        if (test != nullptr) test->ReleaseHeld(store);
        else RealReleaseHeld(store);
    }

    bool ReceiveFromHop(int hopFd, std::uint64_t size, void** outAhb, std::string& why) {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        return test != nullptr ? test->ReceiveFromHop(hopFd, size, outAhb, why)
                               : RealReceiveFromHop(hopFd, size, outAhb, why);
    }

    void AcquireImported(void* ahb) {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        if (test != nullptr) test->AcquireImported(ahb);
        else RealAcquireImported(ahb);
    }

    void ReleaseImported(void* ahb) {
        const PlatformForTest* test = g_testPlatform.load(std::memory_order_acquire);
        if (test != nullptr) test->ReleaseImported(ahb);
        else RealReleaseImported(ahb);
    }

    void SetPlatformForTest(const PlatformForTest* platform) {
        g_testPlatform.store(platform, std::memory_order_release);
    }

} // namespace MobileGL::MG_Remote::Transport::AdoptT0
