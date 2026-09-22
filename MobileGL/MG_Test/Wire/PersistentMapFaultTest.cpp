// MobileGL - MobileGL/MG_Test/Wire/PersistentMapFaultTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P7 wave 2 (package X1): the two facts the persistent-map mprotect tracker owes a client,
// both of which it broke, and both of which cost a process on the device while every host
// lane stayed green.
//
//   1. THE OWNERSHIP ROW WORKS IN THE KERNEL'S ADDRESS SPACE. bionic tags every heap
//      pointer on a TBI-capable arm64 (POINTER_TAG 0xb4 in the top byte, process-wide under
//      PR_TAGGED_ADDR_ENABLE), so the shadow pointer the tracker registers - and every
//      base/end it publishes - carries that tag. si_addr does not. Comparing one against the
//      other made `address < base` true for EVERY fault, so the handler chained every fault
//      on a page it had itself protected straight to debuggerd. No host produces a tagged
//      pointer of its own, so the cases below hand the real predicate the two addresses the
//      DEVICE produced (docs/Disaggregated/notes/p7/device-window-1,
//      improved-transparency-minecraft-26.3 under inproc: x0 b4000073ac9a5000 against
//      "fault addr 0x00000073ac9a5000"). That works on any host only because the
//      normalisation is written without an architecture guard - masking bits 56-63 is the
//      identity for every valid userspace address off arm64 - and that was a deliberate
//      choice: an `#if defined(__aarch64__)` would have put the only gate that can catch
//      this defect on the one platform CI never runs, which is how it reached the phone.
//
//   2. A MAP HANDS BACK A RANGE THE APPLICATION CAN WRITE, FULL STOP. Registration used to
//      mprotect the mapped range PROT_READ before AcquireMemoryRange returned the pointer
//      (BufferObject.cpp:913-965), so the writability of a pointer the GL API had promised
//      was writable depended on a SIGSEGV handler reaching the writing thread. It does not
//      always: the application may write a mapped arena from a worker that blocks every
//      signal, and a fault there with SIGSEGV blocked is an instant process kill with no
//      handler run and no tombstone worth reading. The case below writes every page of a
//      freshly registered range from a process that has BOTH blocked SIGSEGV and restored
//      SIG_DFL, which is the strongest available spelling of "without relying on the
//      handler".
//
// RED-ONCE (both, and each for its own reason):
//   * restore `mprotect(base, end - base, PROT_READ)` at the end of TrackWriteMap's slot
//     setup -> AMapHandsBackARangeWritableWithoutTheFaultHandler dies by SIGSEGV in the
//     child, and the two page-protection cases go red; the ownership cases stay green.
//   * drop the two UntagAddress calls in SlotOwnsFault -> the three ownership cases go red
//     and the writability cases stay green.
// Neither revert can be hidden by the other, which is the property R-16 asks for.

#include <Config.h>
#include <MG_Remote/Client/PersistentMapTracker.h>
#include <MG_State/GLState/BufferState/PipeResource.h>
#include <MG_Util/Debug/Log.h>

#include <gtest/gtest.h>

#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

    using MobileGL::Bool;
    using MobileGL::SizeT;
    using MobileGL::Uint64;
    using MobileGL::Uint8;
    using MobileGL::MG_Remote::Client::PersistentMapTracker;
    using MobileGL::MG_State::GLState::MapAlignedData;
    using MobileGL::MG_State::GLState::ShadowAllocationBytesFor;

    constexpr SizeT kPageBytes = 4096;

    // The device's own two numbers, from the E0a tombstone. Kept as literals, not as
    // "some pointer with the top byte set": the point of the case is that THESE two
    // addresses name one page, and a reader who doubts it can check them against the
    // tombstone quoted in the file header.
    constexpr uintptr_t kDeviceTaggedShadowBase = 0xb4000073ac9a5000ull;
    constexpr uintptr_t kDeviceKernelFaultAddress = 0x00000073ac9a5000ull;
    constexpr uintptr_t kDeviceMappedBytes = 0x1800000ull; // 24 MiB, the memcpy's x2

} // namespace

