// MobileGL - MobileGL/MG_Remote/Transport/AdoptT0.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 B2 (MG_Remote/CONTRACT-P11.md B2): T0, the zero-copy adoption of a persistently mapped
// store. The CLIENT allocates the store as an AHardwareBuffer (AHB), keeps a CPU lock on it for
// the store's life and uses that pointer as the adopted mapping; the SERVER imports the same
// pages as the store's resident twin (Espryt glBufferStorageExternalEXT, Magma
// VK_ANDROID_external_memory_android_hardware_buffer).
//
// HOW THE HANDLE CROSSES, AND WHY IT IS ORDERED WITH THE RECORDS THAT USE THE STORE. The adoption
// rides the data-ring map_persistent record (not a control op, which the apply thread could
// overtake under run-ahead). Its AHB travels over the session's aux socket as ONE descriptor: a
// fresh socketpair end (the "hop") on which AHardwareBuffer_sendHandleToUnixSocket already wrote
// the handle, with an Offer in the sideband naming the record's seq. The hop keeps the aux channel
// uniform (one fd + sideband per message, FdPassing's shape on every route) and the AHB's own
// message format off it. Step 1 of B2 proved both this and the direct form on the Redmi across
// every route (shell, another app, same app, spawned child; 64 KiB to 128 MiB).
//
// Every T0 map_persistent is preceded by exactly one Offer for its seq - with the handle, or with
// flags 0 when this client could not allocate one - so the server always has something to wait
// for and never waits for nothing.

#pragma once

#include <cstdint>
#include <string>

namespace MobileGL::MG_Remote::Transport::AdoptT0 {

    // The aux sideband that travels with the hop descriptor.
    struct Offer {
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint64_t seq = 0;  // the map_persistent record's seq (its reply slot id)
        std::uint64_t size = 0; // the store's bytes (the AHB's width)
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;
    };
    static_assert(sizeof(Offer) == 32, "the Offer is a fixed 32-byte sideband");
    inline constexpr std::uint32_t kOfferMagic = 0x3054474Du; // 'MGT0' on the wire
    inline constexpr std::uint32_t kOfferVersion = 1;
    // The hop carries an AHB. Clear = the client could not allocate one for this store: the
    // server answers DECLINED at once and the store runs T2.
    inline constexpr std::uint32_t kOfferHasBuffer = 1u;

    // How long the apply thread waits for an Offer whose record it has decoded. The client queues
    // the Offer BEFORE it publishes the record, so the wait normally ends at once; the bound is
    // what turns a lost Offer into a named DECLINED instead of a hang.
    inline constexpr std::uint32_t kOfferWaitMs = 2000;

    // True where AHardwareBuffer exists (Android). Everywhere else T0 is never granted.
    bool PlatformHasAhb();

    // A session whose peer is this very process (Transport::PeerIsThisProcess) never takes this
    // header's arm: its stores are this process's memory already, so T0 there is the backend
    // handing over its own store (kCapAdoptInProcess) or nothing (T2).

    // ---- client half ----------------------------------------------------------------------

    // A store the client allocated. `ahb` is an AHardwareBuffer*, `ptr` its CPU lock, held until
    // ReleaseHeld.
    struct HeldStore {
        void* ahb = nullptr;
        void* ptr = nullptr;
        std::uint64_t size = 0;
    };

    // Allocates a BLOB AHB of `size` bytes (CPU_READ_OFTEN | CPU_WRITE_OFTEN | GPU_DATA_BUFFER),
    // locks it for reading and writing - the lock is held for the store's life - and copies
    // `seed` (size bytes, may be null = zeros) into it. False with the reason in `why`.
    bool AllocateHeld(std::uint64_t size, const void* seed, HeldStore& out, std::string& why);

    // A hop descriptor for `store`: a socketpair whose first end had the handle written on it
    // (AHardwareBuffer_sendHandleToUnixSocket) and was closed. Returns the other end, which the
    // caller shares over aux and closes. `store == nullptr` makes an empty hop (the flags-0
    // Offer's descriptor). -1 with `why` on failure.
    int MakeHop(const HeldStore* store, std::string& why);

    // Unlocks and releases the client's reference. The server's import holds its own.
    void ReleaseHeld(HeldStore& store);

    // ---- server half ----------------------------------------------------------------------

    // Reads the handle a client wrote on `hopFd` (AHardwareBuffer_recvHandleFromUnixSocket) and
    // checks it is a BLOB of `size` bytes. On success *outAhb holds a reference the caller
    // releases with ReleaseImported. The caller closes hopFd.
    bool ReceiveFromHop(int hopFd, std::uint64_t size, void** outAhb, std::string& why);

    void AcquireImported(void* ahb);
    void ReleaseImported(void* ahb);

    // ---- test seam ------------------------------------------------------------------------

    // A host has no AHardwareBuffer, so the host tests of T0's PROTOCOL (the Offer, its order
    // against the record, the inbox, the import call and the answers) run on a stand-in: every
    // function above dispatches to it while one is installed. Null restores the platform's own.
    // The GPU half of T0 is the device's to prove (B2's device gates).
    struct PlatformForTest {
        bool (*HasAhb)();
        bool (*AllocateHeld)(std::uint64_t size, const void* seed, HeldStore& out, std::string& why);
        int (*MakeHop)(const HeldStore* store, std::string& why);
        void (*ReleaseHeld)(HeldStore& store);
        bool (*ReceiveFromHop)(int hopFd, std::uint64_t size, void** outAhb, std::string& why);
        void (*AcquireImported)(void* ahb);
        void (*ReleaseImported)(void* ahb);
    };
    void SetPlatformForTest(const PlatformForTest* platform);

} // namespace MobileGL::MG_Remote::Transport::AdoptT0
