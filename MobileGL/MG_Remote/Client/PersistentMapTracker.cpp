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
        // THE PAGE-GRANULAR SHADOW IS THE TRACKER'S PRECONDITION, NOT A COINCIDENCE. Every
        // shadow a split build hands out is SHADOW_ALLOCATION_ALIGNMENT-aligned AND sized to
        // a multiple of it (PipeResource.h, ShadowAllocationBytesFor), so a shadow owns every
        // byte of every page it touches; the outward alignment in TrackWriteMap rests on
        // that, and it is only true when the allocator's unit is a whole number of the
        // tracker's pages.
        static_assert(MG_State::GLState::SHADOW_ALLOCATION_ALIGNMENT % kPageBytes == 0,
                      "the shadow allocator's granule must be a whole number of tracker pages, "
                      "or a page-granular shadow could still end mid-page");
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
        // Bumped by the handler for every write fault it answers, and by TrackWriteMap
        // for every registration (a fresh slot has EVERY page marked, which is a fault of
        // every page as far as the push is concerned). PushDrawConsumers compares it
        // against the GL thread's last-walk value: an unmoved epoch proves no page of any
        // tracked map was marked since the last walk, because a mark can only ever arrive
        // through one of those two events - and the walk drains every marked page of
        // every tracked member, which is what makes the proof worth having. The bump is a
        // RELEASE and the walk's load an ACQUIRE, so the bit the handler set before it
        // bumped is visible to a walk that saw the bump; a chunk builder writing an arena
        // off the GL thread is the ordinary case, not a corner.
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
                // EVERY overlapping slot gets its bit, not just the first: unprotecting
                // after only the first mark would leave a second owner's block permanently
                // un-pushed - the page is writable now, so it never faults again. With
                // page-granular shadows no two tracked ranges can share a page any more
                // (each protected page is one shadow's own), so the loop finds at most one
                // owner; it stays in this form because a fault is rare and the exhaustive
                // scan is what makes the claim checkable rather than assumed.
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
                    // moment the page becomes writable must still see this fault. And it
                    // moves with RELEASE, after the bit: a walk that acquires the moved
                    // epoch is guaranteed to see the bit that goes with it.
                    g_faultEpoch.fetch_add(1, std::memory_order_release);
                    // Un-arm is a raw syscall, deliberately not the libc wrapper's
                    // bookkeeping: the handler's whole job is to get out of the way.
                    mprotect(reinterpret_cast<void*>(address & ~(kPageBytes - 1)),
                             kPageBytes, PROT_READ | PROT_WRITE);
                    return;
                }
            }
            ChainToPreviousSegvHandler(sig, info, ucontext);
        }

        // THE KERNEL'S PAGE MUST BE THE TRACKER'S PAGE. Every ownership argument here -
        // above all "a protected page holds only this shadow's bytes" - is made at
        // kPageBytes granularity, and mprotect protects at the KERNEL's. On a 16 KB-page
        // kernel (Android 15 admits them) a kPageBytes mprotect would silently cover
        // three neighbouring 4 KB quarters that may be foreign memory, which is exactly
        // the process-killer the page-granular shadow exists to keep out. So on such a
        // kernel the arm is not installed at all and every persistent write map takes
        // the hash arm: slower, never unsafe. Said once, not per map - and decided BEFORE
        // g_segvInstalled can read true to anyone: a function-static answers exactly once,
        // on whichever thread asks first, and every other asker waits on that answer, so
        // no caller can pass TrackWriteMap's installed gate while the page size is still
        // being checked.
        Bool KernelPageIsTheTrackerPage() {
            static const Bool answer = [] {
                const long kernelPage = sysconf(_SC_PAGESIZE);
                if (kernelPage == static_cast<long>(kPageBytes)) return true;
                MGLOG_W("MGPipe: persistent-map mprotect tracking unavailable (kernel page %ld "
                        "is not the tracker's %zu); persistent write maps fall back to the hash scan",
                        kernelPage, kPageBytes);
                return false;
            }();
            return answer;
        }

        void InstallWriteFaultHandler() {
            if (!KernelPageIsTheTrackerPage()) return;
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

        // `shadowExtent` is the shadow allocation's own size in bytes as its allocator
        // sized it (BufferObject::ShadowAllocationBytes), or 0 when the caller cannot vouch
        // for it; it is what decides between the two alignments below.
        Bool TrackWriteMap(Uint64 lifetimeId, const Uint8* shadow,
                           SizeT rangeBegin, SizeT rangeEnd, SizeT shadowExtent) {
            if (shadow == nullptr || rangeEnd <= rangeBegin) return false;
            InstallWriteFaultHandler();
            if (!g_segvInstalled.load()) return false;
            // WHICH WAY TO ALIGN IS A QUESTION OF OWNERSHIP, and the reason is a
            // process-killer: a protected page that holds FOREIGN bytes can be written by
            // a thread that does not run our handler - driver and runtime worker threads
            // commonly block every signal - and a fault there with SIGSEGV blocked ends the
            // process on the spot, undebuggable (the ctest-only SmallRing segfault of
            // cb06538c). So a page may be protected only when every byte of it is this
            // shadow's own, and the two alignments are the two ways of guaranteeing that:
            //
            //   OUTWARD, when the shadow is PAGE-GRANULAR: its base is on a page boundary
            //   and its allocation is a whole number of pages (PipeResource.h's allocator
            //   promises both in a split build, and `shadowExtent` is what it promised).
            //   Then every page that intersects the mapped range lies inside
            //   [shadow, shadow + shadowExtent), and every byte of that span belongs to
            //   this allocation and to no other - the heap handed it out as ONE block of
            //   that size, and its own metadata lives before the block, never inside it.
            //   The mapped range's containing pages are protected whole, there are NO
            //   unprotected edge bytes, and a sub-page map is one protected page. The bytes
            //   of a head/tail page that lie OUTSIDE the mapped range are still the
            //   shadow's: only the client's own API paths write them (a SubData /
            //   FlushMappedRange memcpy into the shadow, a readback writeback landing on
            //   the client thread), on threads that run this handler, and such a fault is
            //   answered like any interior one - the push then clamps to [begin, end), so
            //   at worst one block ships redundantly, and the byte itself crossed by its own
            //   record.
            //
            //   INWARD, for anything else (an unaligned base, an extent the caller could not
            //   vouch for, a range running past the extent it was told): only pages FULLY
            //   inside the mapped range are protected, which are the shadow's own by
            //   construction, and the head/tail partial pages stay writable. Nothing tells
            //   us they changed, so they are hashed per push (the edge hashes) - which was
            //   8.5% of the client thread at VD12 on every draw for every live map, and
            //   the reason the outward arm exists. A sub-page range still registers here
            //   with an EMPTY interior and hasEdges set rather than falling to the hash
            //   arm, where as an "untracked" member it would veto the epoch skip for every
            //   tracked buffer in the process (measured on device); end is clamped up to
            //   base so the [base, end) arithmetic never underflows.
            const uintptr_t shadowBase = reinterpret_cast<uintptr_t>(shadow);
            const uintptr_t rawBegin = shadowBase + rangeBegin;
            const uintptr_t rawEnd = shadowBase + rangeEnd;
            constexpr uintptr_t kPageMask = ~static_cast<uintptr_t>(kPageBytes - 1);
            const Bool pageGranular = (shadowBase & (kPageBytes - 1)) == 0 && shadowExtent != 0 &&
                                      (shadowExtent & (kPageBytes - 1)) == 0 && rangeEnd <= shadowExtent;
            uintptr_t base = 0;
            uintptr_t end = 0;
            Bool outward = pageGranular;
            if (outward) {
                base = rawBegin & kPageMask;
                end = (rawEnd + kPageBytes - 1) & kPageMask;
                // Already implied by rangeEnd <= shadowExtent and the extent's own page
                // granularity; the clamp is kept because it is the line the ownership
                // argument rests on, and it must not depend on a caller's arithmetic.
                if (end > shadowBase + shadowExtent) end = shadowBase + shadowExtent;
                // WIDENING NEVER DEMOTES A MAP THE INWARD ARM COULD HOLD. Outward covers up
                // to one page more than inward does for the same range (both ends rounded
                // away instead of towards), so a map sitting on the bitmap's capacity that
                // was tracked inward before this round would otherwise fall off the tracker
                // altogether - onto the hash arm, where as an untracked member it vetoes the
                // epoch skip for every buffer in the process. It keeps the inward arm and
                // its two hashed edges instead: the old cost, on the one ~256 MB map that
                // can pay it, and nothing else changes.
                if (((end - base) >> kPageShift) > kMaxTrackedPages) outward = false;
            }
            if (!outward) {
                base = (rawBegin + kPageBytes - 1) & kPageMask;
                end = rawEnd & kPageMask;
                if (end < base) end = base;
            }
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
                // Edges are UNPROTECTED mapped bytes: the range starts before the first
                // protected page or ends after the last. Outward alignment never leaves
                // any (base <= rawBegin, end >= rawEnd); inward leaves one per unaligned end.
                g_trackedMaps[i].hasEdges = base > rawBegin || end < rawEnd;
                // EVERY BIT SET, NOT ZEROED - the same "fresh state pushes everything
                // once" rule the hash arm keeps: the server has never seen these bytes,
                // so the first push must ship the whole interior, not just the pages the
                // application happened to write first. The first push then clears and
                // re-arms page by page, and the set-bit read the fault discriminator
                // does on a pre-first-push fault is answered once and unprotected -
                // harmless, and the invariant (bit set <=> writable) holds from the
                // first push onward. The last word is masked: a set bit past pageCount
                // would re-arm a page outside the protected span - past the shadow's
                // extent, a foreign page, which is the process-killer both alignments
                // exist to keep out.
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
                // AND THE EPOCH MOVES, as it would for a fault of every page: the bits just
                // set are exactly what a fault leaves behind, and the epoch is the only thing
                // PushDrawConsumers reads before deciding it has nothing to drain. Without
                // this a member registered between two draws sits fully marked and fully
                // protected until something else faults - "first push ships everything
                // once" would then be true only of the read-only verbs' whole push.
                g_faultEpoch.fetch_add(1, std::memory_order_release);
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

        // Whether any page of a live slot is marked - faulted since its last push, or never
        // pushed since registration. Read on the GL thread by the consumer walk only (a
        // walk runs when the epoch moved, never per draw in steady state), and it costs
        // one word load per 64 pages: 40 loads for a 10 MB arena. A slot with an empty
        // interior (the inward fallback's sub-page case) has no words and no marks.
        Bool AnyPageMarked(const TrackedWriteMap& slot) {
            const SizeT words = (slot.pageCount + 63) / 64;
            for (SizeT w = 0; w < words; ++w) {
                if (slot.pageBits[w].load(std::memory_order_relaxed) != 0) return true;
            }
            return false;
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

    // P5d round 3 (package D): the body this forwards to is now one relaxed load of the apply
    // thread's key and one thread-pointer compare (ServerLoop.h), instead of a function-static
    // guard, two acquire loads and an out-of-line thread::id comparison. It stays out of line
    // here for WireTables.cpp's reason beside RunsAsTheServerRole: inlining it would put a
    // server header inside PersistentMapTracker.h, which MG_State's BufferObject.cpp includes.
    Bool PersistentMapTracker::OnServerRole() { return Server::ServerLoop::OnApplyThread(); }

    // SyncPersistentMappedRange's early-out chain (BufferObject.cpp:341-353), in its order,
    // read as a membership test. Every line here has a line there; if one of them moves, the
    // unit case that drives both against each other is what says so.
    //
    // THE ORDER IS THE CONTRACT, so package D did not touch it even though the first row is the
    // only guarded accessor in the chain (IsMapped -> RefuseLegacyBufferArmFromApplyThread) and
    // moving it last would have saved a guard on the members that fail rows 2-6. It would also
    // have stopped being the same chain as the one in BufferObject, which is the property this
    // predicate exists to have. Item 1 made that guard two loads instead, which is cheaper than
    // the reorder would have been and costs nothing in meaning.
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
        m_edgedMembers.erase(key);
        if (IsLivePersistentMap(buffer)) {
            const auto range = buffer.GetMappedRange();
            // The mprotect arm registers beside the hash arm: tracked buffers are pushed
            // from the fault bitmap, everything else keeps the content scan. A failed
            // registration (handler unavailable, table full, unaligned zero range) quietly
            // leaves the buffer on the hash path. The shadow's own extent goes along so
            // the tracker can align outward (TrackWriteMap): a live member's MappedData()
            // IS the shadow vector's storage - the predicate's IsBackendPersistentMapped
            // row keeps every adopted store out - so the extent describes that pointer.
            TrackWriteMap(key, buffer.MappedData(), static_cast<SizeT>(range.start),
                          static_cast<SizeT>(range.end), buffer.ShadowAllocationBytes());
            TrackedWriteMap* slot = TrackedMapFor(key);
            m_livePersistentMaps[key] = MemberEntry{&buffer, slot};
            if (slot == nullptr) {
                m_untrackedMembers[key] = 1;
            } else if (slot->hasEdges) {
                m_edgedMembers[key] = 1;
            }
            return;
        }
        m_livePersistentMaps.erase(key);
        UntrackWriteMap(key);
    }

    void PersistentMapTracker::Forget(const BufferObject& buffer) {
        const Uint64 key = buffer.GetLifetimeId();
        m_untrackedMembers.erase(key);
        m_edgedMembers.erase(key);
        m_livePersistentMaps.erase(key);
        m_blockHashes.erase(key);
        UntrackWriteMap(key);
    }

    void PersistentMapTracker::ClearForTest() {
        m_livePersistentMaps.clear();
        m_blockHashes.clear();
        m_untrackedMembers.clear();
        m_edgedMembers.clear();
        UntrackAllWriteMaps();
        m_lastFaultEpoch = 0;
        ResetCountersForTest();
    }

    Bool PersistentMapTracker::MprotectArmAvailableForTest() {
        InstallWriteFaultHandler();
        return g_segvInstalled.load();
    }

    const TrackedWriteMap* PersistentMapTracker::TrackedSlotForTest(const BufferObject& buffer) {
        return TrackedMapFor(buffer.GetLifetimeId());
    }

    const TrackedWriteMap* PersistentMapTracker::TrackForTest(Uint64 lifetimeId, const Uint8* shadow,
                                                              SizeT rangeBegin, SizeT rangeEnd,
                                                              SizeT shadowExtent) {
        TrackWriteMap(lifetimeId, shadow, rangeBegin, rangeEnd, shadowExtent);
        return TrackedMapFor(lifetimeId);
    }

    void PersistentMapTracker::UntrackForTest(Uint64 lifetimeId) { UntrackWriteMap(lifetimeId); }

    Uint64 PersistentMapTracker::FaultEpochForTest() {
        return g_faultEpoch.load(std::memory_order_acquire);
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
        // hash, no per-draw cost for buffers the application did not touch. A
        // page-granular shadow's protected span is the mapped range's containing pages
        // (TrackWriteMap's outward alignment), so it can start BEFORE `begin` and end
        // AFTER `end`, and the block clamp below is what keeps every push inside
        // [begin, end). Only the inward fallback has the two unprotected partial-page
        // EDGE ranges that nothing faults for; those are hashed per push, at most two.
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
                // The protected span in buffer offsets: <= begin / >= end when aligned
                // outward, >= begin / <= end when aligned inward. Both are read below only
                // to find the inward fallback's edges; the page loop never needs them.
                const Uint64 interiorBegin = static_cast<Uint64>(trackedBase - shadowBase);
                const Uint64 interiorEnd = static_cast<Uint64>(trackedEnd - shadowBase);
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
                // detector there is for them. The two conditions below are self-gating:
                // under outward alignment interiorBegin <= begin and interiorEnd >= end,
                // so neither hash runs for a page-granular shadow - every shadow in steady
                // state - and that, not a flag, is what took the per-draw XXH3 off the
                // client thread. hasEdges is the same fact recorded at registration for
                // the epoch skip's benefit, not a second gate here.
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
                            // A faulted page's offset in buffer space is never negative
                            // (the span starts at or after the shadow base either way),
                            // but under outward alignment the head page can start before
                            // `begin` and the tail page runs past `end`; and a containing
                            // block starts before `begin` in any case (a block is 64 KB, a
                            // page is 4 KB). The clamp is what keeps the push inside
                            // [begin, end): a write to the shadow's own bytes outside the
                            // mapped range faults here too and ships, at worst, one block
                            // of the range redundantly.
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
        // THE EPOCH SKIP, AND WHAT IT RESTS ON. A page of a tracked map is marked by
        // exactly two events - the handler answering a write fault, and TrackWriteMap
        // setting every bit of a fresh slot - and both move the fault epoch after the
        // mark. An unmoved epoch therefore proves no page of any tracked map was marked
        // since the last walk. That is only the whole answer if the last walk DRAINED
        // every marked page there was, and it does: the walk below pushes every tracked
        // member with a marked page whether or not this draw consumes it (see there).
        // Before this round the skip's per-member edge service did that drain by
        // accident - it ran PushBlocksForChecked, bitmap walk included, on every member
        // with edges, and nearly every member had edges; with page-granular shadows no
        // member has, so the drain has to be where the argument says it is. With every
        // byte of every tracked map protected (TrackWriteMap's outward alignment) the
        // skip then costs one atomic load and two empties. The one exception is a
        // member on the inward fallback, whose unprotected edge spans are served right
        // here, per such member, with no binding state read at all - the m_edgedMembers
        // set is exactly those and is empty in steady state. An untracked (hash-arm)
        // member vetoes the skip outright: only the binding walk knows whether this draw
        // consumes it, and scanning every untracked member on faith measured strictly
        // worse than the walk (VD12, on device).
        const Uint64 epoch = g_faultEpoch.load(std::memory_order_acquire);
        if (m_untrackedMembers.empty() && epoch == m_lastFaultEpoch) {
            if (!m_edgedMembers.empty()) {
                // Keys copied out first: PushBlocksForChecked can Forget() a member that
                // stopped being one, and ska::flat_hash_map invalidates on erase.
                Vector<Uint64> edged;
                edged.reserve(m_edgedMembers.size());
                for (const auto& entry : m_edgedMembers) edged.push_back(entry.first);
                for (const Uint64 key : edged) {
                    const auto it = m_livePersistentMaps.find(key);
                    if (it == m_livePersistentMaps.end() || it->second.buffer == nullptr) continue;
                    // The cached slot pointer, validated against the slot's own
                    // lifetimeId: a slot retired since the cache was written fails the
                    // test, as does one re-claimed by a different buffer.
                    const TrackedWriteMap* tracked = it->second.tracked;
                    if (tracked != nullptr && tracked->lifetimeId == key && tracked->hasEdges) {
                        PushBlocksForChecked(*it->second.buffer);
                    }
                }
            }
            return;
        }
        // Taken BEFORE the drain, on purpose: a fault landing while the drain runs moves
        // the epoch past this value, and the next draw walks again for it.
        m_lastFaultEpoch = epoch;

        // The buffers this walk pushes: every tracked member with a marked page, and the
        // buffers the upcoming draw or dispatch can read. One entry per buffer, not per
        // binding point: an arena bound to six attribute slots would otherwise get its
        // predicate re-check, bitmap walk and edge hashes run six times per draw.
        Vector<BufferObject*> consumers;
        const auto consume = [&consumers](BufferObject* buffer) {
            if (buffer == nullptr) return;
            for (const BufferObject* existing : consumers) {
                if (existing == buffer) return;
            }
            consumers.push_back(buffer);
        };

        // THE DRAIN THE SKIP RESTS ON: every tracked member with a marked page, whether
        // or not this draw consumes it. The epoch says "some page of some tracked map was
        // marked" and nothing about which; pushing only the consumers would advance the
        // watermark past a mark on a buffer the draw did not bind, and that buffer would
        // then never move the epoch again (its faulted page is writable, its bit is set,
        // nothing re-arms it) - the next draw that DOES consume it would skip, and the
        // server would draw stale bytes. Such a push is bitmap-driven, so it ships only
        // the faulted blocks, not the range: the cost the consumer filter exists to avoid
        // is the whole-range rescan of the hash arm, which no tracked member pays. The
        // cached slot pointer is validated against the slot's own lifetimeId: a slot
        // retired since the cache was written fails the test, as does one re-claimed by
        // a different buffer.
        for (const auto& entry : m_livePersistentMaps) {
            const TrackedWriteMap* tracked = entry.second.tracked;
            if (tracked != nullptr && tracked->lifetimeId == entry.first && AnyPageMarked(*tracked)) {
                consume(entry.second.buffer);
            }
        }

        // The buffers the upcoming draw or dispatch can read, and only those. Every one is
        // named by the frontend's own binding state, read on this (the GL) thread - VAO
        // attribute and element buffers, the indexed binding points of every indexed
        // target, and the indirect / parameter slots. Client-sourced attributes (no
        // BufferObject) live on this side already and need no push. This is the half of
        // the walk that serves the hash-arm (untracked) members and the inward fallback's
        // edges: nothing but a binding can say whether this draw reads them.
        if (const auto& ctx = MG_State::pGLContext) {
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
            // BUFFER TEXTURES (the cloud renderer's shape, and the gap the binding-point
            // walk cannot see): a texture attached to a buffer with glTexBuffer is read by
            // the draw through a texture unit, never through a buffer binding point.
            // Walking the touched units and following a buffer-backed texture to its
            // backing buffer is the only way those bytes earn their push - without this
            // arm a persistent-mapped TexBuffer is only refreshed by the read-only verbs'
            // whole push (once per present), which is exactly the per-frame-cadence cloud
            // twitch this comment is written against.
            const Int maxUnit = ctx->GetMaxTouchedTextureUnit();
            for (Int unit = 0; unit <= maxUnit; ++unit) {
                const auto& texture = ctx->GetTextureUnitObject(unit)
                                          .GetBindingSlot(TextureTarget::TextureBuffer)
                                          .GetBoundObject();
                if (texture == nullptr || texture->GetStorageType() != TextureStorageType::Buffer) {
                    continue;
                }
                auto* bufferTexture = static_cast<MG_State::GLState::TextureObjectBuffer*>(texture.get());
                if (const auto& backing = bufferTexture->GetBufferBindingSlot().GetBoundObject()) {
                    consume(backing.get());
                }
            }
        }
        if (consumers.empty()) return;
        // The membership test is what keeps a bound non-member (an ordinary buffer, an
        // adopted store) out of PushBlocksForChecked; PushBlocksForChecked can Forget()
        // a member that stopped being one, which is why the list was copied out first.
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
        // `dl` (CONTRACT-P6 5.2): THE FAMILY WORD THIS SITE NEVER CARRIED. a6 found two aborts
        // under MG_Remote/ with no Fatal{ marker at all, so "the log stays verbatim" was not true
        // of them and no family grep could see them - this is one. (The other, WireLog.cpp's, is
        // the sanctioned funnel: every one of its callers passes a Fatal{ string of its own.)
        if (tier <= 1) {
            MGLOG_F("MGPipe: Fatal{UnimplementedAdoptTier, \"T%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u "
                    "names an adoption tier P11 implements and P5 does not; P5 runs at T2 "
                    "(emulate) only.",
                    static_cast<unsigned>(tier), static_cast<unsigned>(tier));
        } else {
            MGLOG_F("MGPipe: Fatal{UnimplementedAdoptTier, \"%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u is "
                    "not an adoption tier; the only values are 0 and 1 (P11) and 2 (emulate, the "
                    "P5 default).",
                    static_cast<unsigned>(tier), static_cast<unsigned>(tier));
        }
        std::abort();
    }

} // namespace MobileGL::MG_Remote::Client