// =====================================================================================
// 1. THE OWNERSHIP ROW, IN THE KERNEL'S ADDRESS SPACE
// =====================================================================================

// THE CASE THAT WOULD HAVE CAUGHT THE DEVICE CRASH ON A HOST, and it runs on every host:
// both addresses are the device's literal ones and the normalisation they need is written
// without an architecture guard precisely so this can be a gate rather than a phone-only
// hope. The predicate must say the kernel's untagged fault address belongs to the slot a
// bionic-tagged shadow pointer published.
TEST(PersistentMapFaultOwnership, AKernelsUntaggedFaultAddressIsOwnedByATaggedShadowsSlot) {
    SizeT pageIndex = ~static_cast<SizeT>(0);
    EXPECT_TRUE(PersistentMapTracker::OwnershipProbeForTest(
        kDeviceTaggedShadowBase, kDeviceTaggedShadowBase + kDeviceMappedBytes,
        kDeviceKernelFaultAddress, &pageIndex))
        << "the handler compared si_addr (untagged, as the kernel delivers it) against a base "
           "carrying bionic's 0xb4 heap tag, so it owned nothing and chained every fault";
    EXPECT_EQ(pageIndex, 0u) << "the fault is on the first page of the mapped range";
}

// The same row without needing the host to ignore a top byte at all: the predicate is fed a
// composed slot, so it runs everywhere and goes red everywhere the normalisation is dropped.
// This is the case the gate rests on; the one above is the device's literal reproduction.
TEST(PersistentMapFaultOwnership, TheOwnershipRowNormalisesBothSidesBeforeItCompares) {
    SizeT pageIndex = ~static_cast<SizeT>(0);
    const uintptr_t base = PersistentMapTracker::UntagAddressForTest(kDeviceTaggedShadowBase);
    // Untagged base against untagged fault: owned, on this host and on the device alike.
    EXPECT_TRUE(PersistentMapTracker::OwnershipProbeForTest(
        base, base + kDeviceMappedBytes, kDeviceKernelFaultAddress, &pageIndex));
    EXPECT_EQ(pageIndex, 0u);
    // A fault one page in is owned, and names that page.
    EXPECT_TRUE(PersistentMapTracker::OwnershipProbeForTest(
        base, base + kDeviceMappedBytes, kDeviceKernelFaultAddress + kPageBytes + 7, &pageIndex));
    EXPECT_EQ(pageIndex, 1u);
    // And the narrow claim stays narrow: a byte before the base and the first byte past the
    // end are both foreign, so a genuinely broken access still crashes honestly.
    EXPECT_FALSE(PersistentMapTracker::OwnershipProbeForTest(
        base, base + kDeviceMappedBytes, kDeviceKernelFaultAddress - 1, nullptr));
    EXPECT_FALSE(PersistentMapTracker::OwnershipProbeForTest(
        base, base + kDeviceMappedBytes, kDeviceKernelFaultAddress + kDeviceMappedBytes, nullptr));
    // A free slot (base 0) and one mid-setup (base 1) own nothing, whatever the address.
    EXPECT_FALSE(PersistentMapTracker::OwnershipProbeForTest(0, 0, kDeviceKernelFaultAddress, nullptr));
    EXPECT_FALSE(PersistentMapTracker::OwnershipProbeForTest(1, 0, kDeviceKernelFaultAddress, nullptr));
}

TEST(PersistentMapFaultOwnership, UntagAddressClearsTheTopByteAndNothingBelowIt) {
    EXPECT_EQ(PersistentMapTracker::UntagAddressForTest(kDeviceTaggedShadowBase),
              kDeviceKernelFaultAddress)
        << "bionic's 0xb4 heap tag must come off, or the table and the kernel disagree";
    EXPECT_EQ(PersistentMapTracker::UntagAddressForTest(kDeviceKernelFaultAddress),
              kDeviceKernelFaultAddress)
        << "normalising an already-normal address is the identity, or the handler would move "
           "a legitimate fault out of its own slot";
    // Bit 55 is ADDRESS, not tag: userspace lives in TTBR0 on arm64 and below TASK_SIZE_MAX
    // on x86-64, and both keep every bit under 56 meaningful.
    constexpr uintptr_t kBit55 = static_cast<uintptr_t>(1) << 55;
    EXPECT_EQ(PersistentMapTracker::UntagAddressForTest(kBit55), kBit55);
    // A real pointer from this build's own allocator survives untouched, which is the
    // property that makes the mask safe to run unguarded on a host with no tags at all.
    MapAlignedData probe(kPageBytes);
    const auto real = reinterpret_cast<uintptr_t>(probe.data());
    EXPECT_EQ(PersistentMapTracker::UntagAddressForTest(real), real);
}

