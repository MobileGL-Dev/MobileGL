// MobileGL - MobileGL/MG_Remote/Client/PersistentMapTracker.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PersistentMapTracker.h"

#include <MG_Pipe/MGPipeTypes.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Remote/Server/ServerLoop.h>

#include <xxhash.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace MobileGL::MG_Remote::Client {

    using MG_State::GLState::BufferObject;

    // ---------------------------------------------------------------------------
    // THE MPROTECT DIRTY TRACKER. Scanning a persistent write map for changes is
    // O(range) per verb no matter how cheap the hash is (measured: a ~10 MB arena
    // hashed at every draw is the whole frame at view distance 12). The MMU
    // already knows what changed: after each push the range is protected
    // PROT_READ, the application's next write faults, the handler marks the page
    // dirty and makes it writable again, and the next push ships only the 64 KB
    // blocks that contain a faulted page. The per-frame cost collapses from
    // "hash every block of every arena at every draw" to "one fault per touched
    // page per push-cycle plus the block pushes that always existed".
    //
    // THE TABLE IS A FIXED ARRAY, NOT A MAP: the SIGSEGV handler runs on ANY
    // thread (chunk builders write arenas off the render thread) while
    // registration runs on the GL thread, so the handler scans a lock-free array
    // of atomics; the GL thread publishes an entry by writing pageBits, then end,
    // then base with release ordering, and retires one by clearing base first.
    //
    // CHAINING IS LOAD-BEARING: the JVM uses SIGSEGV for implicit null checks,
    // so a fault outside every tracked range MUST reach the previous handler or
    // the first NPE after this installs is a process kill. Ownership is three
    // tests, applied in the handler below: kernel-raised permission fault
    // (SEGV_ACCERR - never a userspace raise, never an unmapped page), inside a
    // live tracked range, and not an immediate same-instruction refault (a write
    // we unprotected succeeds, so an instant refault is a non-write access and
    // must crash honestly). Everything failing any test is chained verbatim.
    // ---------------------------------------------------------------------------
    namespace {

        constexpr SizeT kMaxTrackedMaps = 64;
        constexpr SizeT kPageShift = 12;
        constexpr SizeT kPageBytes = 1u << kPageShift;
        // The bitmap is FIXED-CAPACITY and never reallocated: a handler mid-flight on
        // another thread can hold the pointer it loaded before a retire, so a grow-and-free
        // would be a use-after-free no memory ordering can close. 64 K pages = 256 MB per
        // tracked map, 8 KB per slot, allocated lazily once and kept for the process.
        constexpr SizeT kMaxTrackedPages = 65536;
        constexpr SizeT kPageBitsWords = kMaxTrackedPages / 64;
        // Slot states for base: 0 = free, 1 = being set up (handler must skip: end is 0),
        // anything else = live. The claim token is the sentinel, NOT the real base, so the
        // handler can never observe a half-published entry (base live, end still stale).
        constexpr uintptr_t kSlotSettingUp = 1;

        TrackedWriteMap g_trackedMaps[kMaxTrackedMaps];
        struct sigaction g_prevSegvAction {};
        std::atomic<Bool> g_segvInstalled{false};
        // Bumped by the handler for every write fault it answers. PushDrawConsumers
        // compares it against the GL thread's last-walk value: an unmoved epoch proves
        // no interior byte of any tracked map changed, because interior dirtiness can
        // only ever arrive through a fault.
        std::atomic<Uint64> g_faultEpoch{0};

        // THE OWNERSHIP TESTS. "Ours" is a NARROW claim, and every fault that fails any
        // one of these three tests is chained to the previous owner untouched, so a
        // genuinely broken memory access crashes exactly as it would without us:
        //
        // (1) KERNEL-RAISED PERMISSION FAULT ONLY: si_code must be SEGV_ACCERR. A fault
        //     on an unmapped page (SEGV_MAPERR) is a wild pointer, and anything raised
        //     from userspace (raise/kill/tgkill/sigqueue) carries si_code <= 0 - this
        //     tracker never raises SIGSEGV itself, so a user-raised one is always foreign.
        // (2) INSIDE A LIVE TRACKED RANGE: a fault anywhere else is the application's.
        // (3) NOT A REFAULT WITHOUT RE-ARM: a write to a page this handler just
        //     unprotected SUCCEEDS, so a fault on a page whose dirty bit is still set
        //     is either a sibling thread mid-answer (answered once more, idempotently)
        //     or a non-write access such as an execute of the data page (chained, so it
        //     crashes honestly instead of refaulting forever). The page's own bit is
        //     the discriminator - see RefaultOfANonWriteAccess.

        uintptr_t FaultPcFrom(void* ucontext) {
#if defined(__aarch64__)
            return static_cast<uintptr_t>(
                reinterpret_cast<ucontext_t*>(ucontext)->uc_mcontext.pc);
#elif defined(__x86_64__)
            return static_cast<uintptr_t>(
                reinterpret_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_RIP]);
#else
            // No PC access on this arch: the refault test is skipped, ownership rests
            // on the si_code and range tests alone.
            return 0;
#endif
        }

        struct FaultRepeatSlot {
            std::atomic<pid_t> tid{0};
            uintptr_t pc = 0;
            uintptr_t address = 0;
            Uint32 setBitAnswers = 0;
        };
        FaultRepeatSlot g_faultRepeats[8];

        // THE REFAULT TEST, and its exact discriminator: the page's own bit. The
        // tracker keeps ONE invariant - bit set <=> page writable, bit clear <=>
        // page read-only (the handler sets-then-unprotects, the push
        // clears-then-rearms). So at a fault, the bit the fetch_or just observed
        // decides what this access can be:
        //
        //   bit was CLEAR - the page was re-armed since the last answer, so this
        //     is a fresh write. A same-(thread, instruction, address) refault is
        //     EXPECTED here (malloc hands the same block in a re-armed shared
        //     page to the same free() site a frame later); it is not suspicious.
        //   bit was SET - the page should already be writable, so a WRITE cannot
        //     be what faulted. One exception: a sibling thread is mid-answer on
        //     the same page right now (it set the bit and has not unprotected
        //     yet). That thread's write succeeds once either answer lands, so it
        //     never comes back; a non-write access (an execute of the data page)
        //     comes back every time. Answer the first set-bit fault at a
        //     (thread, pc, address), chain the second.
        Bool RefaultOfANonWriteAccess(uintptr_t pc, uintptr_t address, Bool bitWasSet) {
            const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
            FaultRepeatSlot* slot = nullptr;
            for (auto& candidate : g_faultRepeats) {
                if (candidate.tid.load(std::memory_order_acquire) == tid) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot == nullptr) {
                for (auto& candidate : g_faultRepeats) {
                    pid_t empty = 0;
                    if (candidate.tid.compare_exchange_strong(empty, tid)) {
                        slot = &candidate;
                        break;
                    }
                }
            }
            // Full: evict deterministically. Losing a record costs the refault test on
            // that thread, never a wrong answer - the next fault re-records it.
            if (slot == nullptr) slot = &g_faultRepeats[static_cast<SizeT>(tid) & 7];
            if (!bitWasSet) {
                slot->pc = pc;
                slot->address = address;
                slot->setBitAnswers = 0;
                return false;
            }
            const Bool same = slot->pc == pc && slot->address == address;
            const Uint32 answers = same ? slot->setBitAnswers : 0;
            if (answers >= 1) return true;
            slot->pc = pc;
            slot->address = address;
            slot->setBitAnswers = answers + 1;
            return false;
        }

        void ChainToPreviousSegvHandler(int sig, siginfo_t* info, void* ucontext) {
            // Chain with the disposition the previous owner actually installed - the two
            // entry points share a union, so SA_SIGINFO decides which signature is live,
            // and the original info/ucontext go through verbatim so the next handler can
            // run its own si_code test. For SIG_DFL (and the meaningless-but-possible
            // SIG_IGN) restore the default and return: the faulting instruction
            // re-executes, faults again, and dies the way an untracked fault would have.
            if ((g_prevSegvAction.sa_flags & SA_SIGINFO) != 0) {
                if (g_prevSegvAction.sa_sigaction != nullptr) {
                    g_prevSegvAction.sa_sigaction(sig, info, ucontext);
                    return;
                }
            } else if (g_prevSegvAction.sa_handler != SIG_DFL &&
                       g_prevSegvAction.sa_handler != SIG_IGN &&
                       g_prevSegvAction.sa_handler != nullptr) {
                g_prevSegvAction.sa_handler(sig);
                return;
            }
            struct sigaction restore {};
            restore.sa_handler = SIG_DFL;
            sigemptyset(&restore.sa_mask);
            sigaction(SIGSEGV, &restore, nullptr);
        }

        void PersistentWriteFaultHandler(int sig, siginfo_t* info, void* ucontext) {
            if (info != nullptr && info->si_code == SEGV_ACCERR) {
                const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
                // EVERY overlapping slot gets its bit, not just the first: two tracked
                // shadows can share an edge page (separate heap allocations, one page
                // granularity), and unprotecting after only the first mark would leave the
                // second buffer's block permanently un-pushed - the page is writable now,
                // so it never faults again.
                Bool matched = false;
                Bool bitWasSet = false;
                for (SizeT i = 0; i < kMaxTrackedMaps; ++i) {
                    const uintptr_t base =
                        g_trackedMaps[i].base.load(std::memory_order_acquire);
                    if (base <= kSlotSettingUp || address < base) continue;
                    const uintptr_t end =
                        g_trackedMaps[i].end.load(std::memory_order_acquire);
                    if (address >= end) continue;
                    const SizeT pageIndex = (address - base) >> kPageShift;
                    std::atomic<Uint64>* bits = g_trackedMaps[i].pageBits;
                    if (bits != nullptr) {
                        const Uint64 mask = 1ull << (pageIndex & 63);
                        const Uint64 old =
                            bits[pageIndex >> 6].fetch_or(mask, std::memory_order_relaxed);
                        if (!matched) bitWasSet = (old & mask) != 0;
                    }
                    matched = true;
                }
                if (matched &&
                    !RefaultOfANonWriteAccess(FaultPcFrom(ucontext), address, bitWasSet)) {
                    // The epoch moves BEFORE the unprotect: a walk that starts the
                    // moment the page becomes writable must still see this fault.
                    g_faultEpoch.fetch_add(1, std::memory_order_relaxed);
                    // Un-arm is a raw syscall, deliberately not the libc wrapper's
                    // bookkeeping: the handler's whole job is to get out of the way.
                    mprotect(reinterpret_cast<void*>(address & ~(kPageBytes - 1)),
                             kPageBytes, PROT_READ | PROT_WRITE);
                    return;
                }
            }
            ChainToPreviousSegvHandler(sig, info, ucontext);
        }

        void InstallWriteFaultHandler() {
            Bool expected = false;
            if (!g_segvInstalled.compare_exchange_strong(expected, true)) return;
            struct sigaction action {};
            action.sa_sigaction = &PersistentWriteFaultHandler;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_SIGINFO;
            if (sigaction(SIGSEGV, &action, &g_prevSegvAction) != 0) {
                MGLOG_W("MGPipe: persistent-map mprotect tracking unavailable (sigaction "
                        "failed); persistent write maps fall back to the hash scan");
                g_prevSegvAction = {};
                g_segvInstalled.store(false);
            }
        }

        void UntrackWriteMap(Uint64 lifetimeId);

        Bool TrackWriteMap(Uint64 lifetimeId, const Uint8* shadow,
                           SizeT rangeBegin, SizeT rangeEnd) {
            if (shadow == nullptr || rangeEnd <= rangeBegin) return false;
            InstallWriteFaultHandler();
            if (!g_segvInstalled.load()) return false;
            // INWARD alignment, and the reason is a process-killer: with OUTWARD
            // alignment the protected edge pages hold FOREIGN bytes (the shadow is a
            // heap allocation; malloc metadata and other allocations share its first
            // and last page). A foreign write to such a page is fine for a thread
            // that runs our handler - it is marked and unprotected - but driver and
            // runtime worker threads commonly block every signal, and a fault there
            // with SIGSEGV blocked ends the process on the spot, undebuggable.
            // Protecting only pages FULLY inside the mapped range means every byte of
            // a protected page is the shadow's own, and no other allocation can ever
            // be made to fault by us. The head/tail partial pages stay writable and
            // their blocks are pushed unconditionally at every push (at most two).
            const uintptr_t rawBegin = reinterpret_cast<uintptr_t>(shadow + rangeBegin);
            const uintptr_t rawEnd = reinterpret_cast<uintptr_t>(shadow + rangeEnd);
            uintptr_t base = (rawBegin + kPageBytes - 1) &
                             ~static_cast<uintptr_t>(kPageBytes - 1);
            uintptr_t end = rawEnd & ~static_cast<uintptr_t>(kPageBytes - 1);
            // A SUB-PAGE range is not refused; it is registered with an EMPTY interior
            // (no pages are protected, no bitmap is needed) and hasEdges set, so the
            // whole range is served by the two edge hashes - bounded at 8 KB per push -
            // instead of falling to the hash arm, where as an "untracked" member it
            // would veto the epoch skip for every tracked buffer in the process and
            // force the full binding walk at every draw (measured on device). end is
            // clamped up to base so the [base, end) arithmetic below never underflows.
            if (end < base) end = base;
            const SizeT pageCount = (end - base) >> kPageShift;
            if (pageCount > kMaxTrackedPages) {
                MGLOG_W("MGPipe: persistent write map of %zu pages exceeds the mprotect "
                        "tracker's %zu-page capacity; this buffer falls back to the hash scan",
                        pageCount, kMaxTrackedPages);
                return false;
            }
            // A remap/re-register of the same buffer must not stack a second slot on the
            // same lifetimeId: retire whatever this id had first. No-op when untracked.
            UntrackWriteMap(lifetimeId);
            for (SizeT i = 0; i < kMaxTrackedMaps; ++i) {
                uintptr_t empty = 0;
                if (!g_trackedMaps[i].base.compare_exchange_strong(empty, kSlotSettingUp)) {
                    continue;
                }
                // Claimed. From here until the final base store the slot reads as
                // (base=1, end=0), which the handler skips on both tests.
                if (pageCount > 0 && g_trackedMaps[i].pageBits == nullptr) {
                    g_trackedMaps[i].pageBits = new std::atomic<Uint64>[kPageBitsWords];
                }
                g_trackedMaps[i].pageCount = pageCount;
                g_trackedMaps[i].lifetimeId = lifetimeId;
                g_trackedMaps[i].edgeHashHead = 0;
                g_trackedMaps[i].edgeHashTail = 0;
                g_trackedMaps[i].hasEdges = base != rawBegin || end != rawEnd;
                // EVERY BIT SET, NOT ZEROED - the same "fresh state pushes everything
                // once" rule the hash arm keeps: the server has never seen these bytes,
                // so the first push must ship the whole interior, not just the pages the
                // application happened to write first. The first push then clears and
                // re-arms page by page, and the set-bit read the fault discriminator
                // does on a pre-first-push fault is answered once and unprotected -
                // harmless, and the invariant (bit set <=> writable) holds from the
                // first push onward. The last word is masked: a set bit past pageCount
                // would re-arm a page outside the interior - a foreign page, which is
                // the process-killer the inward alignment exists to keep out.
                const SizeT wordCount = (pageCount + 63) / 64;
                for (SizeT w = 0; w < wordCount; ++w) {
                    const SizeT bitsInWord =
                        (w == wordCount - 1 && (pageCount & 63) != 0) ? (pageCount & 63) : 64;
                    g_trackedMaps[i].pageBits[w].store(
                        bitsInWord == 64 ? ~0ull : ((1ull << bitsInWord) - 1),
                        std::memory_order_relaxed);
                }
                if (end > base &&
                    mprotect(reinterpret_cast<void*>(base), end - base, PROT_READ) != 0) {
                    g_trackedMaps[i].base.store(0, std::memory_order_release);
                    return false;
                }
                // Publish order is end, then base: the handler reads base first, and a base
                // it can observe is only ever one whose end is already visible.
                g_trackedMaps[i].end.store(end, std::memory_order_release);
                g_trackedMaps[i].base.store(base, std::memory_order_release);
                return true;
            }
            MGLOG_W("MGPipe: the mprotect tracker is full (%zu live persistent write maps); "
                    "this buffer falls back to the hash scan",
                    kMaxTrackedMaps);
            return false;
        }

        void UntrackWriteMap(Uint64 lifetimeId) {
            for (SizeT i = 0; i < kMaxTrackedMaps; ++i) {
                if (g_trackedMaps[i].lifetimeId != lifetimeId ||
                    g_trackedMaps[i].base.load(std::memory_order_acquire) == 0) {
                    continue;
                }
                const uintptr_t base = g_trackedMaps[i].base.load(std::memory_order_acquire);
                const uintptr_t end = g_trackedMaps[i].end.load(std::memory_order_acquire);
                // Retire first, then unprotect: a fault racing the retire falls through to
                // the previous handler, which is the honest answer for a page we no longer
                // own; a fault racing the unprotect is impossible, the page is writable.
                // end goes to 0 with the retire so a later re-claim of this slot reads
                // (base=1, end=0) for its whole setup, which the handler skips.
                g_trackedMaps[i].base.store(0, std::memory_order_release);
                g_trackedMaps[i].end.store(0, std::memory_order_release);
                g_trackedMaps[i].lifetimeId = 0;
                if (end > base) {
                    mprotect(reinterpret_cast<void*>(base), end - base, PROT_READ | PROT_WRITE);
                }
                return;
            }
        }

        // Every live slot, restored writable and freed. Test teardown only: without it a
        // cleared fixture would leave its shadow pages PROT_READ behind, and the next
        // fixture writing the recycled heap would fault into ranges whose owner is gone.
        void UntrackAllWriteMaps() {
            for (SizeT i = 0; i < kMaxTrackedMaps; ++i) {
                const uintptr_t base = g_trackedMaps[i].base.load(std::memory_order_acquire);
                if (base <= kSlotSettingUp) continue;
                const uintptr_t end = g_trackedMaps[i].end.load(std::memory_order_acquire);
                g_trackedMaps[i].base.store(0, std::memory_order_release);
                g_trackedMaps[i].end.store(0, std::memory_order_release);
                g_trackedMaps[i].lifetimeId = 0;
                if (end > base) {
                    mprotect(reinterpret_cast<void*>(base), end - base, PROT_READ | PROT_WRITE);
                }
            }
        }

        TrackedWriteMap* TrackedMapFor(Uint64 lifetimeId) {
            for (SizeT i = 0; i < kMaxTrackedMaps; ++i) {
                if (g_trackedMaps[i].lifetimeId == lifetimeId &&
                    g_trackedMaps[i].base.load(std::memory_order_acquire) > kSlotSettingUp) {
                    return &g_trackedMaps[i];
                }
            }
            return nullptr;
        }

    } // namespace

    PersistentMapTracker& PersistentMapTracker::Instance() {
        // Leaked on purpose, once, like every other role-local singleton (ID-8): a buffer's
        // destructor runs from exit handlers after this TU's globals would already be gone,
        // and it calls Forget().
        static PersistentMapTracker* instance = new PersistentMapTracker{};
        return *instance;
    }

    Uint64 PersistentMapTracker::BlockBytes() {
        return static_cast<Uint64>(MG_Config::Ipc.PersistentBlockKb) * 1024ull;
    }

    Bool PersistentMapTracker::PushIsArmed() {
        return MG_Config::Transport != MG_Config::TransportMode::Monolith;
    }

    Bool PersistentMapTracker::OnServerRole() { return Server::ServerLoop::OnApplyThread(); }

    // SyncPersistentMappedRange's early-out chain (BufferObject.cpp:341-353), in its order,
    // read as a membership test. Every line here has a line there; if one of them moves, the
    // unit case that drives both against each other is what says so.
    Bool PersistentMapTracker::IsLivePersistentMap(const BufferObject& buffer) {
        if (!buffer.IsMapped()) return false;
        // GPU-resident: the application already wrote into coherent GPU memory and there is
        // nothing to ship. At tier T2 this arm is unreachable - MapPersistent declines - but
        // the predicate must still read the chain, not the tier: a build that reaches T0/T1
        // later must see this row answer for itself.
        if (buffer.IsBackendPersistentMapped()) return false;
        const auto access = buffer.GetMappingAccess();
        if (!(access & BufferMappingAccessBit::Persistent)) return false;
        if (!(access & BufferMappingAccessBit::Write)) return false;
        // FLUSH_EXPLICIT: the application promises to announce its own writes with
        // glFlushMappedBufferRange, which already crosses as resource_flush_range. Pushing
        // here as well would ship the same bytes twice and take the upload-shape decision
        // away from the side that pays for it.
        if (access & BufferMappingAccessBit::FlushExplicit) return false;
        const auto range = buffer.GetMappedRange();
        if (range.start >= range.end) return false;
        return true;
    }

    void PersistentMapTracker::NoteMapStateChanged(BufferObject& buffer) {
        const Uint64 key = buffer.GetLifetimeId();
        // Unaccount the OLD state first: a re-notified member keeps its membership but
        // may move arms (a resize re-registers the tracker), and the epoch skip's veto
        // set must follow the arm the member actually runs on. Erase is a no-op for a
        // member that was never large-untracked.
        m_untrackedMembers.erase(key);
        if (IsLivePersistentMap(buffer)) {
            const auto range = buffer.GetMappedRange();
            // The mprotect arm registers beside the hash arm: tracked buffers are pushed
            // from the fault bitmap, everything else keeps the content scan. A failed
            // registration (handler unavailable, table full, unaligned zero range) quietly
            // leaves the buffer on the hash path.
            TrackWriteMap(key, buffer.MappedData(), static_cast<SizeT>(range.start),
                          static_cast<SizeT>(range.end));
            TrackedWriteMap* slot = TrackedMapFor(key);
            m_livePersistentMaps[key] = MemberEntry{&buffer, slot};
            if (slot == nullptr) m_untrackedMembers[key] = 1;
            return;
        }
        m_livePersistentMaps.erase(key);
        UntrackWriteMap(key);
    }

    void PersistentMapTracker::Forget(const BufferObject& buffer) {
        const Uint64 key = buffer.GetLifetimeId();
        m_untrackedMembers.erase(key);
        m_livePersistentMaps.erase(key);
        m_blockHashes.erase(key);
        UntrackWriteMap(key);
    }

    void PersistentMapTracker::ClearForTest() {
        m_livePersistentMaps.clear();
        m_blockHashes.clear();
        m_untrackedMembers.clear();
        UntrackAllWriteMaps();
        m_lastFaultEpoch = 0;
        ResetCountersForTest();
    }

    void PersistentMapTracker::PushBlocksFor(BufferObject& buffer) {
        if (!PushIsArmed()) return;
        if (OnServerRole()) {
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"PushBlocksFor\"} - the persistent-map "
                    "producer belongs to the client; the server consumes transported bytes");
            std::abort();
        }
        PushBlocksForChecked(buffer);
    }

    void PersistentMapTracker::PushBlocksForChecked(BufferObject& buffer) {
        // Re-checked rather than trusted. The set is maintained at five events and a sixth
        // one arriving without a NoteMapStateChanged would otherwise push a buffer whose
        // shadow has been released - an adopted store's Bytes() is the GPU map, and reading
        // it as if it were the shadow is how a "conservative" push turns into a fault.
        if (!IsLivePersistentMap(buffer)) {
            Forget(buffer);
            return;
        }
        const Uint64 blockBytes = BlockBytes();
        // 0 IS THE NEGATIVE CONTROL, NOT "unlimited" (E3(a)). Pushing one whole-span block
        // here would make the control green for the wrong reason - it has to disable the
        // push, so that PersistentCoherentMapScenario draws the last uploaded bytes and goes
        // red exactly the way an unpushed map does.
        //
        // AND IT SAYS SO, ONCE. Until now this was a silent `return`, so E3(a)'s red could
        // only ever be the scenario's pixel assertion and the control had no way to tell "the
        // push was disabled" apart from "the push was never armed, or never reached, or the
        // knob never got here" (joint-v1.md §3: "There is no Fatal for block size zero";
        // ID-65 assigns the line to x2). The control now requires BOTH: the pixel red AND
        // this line in the entry's own private log. It is MGLOG_W and not a Fatal because 0
        // is a legal configured value whose whole purpose is to keep running with the push
        // off; aborting here would turn every E3(a) entry into a subprocess abort and take
        // the pixel evidence with it.
        if (blockBytes == 0) {
            if (!m_blockZeroAnnounced) {
                m_blockZeroAnnounced = true;
                MGLOG_W("MGPipe: persistent-map push disabled - MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0 "
                        "is exit gate E3(a)'s NEGATIVE CONTROL, not 'unlimited': a live "
                        "persistent WRITE mapping's dirty blocks are NOT being pushed, so the "
                        "server draws whatever bytes last crossed by some other route. A lane "
                        "that stays green with this set is not getting its pixels from the push");
            }
            return;
        }

        const auto range = buffer.GetMappedRange();
        const Uint64 begin = static_cast<Uint64>(range.start);
        const Uint64 end = static_cast<Uint64>(range.end);
        // THE MPROTECT ARM. A tracked buffer's push is driven by the fault bitmap: one
        // 64 KB block per block that contains at least one faulted page, shipped with
        // the whole machinery of every other arm (serial bump, HasDefinedContent,
        // wire record), then the page is cleared and re-armed read-only. No scan, no
        // hash, no per-draw cost for buffers the application did not touch. The two
        // partial-page EDGE ranges beside the page-aligned interior (see TrackWriteMap's
        // inward alignment) are never protected, so nothing tells us they changed -
        // their blocks ride along on every push, at most two.
        TrackedWriteMap* tracked = nullptr;
        if (const auto it = m_livePersistentMaps.find(buffer.GetLifetimeId());
            it != m_livePersistentMaps.end() && it->second.tracked != nullptr &&
            it->second.tracked->lifetimeId == buffer.GetLifetimeId()) {
            tracked = it->second.tracked;
        }
        if (tracked != nullptr) {
            const uintptr_t trackedBase = tracked->base.load(std::memory_order_acquire);
            const uintptr_t trackedEnd = tracked->end.load(std::memory_order_acquire);
            const uintptr_t shadowBase = reinterpret_cast<uintptr_t>(buffer.MappedData());
            if (trackedBase != 0 && trackedEnd > trackedBase && shadowBase != 0) {
                const Uint64 interiorBegin =
                    static_cast<Uint64>(trackedBase - shadowBase); // >= begin
                const Uint64 interiorEnd =
                    static_cast<Uint64>(trackedEnd - shadowBase); // <= end
                // Pushes are made in ASCENDING offset order, and `pushedUpTo` trims each
                // span against the one before it: edge spans, duplicate pages inside one
                // 64 KB block and faulted blocks overlapping an edge all collapse to one
                // wire record per covered interval. The edge spans cover their WHOLE
                // containing block rather than just the edge bytes, so a head edge and
                // the faulted pages of the same first block merge into the one block
                // push the hash arm would have made - the wire shape stays block-shaped.
                // And an edge span ships ONLY when its bytes changed since the last push
                // (the struct comment says why unconditional was measured and rejected):
                // no faults happen on unprotected bytes, so the hash is the only change
                // detector there is for them.
                Uint64 pushedUpTo = begin;
                const auto pushSpan = [&](Uint64 spanBegin, Uint64 spanEnd) {
                    if (spanBegin < pushedUpTo) spanBegin = pushedUpTo;
                    if (spanBegin >= spanEnd) return;
                    buffer.PushMappedSpanBlock(static_cast<SizeT>(spanBegin),
                                               static_cast<SizeT>(spanEnd - spanBegin));
                    ++m_blocksPushed;
                    m_bytesPushed += spanEnd - spanBegin;
                    pushedUpTo = spanEnd;
                };
                if (interiorBegin > begin) {
                    const Uint64 edgeHash = XXH3_64bits(
                        reinterpret_cast<const Uint8*>(shadowBase) + begin,
                        static_cast<size_t>(interiorBegin - begin));
                    if (edgeHash != tracked->edgeHashHead) {
                        tracked->edgeHashHead = edgeHash;
                        const Uint64 headBlockEnd = (begin / blockBytes + 1) * blockBytes;
                        pushSpan(begin, headBlockEnd < end ? headBlockEnd : end);
                    }
                }
                for (SizeT wordIndex = 0; wordIndex < (tracked->pageCount + 63) / 64; ++wordIndex) {
                    Uint64 word = tracked->pageBits[wordIndex].load(std::memory_order_relaxed);
                    while (word != 0) {
                        const SizeT bit = static_cast<SizeT>(__builtin_ctzll(word));
                        word &= word - 1;
                        const SizeT pageIndex = (wordIndex << 6) + bit;
                        // Clear the bit FIRST, per the order the correctness argument
                        // needs, but batch the re-arm: bits are visited in ascending
                        // page order, so a contiguous run of faulted pages is one
                        // mprotect instead of one per page. The order still holds for
                        // every page of the run - clear, then re-arm, then read - and a
                        // write in the clear-to-re-arm window lands before the run's
                        // reads, so it is inside the shipped bytes.
                        tracked->pageBits[wordIndex].fetch_and(~(1ull << bit),
                                                               std::memory_order_relaxed);
                        SizeT runEnd = pageIndex + 1;
                        while (word != 0) {
                            const SizeT nextBit = static_cast<SizeT>(__builtin_ctzll(word));
                            if ((wordIndex << 6) + nextBit != runEnd) break;
                            word &= word - 1;
                            tracked->pageBits[wordIndex].fetch_and(~(1ull << nextBit),
                                                                   std::memory_order_relaxed);
                            ++runEnd;
                        }
                        mprotect(reinterpret_cast<void*>(trackedBase + (pageIndex << kPageShift)),
                                 (runEnd - pageIndex) << kPageShift, PROT_READ);
                        for (SizeT runPage = pageIndex; runPage < runEnd; ++runPage) {
                            // The interior was page-aligned INSIDE the mapped range at
                            // registration, so a faulted page's offset in buffer space is
                            // never negative; its containing block can still start before
                            // `begin` (a block is 64 KB, a page is 4 KB), and the clamp is
                            // what keeps the push inside [begin, end).
                            const Uint64 pageOffset =
                                (trackedBase + (runPage << kPageShift)) - shadowBase;
                            const Uint64 blockFloor = (pageOffset / blockBytes) * blockBytes;
                            const Uint64 blockStart =
                                blockFloor < begin ? begin : blockFloor;
                            const Uint64 blockEnd =
                                (end - blockStart) < blockBytes ? end : blockStart + blockBytes;
                            pushSpan(blockStart, blockEnd);
                        }
                    }
                }
                if (interiorEnd < end) {
                    const Uint64 edgeHash = XXH3_64bits(
                        reinterpret_cast<const Uint8*>(shadowBase) + interiorEnd,
                        static_cast<size_t>(end - interiorEnd));
                    if (edgeHash != tracked->edgeHashTail) {
                        tracked->edgeHashTail = edgeHash;
                        const Uint64 tailBlockStart = ((end - 1) / blockBytes) * blockBytes;
                        pushSpan(tailBlockStart > begin ? tailBlockStart : begin, end);
                    }
                }
                return;
            }
        }
        // THE DIRTY-BLOCK PUSH (MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS). The whole-range push
        // below is what the numbers made untenable on a real workload: every verb shipped the
        // entire mapped range (measured ~25 MB/frame in a streamed world - one record, one
        // staging copy, one server-side adoption and one barrier wait per 64 KB block). The
        // app writes a small fraction of the range per frame, so each block is hashed against
        // the shadow the last push saw and only changed blocks cross. Semantics are the
        // push's own: a changed byte still reaches the server before the next consuming verb,
        // and a hash of 0 is "unknown" (XXH64 of zero bytes is not zero), so a fresh state
        // pushes everything once. API writes that bypass the map (NotifySubData) show up as a
        // one-frame redundant push of the touched block, then the cache catches up - correct
        // in both directions.
        BlockHashState* state = nullptr;
        const Uint8* shadow = nullptr;
        if (MG_Config::Ipc.PersistentHashSuppress != 0) {
            shadow = buffer.MappedData();
            if (shadow != nullptr) {
                state = &m_blockHashes[buffer.GetLifetimeId()];
                const Uint64 blockCount = (end - begin + blockBytes - 1) / blockBytes;
                // A remap, a resize or a block-size change invalidates the layout the
                // hashes were taken against: reset, which forces one full push - the
                // conservative answer for a new layout.
                if (state->begin != begin || state->end != end || state->blockBytes != blockBytes ||
                    state->hashes.size() != blockCount) {
                    *state = {};
                    state->begin = begin;
                    state->end = end;
                    state->blockBytes = blockBytes;
                    state->hashes.assign(static_cast<SizeT>(blockCount), 0);
                }
            }
        }
        SizeT blockIndex = 0;
        for (Uint64 at = begin; at < end; at += blockBytes, ++blockIndex) {
            const Uint64 length = (end - at) < blockBytes ? (end - at) : blockBytes;
            if (state != nullptr) {
                const Uint64 h = XXH3_64bits(shadow + at, static_cast<size_t>(length));
                if (h == state->hashes[blockIndex]) continue;
                state->hashes[blockIndex] = h;
            }
            buffer.PushMappedSpanBlock(static_cast<SizeT>(at), static_cast<SizeT>(length));
            ++m_blocksPushed;
            m_bytesPushed += length;
        }
    }

    void PersistentMapTracker::PushAllMembers() {
        if (!PushIsArmed()) return;
        if (OnServerRole()) {
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"PushAllMembers\"} - the persistent-map "
                    "producer belongs to the client; the server consumes transported bytes");
            std::abort();
        }
        if (m_livePersistentMaps.empty()) return;
        // Copied out first: PushBlocksFor can erase its own entry (a member that stopped
        // being one), and ska::flat_hash_map invalidates on erase.
        Vector<BufferObject*> members;
        members.reserve(m_livePersistentMaps.size());
        for (const auto& entry : m_livePersistentMaps) members.push_back(entry.second.buffer);
        for (BufferObject* buffer : members) {
            if (buffer != nullptr) PushBlocksForChecked(*buffer);
        }
    }

    void PersistentMapTracker::PushDrawConsumers() {
        if (!PushIsArmed()) return;
        if (OnServerRole()) {
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"PushDrawConsumers\"} - the persistent-map "
                    "producer belongs to the client; the server consumes transported bytes");
            std::abort();
        }
        if (m_livePersistentMaps.empty()) return;
        // THE EPOCH SKIP. Interior dirtiness can only arrive as a write fault, and the
        // handler bumps the fault epoch for every one it answers before it unprotects;
        // an unmoved epoch therefore proves no interior byte of any tracked map changed
        // since the last walk. The skip still serves a tracked member's partial-page
        // edge spans right here, per member, with no binding state read at all - they
        // are never protected, so their two spans are hashed per push. An untracked
        // (hash-arm) member vetoes the skip outright: only the binding walk knows
        // whether this draw consumes it, and scanning every untracked member on faith
        // measured strictly worse than the walk (VD12, on device). With none present
        // and the epoch unmoved, nearly every draw in steady flight costs a handful of
        // 4 KB edge hashes.
        const Uint64 epoch = g_faultEpoch.load(std::memory_order_acquire);
        if (m_untrackedMembers.empty() && epoch == m_lastFaultEpoch) {
            for (const auto& entry : m_livePersistentMaps) {
                BufferObject* member = entry.second.buffer;
                if (member == nullptr) continue;
                // The cached slot pointer, validated against the slot's own lifetimeId:
                // a slot retired since the cache was written fails the test, as does one
                // re-claimed by a different buffer.
                const TrackedWriteMap* tracked = entry.second.tracked;
                if (tracked != nullptr && tracked->lifetimeId == member->GetLifetimeId() &&
                    tracked->hasEdges) {
                    PushBlocksForChecked(*member);
                }
            }
            return;
        }
        m_lastFaultEpoch = epoch;
        const auto& ctx = MG_State::pGLContext;
        if (ctx == nullptr) return;

        // The buffers the upcoming draw or dispatch can read, and only those. Every one is
        // named by the frontend's own binding state, read on this (the GL) thread - VAO
        // attribute and element buffers, the indexed binding points of every indexed
        // target, and the indirect / parameter slots. Client-sourced attributes (no
        // BufferObject) live on this side already and need no push.
        Vector<BufferObject*> consumers;
        // One entry per buffer, not per binding point: an arena bound to six attribute
        // slots would otherwise get its predicate re-check, bitmap walk and edge hashes
        // run six times per draw.
        const auto consume = [&consumers](BufferObject* buffer) {
            if (buffer == nullptr) return;
            for (const BufferObject* existing : consumers) {
                if (existing == buffer) return;
            }
            consumers.push_back(buffer);
        };
        if (const auto& vao = ctx->GetBoundVertexArray()) {
            for (Uint i = 0; i < MG_Pipe::kMGPipeMaxVertexAttribs; ++i) {
                const auto& attrib = vao->GetAttribute(i);
                if (attrib.Enabled && attrib.Buffer != nullptr) {
                    consume(attrib.Buffer.get());
                }
            }
            if (const auto& element = vao->GetIndexBufferBindingSlot().GetBoundObject()) {
                consume(element.get());
            }
        }
        static constexpr BufferTarget kIndexedTargets[] = {
                BufferTarget::Uniform, BufferTarget::ShaderStorage, BufferTarget::AtomicCounter,
                BufferTarget::TransformFeedback, BufferTarget::CopyRead, BufferTarget::CopyWrite,
                BufferTarget::Query, BufferTarget::Texture};
        for (const BufferTarget target : kIndexedTargets) {
            const SizeT count = ctx->GetBufferBindingPointCount(target);
            for (SizeT i = 0; i < count; ++i) {
                if (const auto& bound = ctx->GetBufferBindingPoint(target, i).GetBoundObject()) {
                    consume(bound.get());
                }
            }
        }
        static constexpr BufferTarget kSlotTargets[] = {BufferTarget::DrawIndirect,
                                                        BufferTarget::DispatchIndirect,
                                                        BufferTarget::Parameter};
        for (const BufferTarget target : kSlotTargets) {
            if (const auto& bound = ctx->GetBufferBindingSlot(target).GetBoundObject()) {
                consume(bound.get());
            }
        }
        // BUFFER TEXTURES (the cloud renderer's shape, and the gap the binding-point walk
        // cannot see): a texture attached to a buffer with glTexBuffer is read by the draw
        // through a texture unit, never through a buffer binding point. Walking the touched
        // units and following a buffer-backed texture to its backing buffer is the only way
        // those bytes earn their push - without this arm a persistent-mapped TexBuffer is
        // only refreshed by the read-only verbs' whole push (once per present), which is
        // exactly the per-frame-cadence cloud twitch this comment is written against.
        const Int maxUnit = ctx->GetMaxTouchedTextureUnit();
        for (Int unit = 0; unit <= maxUnit; ++unit) {
            const auto& texture =
                ctx->GetTextureUnitObject(unit).GetBindingSlot(TextureTarget::TextureBuffer).GetBoundObject();
            if (texture == nullptr || texture->GetStorageType() != TextureStorageType::Buffer) {
                continue;
            }
            auto* bufferTexture = static_cast<MG_State::GLState::TextureObjectBuffer*>(texture.get());
            if (const auto& backing = bufferTexture->GetBufferBindingSlot().GetBoundObject()) {
                consume(backing.get());
            }
        }
        if (consumers.empty()) return;
        for (BufferObject* buffer : consumers) {
            if (buffer != nullptr && m_livePersistentMaps.count(buffer->GetLifetimeId()) != 0) {
                PushBlocksForChecked(*buffer);
            }
        }
    }

    void PushPersistentMapsBeforeVerb() {
        PersistentMapTracker::Instance().PushAllMembers();
    }

    Bool AdoptTierIsEmulate() {
        const Uint32 tier = MG_Config::Ipc.AdoptTier;
        if (tier == 2) return true;
        // A NAMED refusal, not a silent fall back to T2. T0 (a real cross-process shared
        // mapping) and T1 (a server-side staging map) are P11's, and the reason the knob
        // parses them today is that the negative control needs a spelling before the thing
        // it controls exists. Falling back would make `MOBILEGL_IPC_ADOPT_TIER=0` look like
        // a working T0 run and silently produce pmap bytes it must not produce.
        // 0 and 1 are the two CONTRACT §5 promises - a real cross-process shared mapping and a
        // server-side staging map - and they name P11. Anything else is not a tier at all, and
        // saying "P11 implements it" of a 7 would be a lie the operator then repeats. Both die
        // here rather than at parse, which is late: the abort lands at the first
        // map_persistent, so a mis-set run gets through EGL bring-up and a frame of setup
        // first. Moving it to the parse means a knob-validity rule in ConfigLoader, which is
        // c0's file; filed for the integrator rather than taken here.
        if (tier <= 1) {
            MGLOG_F("MGPipe: MOBILEGL_IPC_ADOPT_TIER=%u names adoption tier T%u, which P11 implements "
                    "and P5 does not; P5 runs at T2 (emulate) only.",
                    static_cast<unsigned>(tier), static_cast<unsigned>(tier));
        } else {
            MGLOG_F("MGPipe: MOBILEGL_IPC_ADOPT_TIER=%u is not an adoption tier; the only values are 0 "
                    "and 1 (P11) and 2 (emulate, the P5 default).",
                    static_cast<unsigned>(tier));
        }
        std::abort();
    }

} // namespace MobileGL::MG_Remote::Client