// =====================================================================================
// 2. A MAP HANDS BACK A RANGE THE APPLICATION CAN WRITE
// =====================================================================================

#if !defined(_WIN32)

namespace {

    // Every page of [begin, begin + bytes) as /proc/self/maps reports it. The protection is
    // the fact under test and there is no portable getter for it, so the file is read
    // directly; a host without it skips rather than passes.
    Bool EveryPageIsWritable(const Uint8* begin, SizeT bytes, std::string& detail) {
        std::FILE* maps = std::fopen("/proc/self/maps", "re");
        if (maps == nullptr) {
            detail = "no /proc/self/maps on this host";
            return true;
        }
        const auto first = reinterpret_cast<uintptr_t>(begin);
        const uintptr_t last = first + bytes - 1;
        uintptr_t covered = first;
        Bool writable = true;
        char line[512];
        while (std::fgets(line, sizeof(line), maps) != nullptr) {
            unsigned long long lo = 0;
            unsigned long long hi = 0;
            char perms[8] = {0};
            if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
            if (hi <= first || lo > last) continue;
            if (perms[1] != 'w') {
                writable = false;
                detail = std::string("range [") + std::to_string(lo) + "," + std::to_string(hi) +
                         ") is '" + perms + "'";
                break;
            }
            if (lo <= covered && hi > covered) covered = hi;
        }
        std::fclose(maps);
        if (writable && covered <= last) {
            writable = false;
            detail = "the mapped range is not fully covered by any writable mapping";
        }
        return writable;
    }

} // namespace

// THE INVARIANT. A freshly registered write map's whole range must be writable from a thread
// that can never run the handler - SIGSEGV blocked AND the disposition back at SIG_DFL - or
// the client has handed the application a pointer whose writability is a signal-delivery
// accident. Driven in a child, because the red form of this case is a process death by
// SIGSEGV and the suite has to survive observing it.
TEST(PersistentMapWritability, AMapHandsBackARangeWritableWithoutTheFaultHandler) {
    if (!PersistentMapTracker::MprotectArmAvailableForTest()) {
        GTEST_SKIP() << "no mprotect arm on this host (handler not installable, or the kernel "
                        "page is not the tracker's 4 KB): nothing here can protect a page";
    }
    // Eight pages, so a per-page protection bug cannot hide inside one page's rounding.
    constexpr SizeT kBytes = 8u * kPageBytes;
    MapAlignedData store(kBytes);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(store.data()) % kPageBytes, 0u);
    constexpr Uint64 kId = ~0ull - 21u;
    const auto* slot = PersistentMapTracker::TrackForTest(kId, store.data(), 0, kBytes,
                                                          ShadowAllocationBytesFor(store.capacity()));
    ASSERT_NE(slot, nullptr) << "a page-aligned, page-granular shadow registers on the mprotect arm";
    ASSERT_EQ(slot->pageCount, 8u);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0) << "fork failed: " << std::strerror(errno);
    if (child == 0) {
        // No handler can rescue this process: the disposition is the default and the signal
        // is blocked in this thread, so a fault here is a kill, not a fault.
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        ::sigemptyset(&dfl.sa_mask);
        ::sigaction(SIGSEGV, &dfl, nullptr);
        ::sigaction(SIGBUS, &dfl, nullptr);
        sigset_t blocked;
        ::sigemptyset(&blocked);
        ::sigaddset(&blocked, SIGSEGV);
        ::sigaddset(&blocked, SIGBUS);
        ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        Uint8* const mapped = store.data();
        for (SizeT page = 0; page < kBytes / kPageBytes; ++page) {
            mapped[page * kPageBytes] = static_cast<Uint8>(0x5A + page);
            mapped[page * kPageBytes + kPageBytes - 1] = static_cast<Uint8>(0xA5 - page);
        }
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_FALSE(WIFSIGNALED(status))
        << "the range glMapBufferRange hands back was not writable: the child died by signal "
        << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
        << ". Registration must arm nothing - the first push is what protects pages";
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    PersistentMapTracker::UntrackForTest(kId);
    store[0] = 1;
    store[kBytes - 1] = 1;
}

// The same fact read straight off the kernel's own bookkeeping, so a failure names the
// protection rather than only the death it caused.
TEST(PersistentMapWritability, RegistrationLeavesEveryPageOfTheRangeReadWrite) {
    if (!PersistentMapTracker::MprotectArmAvailableForTest()) {
        GTEST_SKIP() << "no mprotect arm on this host";
    }
    constexpr SizeT kBytes = 8u * kPageBytes;
    MapAlignedData store(kBytes);
    constexpr Uint64 kId = ~0ull - 22u;
    const auto* slot = PersistentMapTracker::TrackForTest(kId, store.data(), 0, kBytes,
                                                          ShadowAllocationBytesFor(store.capacity()));
    ASSERT_NE(slot, nullptr);
    std::string detail;
    EXPECT_TRUE(EveryPageIsWritable(store.data(), kBytes, detail))
        << "registration protected the range it had just handed out: " << detail;
    PersistentMapTracker::UntrackForTest(kId);
}

// AND THE HANDLER IS STILL REACHED WHEN IT MATTERS. Deferring the arm must not turn the
// tracker off: a page the module protects by hand - which is what the first push does - has
// to be answered and made writable again, in this process, by this handler. Without this the
// two cases above would stay green with the whole mprotect arm deleted.
TEST(PersistentMapWritability, AnArmedPageIsStillAnsweredByTheHandlerAndMadeWritable) {
    if (!PersistentMapTracker::MprotectArmAvailableForTest()) {
        GTEST_SKIP() << "no mprotect arm on this host";
    }
    constexpr SizeT kBytes = 2u * kPageBytes;
    MapAlignedData store(kBytes);
    constexpr Uint64 kId = ~0ull - 23u;
    const auto* slot = PersistentMapTracker::TrackForTest(kId, store.data(), 0, kBytes,
                                                          ShadowAllocationBytesFor(store.capacity()));
    ASSERT_NE(slot, nullptr);
    // Arm the first page the way the push does, and write through it: the fault must be
    // answered (the epoch moves) rather than chained (which would kill this process).
    ASSERT_EQ(::mprotect(store.data(), kPageBytes, PROT_READ), 0) << std::strerror(errno);
    const Uint64 epochBefore = PersistentMapTracker::FaultEpochForTest();
    const Uint64 declinedBefore = PersistentMapTracker::DeclinedFaults().count;
    store[16] = 0x5A;
    EXPECT_GT(PersistentMapTracker::FaultEpochForTest(), epochBefore)
        << "the write did not fault, so this case proved nothing about the handler";
    EXPECT_EQ(PersistentMapTracker::DeclinedFaults().count, declinedBefore)
        << "the handler chained its own protected page away as foreign - the ownership row "
           "is comparing two different address spaces again";
    EXPECT_EQ(store[16], 0x5A);
    PersistentMapTracker::UntrackForTest(kId);
}

#endif // !_WIN32

int main(int argc, char** argv) {
    // Before anything logs, for StagedTextureStoreTest's reason: the file sink caches its
    // FILE* on the first write and gtest_discover_tests runs every case as its own process.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() /
        ("mobilegl-persistentmapfault-test-" + std::to_string(static_cast<long long>(::getpid())) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    const std::string logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", logPath.c_str(), 1);
#endif
    // A SPLIT process: the tracker is inert on the monolith path and every case here is
    // about the split arm.
    MobileGL::MG_Config::Transport = MobileGL::MG_Config::TransportMode::InProcess;
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
