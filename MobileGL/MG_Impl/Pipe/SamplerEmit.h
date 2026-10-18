// MobileGL - MobileGL/MG_Impl/Pipe/SamplerEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// Client side of the sampler family: the content-addressed sampler CSO cache, one sampler view
// per texture object (addressed by identity), and the unit sets set_sampler_views and
// bind_sampler_states. set_shader_images is ImageEmit.h's, under the same subsystem bit.
// It is the client half of DirectGLES' ResolveAndBindUnitTextures, BindCurrentUnitSamplers
// and the program pass' sampler override (ARCHITECTURE.md §5.5). Espryt's raw-depth-fetch
// sampler substitution and Magma's feedback-loop detection act on the resolved set and stay on
// the server. Every emitter checks version latches before hashing anything.
#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHostSpan.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/SamplerState/SamplerObject.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/TextureState/TextureUnit.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <xxhash.h>

#include <cstring>

namespace MobileGL::MG_Pipe {

    // The subsystem bit this build emits for, covering the sampler CSO, the sampler view and all
    // three unit sets (set_shader_images in ImageEmit.h included): one bit, so switching samplers
    // off returns the whole family to the legacy path. PipeFill.cpp ORs it into
    // kMGPipeWiredSubsystems, which the validate point's `wants()` tests; the birth hooks'
    // FamilyIsLive tests this constant. While it is non-zero the ForwardWhenWired seam calls
    // EmitSamplerCso / EmitSamplerView, so a misspelled entry point fails to build. The runtime
    // switch is the MOBILEGL_PIPE_PUSH mask. Set it together with kMGPipeWiredTextureSubsystem.
    inline constexpr Uint64 kMGPipeWiredSamplerSubsystem = kMGPipeSubsystemSamplers;

    // ---- Canonical SamplerParameters copy ----
    // SamplerParameters is 100 bytes with 3 bytes of trailing padding no writer touches, so the
    // CSO cache never hashes or compares an object's own bytes: a padding mismatch is a silent
    // false miss (correct pixels, a fresh CSO per call). The copy is memset to zero, then
    // assigned field by field in MGP_FIELDS_SamplerParameters' order. It is an out-parameter
    // because returning by value copies and leaves the padding unspecified; canonical bytes are
    // stored only with memcpy. It runs once per mint attempt; the emitters' version checks keep
    // it off the per-draw path.
    inline void MGPipeCanonicaliseSamplerParameters(const SamplerParameters& src, SamplerParameters& canon) {
        static_assert(std::is_trivially_copyable_v<SamplerParameters>,
                      "the canonical copy is memset and then assigned field by field");
        std::memset(static_cast<void*>(&canon), 0, sizeof(canon));
        canon.wrapS = src.wrapS;
        canon.wrapT = src.wrapT;
        canon.wrapR = src.wrapR;
        canon.minFilter = src.minFilter;
        canon.magFilter = src.magFilter;
        canon.mipmapMode = src.mipmapMode;
        canon.minLod = src.minLod;
        canon.maxLod = src.maxLod;
        canon.lodBias = src.lodBias;
        canon.maxAnisotropy = src.maxAnisotropy;
        canon.compareFunc = src.compareFunc;
        canon.compareMode = src.compareMode;
        canon.borderColor = src.borderColor;
        canon.borderColorI = src.borderColorI;
        canon.borderColorUI = src.borderColorUI;
        // ALL FOUR BORDER-COLOUR MEMBERS CROSS, the form included. All three representations
        // are always numerically populated, so the value alone cannot say which driver entry
        // point applies (glSamplerParameterIiv vs fv, or which VkBorderColor family), and both
        // of Espryt's redundancy filters compare all four. Dropping this one line is G7's
        // scripted negative control and SamplerEmit's suite must go red naming it.
        canon.borderColorForm = src.borderColorForm;
    }

    inline Uint64 MGPipeHashSamplerParameters(const SamplerParameters& canon) {
        return XXH64(&canon, sizeof(canon), 0);
    }

    // Canonicalise and hash in one step, for a caller that wants only the hash.
    inline Uint64 MGPipeHashOfSamplerParameters(const SamplerParameters& src) {
        SamplerParameters canon;
        MGPipeCanonicaliseSamplerParameters(src, canon);
        return MGPipeHashSamplerParameters(canon);
    }

    // ---- Content-addressed sampler CSO cache ----
    // A SamplerObject is a pure value with no per-object driver binding state, so equal samplers
    // share one CSO. Lookup is CsoCache.h's: hash, probe, then memcmp before reusing a handle.
    // Eviction safety comes from reference counts, not capacity: LRU evicts only unreferenced
    // entries and the cache mints past capacity when all are pinned. Slots carry no lifetime id,
    // so ~SamplerObject frees nothing and a backend must not key a sampler twin on a
    // SamplerObject's lifetime id. Holders and the full rule: ARCHITECTURE.md §2.3.1.
    inline constexpr SizeT kMGPipeSamplerCsoCacheCapacity = 256;
    static_assert(kMGPipeSamplerCsoCacheCapacity > kMGPipeMaxTextureUnits,
                  "a cache that cannot hold one whole emission pass would mint over capacity on "
                  "every pass; correctness against eviction is the reference count, not this bound");

    // P14 (docs/Disaggregated/design/11-state-ownership.md): THE SHARE GROUP A RECORD EMITTED NOW
    // LANDS IN. The server files object records - create_sampler_state's among them - per
    // {session, share group}, under the context the stream is bound to. That is the stream's own
    // answer and not the calling thread's: a GL entry point rebinds to the caller first
    // (GLStreamScope), but an EGL operation does not, and eglCreateContext's birth hooks mint.
    // Without a stream (a monolith, a unit case) the caller's GLContext is the answer. Group
    // tokens are minted once and never reused (EGLState::CreateContext), which is what makes them
    // usable as an identity; 0 is "no EGL context", the server's "no context" applier.
    inline Uint64 MGPipeCurrentShareGroupToken() {
#if MOBILEGL_BUILD_DISAGGREGATED
        Uint64 bound = 0;
        if (MG_Impl::EGLImpl::StreamBoundShareGroupToken(&bound)) return bound;
#endif
        const auto& context = MG_State::pGLContext;
        if (!context) return 0;
        const auto& group = context->GetShareGroup();
        return group ? group->GetToken() : 0;
    }

    // How many share groups one entry remembers having published into. Forgetting one is always
    // safe - the next Acquire there re-issues the create on the same handle, which the applier
    // takes as a serial bump - so this bounds memory, not correctness.
    inline constexpr SizeT kMGPipeSamplerCsoPublishedGroups = 8;

    class MGPipeSamplerCsoCache {
    public:
        struct Counters {
            Uint64 Mints = 0; // create_sampler_state emissions
            Uint64 Acquisitions = 0;
            Uint64 Hits = 0;       // a probe that found a live entry and passed the memcmp
            Uint64 Collisions = 0; // a hash hit the memcmp REJECTED - the reason it exists
            Uint64 Evictions = 0;  // LRU evictions, each one a delete_sampler_state
            Uint64 Releases = 0;   // reference drops
            // Mints made with the cache already full and EVERY entry referenced. A recorded
            // number and not a gate (ID-17): growing past 256 is the safe direction, and this
            // is what makes it visible instead of silent.
            //
            // READ IT AS "HOW FAR THE CACHE PERMANENTLY GREW", not "how often it grew". Once
            // m_entries.size() is past the capacity every later Mint evicts one unreferenced
            // entry and pushes one, so the size stays at its high-water mark for the life of
            // the process - along with that many SamplerCso slots in the allocator and that
            // many create_sampler_state records in the applier. Bounded by the peak
            // simultaneous pin count, so this is memory retention and not a leak; the cache
            // never shrinks back.
            Uint64 OverCapacityMints = 0;
            // A RELEASE THIS CACHE COULD NOT ACCOUNT FOR. UnknownReleases: the handle names no
            // entry here (a stale handle from before a ForgetSession, a handle of another kind
            // passed by mistake, a handle whose entry was already evicted). UnderflowedReleases:
            // the entry was found with a count already 0. Both are contract violations by a
            // HOLDER and are counted rather than absorbed, because the pin they steal is
            // exactly what stops the LRU taking a handle a published record still names - see
            // Release(). Recorded numbers, not gates; MOBILEGL_ASSERT could not carry them
            // because it compiles out at INFO, i.e. in every gate build and every shipped one.
            Uint64 UnknownReleases = 0;
            Uint64 UnderflowedReleases = 0;
            // ID-17's Evict invariant, as a counter for the same reason (C2-n1). Mint's victim
            // choice structurally guarantees it stays 0; it is what would say so in a shipped
            // build if a second caller of Evict ever appeared.
            Uint64 ReferencedEvictions = 0;
            // create_sampler_state RE-ISSUED on an existing handle because the bound share group
            // had not been sent it - see AcquireCanonical.
            Uint64 GroupPublishes = 0;
        };

        // The handle for `params`' value, with a reference on it. On a miss it mints and emits
        // create_sampler_state, and emits delete_sampler_state for any entry it evicts;
        // `payloadBytes` accumulates the wire bytes. Every Acquire owes exactly one Release, and
        // a caller that names the handle in a published record holds it for as long as the
        // record stands. TextureEmit.h acquires MGPTextureParams::BuiltinSampler here (a null
        // handle there is Fatal{ProtocolCorruption}), so a texture's built-in sampler and a
        // bound sampler object with the same value share one CSO and one server twin.
        MGPipeHandle Acquire(const SamplerParameters& params, Uint64& payloadBytes) {
            SamplerParameters canon;
            MGPipeCanonicaliseSamplerParameters(params, canon);
            return AcquireCanonical(MGPipeHashSamplerParameters(canon), canon, payloadBytes);
        }

        // A TEST SEAM, and the only reason it is public. The collision branch below - the
        // memcmp confirm, the Collisions counter and the probe that keeps going past a
        // rejected entry - cannot be reached from Acquire without manufacturing a genuine
        // 64-bit XXH64 collision, so without this the whole branch is uncovered and the case
        // named for it proves something else. It hashes nothing and simply uses the hash it is
        // given; production code has no reason to call it.
        //
        // The members a consumer must not call are this one and ResetCounters(); ForgetSession()
        // (which frees every slot the cache holds and drops their publication latches) belongs to
        // the end of a session (MGPipeForgetEndedSession) and to unit fixtures. Package B's seam
        // is Acquire / Release / RefCountOf / RecordIsPublished / Size / GetCounters and nothing
        // else - the class is public because it is header-only, not because all nine members are
        // for callers.
        MGPipeHandle AcquireWithForcedHashForTest(const SamplerParameters& params, Uint64 forcedHash,
                                                  Uint64& payloadBytes) {
            SamplerParameters canon;
            MGPipeCanonicaliseSamplerParameters(params, canon);
            return AcquireCanonical(forcedHash, canon, payloadBytes);
        }

        // Drops one reference. The entry stays - a released value is still worth reusing - it
        // merely becomes eligible for the LRU again. Releasing a handle this cache never handed
        // out, or releasing one twice, does not underflow: the death paths that call it are
        // idempotent by contract and a second call must not corrupt the count.
        //
        // BUT THE COUNT IS PER HANDLE, NOT PER HOLDER, and that is the sentence a holder has to
        // read. Entries are content-addressed and shared by design - package B's texture-params
        // record and this file's own bind_sampler_states will routinely name the SAME handle,
        // each owing its own Release - so a holder that releases twice is not harmlessly
        // repeating itself: it takes ANOTHER holder's pin, the entry can reach RefCount 0 while
        // a published MGPTextureParams and a standing bind_sampler_states set still name it,
        // and the LRU may then evict it with nothing to refuse and nothing to re-emit. This
        // cannot be repaired here, so it is COUNTED (UnknownReleases / UnderflowedReleases)
        // rather than absorbed silently, and that is what a reviewer of a holder asserts on.
        void Release(MGPipeHandle handle) {
            if (MGPipeHandleIsNull(handle)) return;
            for (Entry& entry : m_entries) {
                if (entry.Cso != handle) continue;
                if (entry.RefCount > 0) {
                    --entry.RefCount;
                } else {
                    ++m_counters.UnderflowedReleases;
                }
                ++m_counters.Releases;
                return;
            }
            ++m_counters.UnknownReleases;
        }

        // How many live references this cache is holding for `handle`. Diagnostics and unit
        // cases; nothing on the emission path asks.
        Uint32 RefCountOf(MGPipeHandle handle) const {
            if (MGPipeHandleIsNull(handle)) return 0;
            for (const Entry& entry : m_entries) {
                if (entry.Cso == handle) return entry.RefCount;
            }
            return 0;
        }

        // "Does this cache still hold a create_sampler_state record for exactly this handle?"
        //
        // IT IS NOT THE DEATH PATH's AUTHORITY, and that changed at c0b: the six death helpers
        // read A's publication latch (MGPipeHandleIsPublished, PipeMutation.h), which is one
        // answer per {kind, slot, gen} written by whatever emitted the create. This stays
        // because it is the CACHE's own question - "is this value still resident here" - which
        // is a different question and is what a unit case reads.
        Bool RecordIsPublished(MGPipeHandle handle) const {
            if (MGPipeHandleIsNull(handle)) return false;
            for (const Entry& entry : m_entries) {
                if (entry.Cso == handle) return true;
            }
            return false;
        }

        // A session that replaced an ended one (MGPipeForgetEndedSession: the fresh server holds
        // none of these records), and a unit test's fixture. NOT called from the validate point's
        // FreshlyPrimed arm: MGPipeApplierReset is a make-current and does NOT drop object
        // records, so a sampler CSO the applier holds outlives a context switch. Dropping the
        // cache there would leak the applier's record and re-mint a value it already has.
        //
        // IT DROPS THE PUBLICATION LATCH FOR EVERY ENTRY IT FREES. The slots go back with no
        // delete_sampler_state behind them, so a latch left standing would make a future death
        // helper - on the recycled slot at the same generation - emit a delete for a record
        // that never existed, which is the refusal the applier counts.
        void ForgetSession() {
            for (const Entry& entry : m_entries) {
                MGPipeNoteHandleUnpublished(MGPipeKind::SamplerCso, entry.Cso);
                MGPipeSlots().Free(MGPipeKind::SamplerCso, entry.Cso);
            }
            m_entries.clear();
            m_clock = 0;
        }

        void ResetCounters() { m_counters = Counters{}; }
        SizeT Size() const { return m_entries.size(); }
        const Counters& GetCounters() const { return m_counters; }

    private:
        struct Entry {
            Uint64 Hash = 0;
            Uint64 LastUsed = 0;
            MGPipeHandle Cso = kMGPipeNullHandle;
            // THE PIN. Non-zero means some record that is still standing names this handle, so
            // the LRU may not take it (ID-17).
            Uint32 RefCount = 0;
            // THE CANONICAL BYTES, not the caller's. This is the memcmp's other operand and it
            // has to have the same deterministic padding the probe's copy has.
            SamplerParameters Params{};
            // THE SHARE GROUPS whose object records hold this handle's create_sampler_state
            // (MGPipeCurrentShareGroupToken). The entry is process-wide but the applier's record
            // is per group, so a hit from a group not listed here re-issues the create before
            // the caller names the handle there. Full -> the oldest is overwritten.
            Uint64 PublishedGroups[kMGPipeSamplerCsoPublishedGroups] = {};
            Uint32 PublishedGroupCount = 0;
            Uint32 NextPublishedGroup = 0;
        };

        static Bool IsPublishedIn(const Entry& entry, Uint64 group) {
            for (Uint32 i = 0; i < entry.PublishedGroupCount; ++i) {
                if (entry.PublishedGroups[i] == group) return true;
            }
            return false;
        }

        static void NotePublishedIn(Entry& entry, Uint64 group) {
            if (IsPublishedIn(entry, group)) return;
            if (entry.PublishedGroupCount < kMGPipeSamplerCsoPublishedGroups) {
                entry.PublishedGroups[entry.PublishedGroupCount++] = group;
                return;
            }
            entry.PublishedGroups[entry.NextPublishedGroup] = group;
            entry.NextPublishedGroup = (entry.NextPublishedGroup + 1) % kMGPipeSamplerCsoPublishedGroups;
        }

        // The create Mint sends, re-issued on an entry that already exists: same handle, same
        // canonical bytes.
        void EmitCreate(Entry& entry, Uint64& payloadBytes) {
            MGPSamplerDesc desc{};
            desc.Cso = entry.Cso;
            desc.Parameters.Seg = kMGHostSpanSegNone;
            desc.Parameters.Offset = 0;
            desc.Parameters.Size = 0;
            MGPipeRouteCreateSamplerState(desc, &entry.Params);
            NotePublishedIn(entry, MGPipeCurrentShareGroupToken());
            payloadBytes += sizeof(MGPSamplerDesc) + sizeof(SamplerParameters);
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::CsoBlobBytes, sizeof(SamplerParameters));
            }
        }

        MGPipeHandle AcquireCanonical(Uint64 hash, const SamplerParameters& canon, Uint64& payloadBytes) {
            ++m_counters.Acquisitions;
            for (SizeT i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].Hash != hash) continue;
                if (std::memcmp(&m_entries[i].Params, &canon, sizeof(canon)) != 0) {
                    // A 64-bit collision between two DIFFERENT sampler states. Reusing the
                    // handle would filter one state with the other's parameters, so this entry
                    // is not a match - and the probe KEEPS GOING rather than evicting and
                    // giving up. Evicting here would be a second eviction path the reference
                    // count would have to police for no gain (the colliding entry may be
                    // pinned, and it may even be one the record being built right now names),
                    // and stopping here would miss a LATER entry with the same hash whose bytes
                    // really do match and mint a duplicate CSO for a value the cache holds.
                    ++m_counters.Collisions;
                    continue;
                }
                m_entries[i].LastUsed = ++m_clock;
                ++m_entries[i].RefCount;
                ++m_counters.Hits;
                // A HIT IS ONLY A HIT IN THE GROUPS THAT WERE SENT THE CREATE. Two non-shared
                // contexts asking for one value get one handle, but the server holds the record
                // in the first one's share group only; naming the handle from the second (a
                // set_texture_params, a bind_sampler_states) resolved to nothing there and the
                // texture sampled with default parameters. The re-issue lands the record in this
                // group under the same handle, which is safe because the slot space is
                // process-wide: another group's table holding it is wider, never aliased.
                if (!IsPublishedIn(m_entries[i], MGPipeCurrentShareGroupToken())) {
                    EmitCreate(m_entries[i], payloadBytes);
                    ++m_counters.GroupPublishes;
                }
                return m_entries[i].Cso;
            }
            return Mint(hash, canon, payloadBytes);
        }

        MGPipeHandle Mint(Uint64 hash, const SamplerParameters& canon, Uint64& payloadBytes) {
            if (m_entries.size() >= kMGPipeSamplerCsoCacheCapacity) {
                // THE LRU VICTIM IS CHOSEN AMONG UNREFERENCED ENTRIES ONLY. A referenced entry
                // is named by a record that is still standing - a texture's BuiltinSampler, a
                // bound sampler state - and nothing re-emits when a handle stops being valid,
                // so evicting one is a silent corruption with no gate that can see it.
                SizeT victim = m_entries.size();
                for (SizeT i = 0; i < m_entries.size(); ++i) {
                    if (m_entries[i].RefCount != 0) continue;
                    if (victim == m_entries.size() || m_entries[i].LastUsed < m_entries[victim].LastUsed) {
                        victim = i;
                    }
                }
                if (victim < m_entries.size()) {
                    Evict(victim);
                } else {
                    // EVERY ENTRY IS PINNED, so the cache grows past its capacity and says so.
                    // A recorded number, not a gate (ID-17).
                    ++m_counters.OverCapacityMints;
                }
            }

            const MGPipeHandle cso = MGPipeSlots().Allocate(MGPipeKind::SamplerCso);
            MGPSamplerDesc desc{};
            desc.Cso = cso;
            // THE ONE BLOB RULE (MGPipeTypes.h): Size 0 means "this record does not declare its
            // blob", which is what a monolith emission is - the parameters ride beside the
            // record through the entry point's companion pointer and the applier stores them by
            // value. Splitting this for a transport is P5's problem, not this emitter's.
            desc.Parameters.Seg = kMGHostSpanSegNone;
            desc.Parameters.Offset = 0;
            desc.Parameters.Size = 0;

            // BUILT IN PLACE AND FILLED WITH A MEMCPY, not assigned from a local. This is the
            // padding trap one level deeper than the one the design names: an assignment copies
            // the sixteen MEMBERS and leaves the three trailing padding bytes of the
            // destination at whatever was there, so the next probe's memcmp would reject its
            // own entry, count a collision that never happened, evict and mint a fresh CSO - a
            // cache with a hit rate of zero whose failure depends on heap contents, which is
            // why it passes a case run alone and fails the same case run in a suite. The bytes
            // stored here have to be the bytes compared later, padding included.
            m_entries.push_back(Entry{});
            Entry& entry = m_entries.back();
            entry.Hash = hash;
            entry.LastUsed = ++m_clock;
            entry.Cso = cso;
            // THE MINT IS ITSELF AN ACQUIRE, so the caller owes one Release for it exactly as
            // it would for a hit. Anything else would make the first acquirer of a value the
            // one caller whose handle the LRU could take.
            entry.RefCount = 1;
            std::memcpy(static_cast<void*>(&entry.Params), &canon, sizeof(canon));
            // The applier is handed the CACHE's copy, so the pointer stays valid for the whole
            // call and the bytes it stores are provably the bytes the memcmp will confirm
            // against later.
            MGPipeRouteCreateSamplerState(desc, &m_entries.back().Params);
            NotePublishedIn(m_entries.back(), MGPipeCurrentShareGroupToken());
            // THE CREATE ACTUALLY WENT OUT, so the publication latch is taken here and nowhere
            // else (contract-v2 §3.1). It is what the six death helpers read, and without it a
            // delete_sampler_state can never go out for this kind.
            MGPipeNoteHandlePublished(MGPipeKind::SamplerCso, cso);

            ++m_counters.Mints;
            payloadBytes += sizeof(MGPSamplerDesc) + sizeof(SamplerParameters);
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::CsoBlobBytes, sizeof(SamplerParameters));
            }
            return cso;
        }

        // ONLY EVER CALLED WITH AN UNREFERENCED ENTRY - see Mint's victim choice. That is the
        // whole of ID-17's ruling: a handle a standing record names cannot be taken, because
        // the applier does not resolve MGPTextureParams::BuiltinSampler and an eviction is not
        // a parameter change, so nothing would refuse and nothing would re-emit.
        //
        // COUNTED, NOT ASSERTED (C2-n1). MOBILEGL_ASSERT is live only at DEBUG, so an assertion
        // here would be absent from all three gate builds and from every shipped build - the
        // idiom D-J3 forbids and the one C-m6 removed from the program emitter. The eviction
        // still proceeds when the count is non-zero: the sole caller guarantees the condition,
        // and refusing here would leave Mint without the room it asked for.
        void Evict(SizeT index) {
            if (m_entries[index].RefCount != 0) ++m_counters.ReferencedEvictions;
            MGPHandleOnly handle{};
            handle.Handle = m_entries[index].Cso;
            handle.Kind = static_cast<Uint32>(MGPipeKind::SamplerCso);
            // THE THREE-STEP ORDER, the same one PipeMutation.h fixes for the death helpers:
            // the wire delete drops the applier's record while the record still exists, and
            // only then does the slot go back. There is no NotifyStateObjectDestroyed step
            // here - a content-addressed CSO has no frontend object whose death is being
            // announced, which is precisely why this eviction is the only death path it has.
            //
            // THE DELETE ENDS THE HANDLE IN EVERY SHARE GROUP IT WAS PUBLISHED TO, not only the
            // bound one: the applier drops a sampler CSO from every group of its session
            // (MGPipeApplyDeleteSamplerState), because the handle this frees is one identity for
            // the whole session. So it is sent whenever ANY group was sent the create - and not
            // at all for an entry no group holds, which the applier would count as a refusal.
            if (m_entries[index].PublishedGroupCount != 0) {
                MGPipeRouteDeleteSamplerState(handle);
            }
            // AND THE LATCH GOES WITH THE DELETE. This is the "an emitter that drops a record
            // for its own reasons calls MGPipeNoteHandleUnpublished" half of the publication
            // protocol (contract-v2 §3.1): the record is gone, so a death helper reaching this
            // slot after it is recycled must not emit a second delete for it.
            MGPipeNoteHandleUnpublished(MGPipeKind::SamplerCso, m_entries[index].Cso);
            MGPipeSlots().Free(MGPipeKind::SamplerCso, m_entries[index].Cso);
            // A COPY-ASSIGNMENT, which does NOT carry padding - the same trap Mint's memcpy
            // exists for, one level along. It is safe here only because every entry's padding
            // was already made deterministically zero by that memcpy, so the assignment's
            // memberwise copy of Params has nothing indeterminate to propagate and Vector's own
            // growth memmoves a trivially copyable type. An Entry built any other way would
            // break the cache's "the bytes stored are the bytes compared" invariant here rather
            // than at the mint, which is much harder to see.
            m_entries[index] = m_entries.back();
            m_entries.pop_back();
            ++m_counters.Evictions;
        }

        Vector<Entry> m_entries;
        Uint64 m_clock = 0;
        Counters m_counters;
    };

    inline MGPipeSamplerCsoCache& MGPipeSamplerCsoCacheInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason, and this one is on a death
        // path: TextureEmit.h asks it for every texture's built-in sampler CSO, so an exit
        // handler running a frontend destructor must not find it freed.
        static MGPipeSamplerCsoCache* cache = new MGPipeSamplerCsoCache();
        return *cache;
    }

    // ---------------------------------------------------------------------------------
    // D-F3: the client-side sampling resolution
    // ---------------------------------------------------------------------------------
    //
    // GL binds one texture per unit PER TARGET; which one the shader actually samples depends
    // on the sampler uniform's TYPE. One sampler view per slot is the resolved form, so the
    // resolution moves to the client and the record carries the answer rather than the inputs.
    //
    // This is DirectGLES' SamplerUniformTextureTarget, moved to the side of the boundary that
    // now owns the question. Targets with no sampler spelling map to Unknown, and a unit whose
    // uniform type resolves to Unknown carries no view - which is exactly "the program does not
    // sample this unit".
    inline MobileGL::TextureTarget MGPipeSamplerUniformTextureTarget(GLenum uniformType) {
        switch (uniformType) {
        // GL_OES_EGL_image_external: the unit's external binding, whose texture is a 2D one below.
        case GL_SAMPLER_EXTERNAL_OES:
            return TextureTarget::External;
        case GL_SAMPLER_1D:
        case GL_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_SAMPLER_1D_SHADOW:
            return TextureTarget::Texture1D;
        case GL_SAMPLER_2D:
        case GL_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_SAMPLER_2D_SHADOW:
            return TextureTarget::Texture2D;
        case GL_SAMPLER_3D:
        case GL_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
            return TextureTarget::Texture3D;
        case GL_SAMPLER_CUBE:
        case GL_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_SAMPLER_CUBE_SHADOW:
            return TextureTarget::TextureCubeMap;
        case GL_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
            return TextureTarget::Texture1DArray;
        case GL_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
            return TextureTarget::Texture2DArray;
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
            return TextureTarget::TextureCubeMapArray;
        case GL_SAMPLER_2D_RECT:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
            return TextureTarget::TextureRectangle;
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
            return TextureTarget::Texture2DMultisample;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
            return TextureTarget::Texture2DMultisampleArray;
        case GL_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
            return TextureTarget::TextureBuffer;
        default:
            return TextureTarget::Unknown;
        }
    }

    // Unit -> sampled target, and the highest image unit the program names, both INVERTED ONCE
    // per program state rather than searched per unit.
    //
    // DirectGLES asks the question the other way round - for one unit it walks every uniform
    // location - and gets away with it because it only asks on an aliasing conflict. A client
    // that asked per unit would be O(units x locations) at every verb the sampler bit fires
    // on, which is exactly the per-draw cost the phase's budget forbids. So the walk runs once
    // and is memoised on (lifetime id, link version, backend state version): the third is the
    // counter SetUniformSamplerOrImageUnitIndex bumps, i.e. the one thing that can move a
    // uniform's unit without relinking.
    //
    // IT IS SHARED WITH ImageEmit.h on purpose. The sampler and image halves come out of one
    // walk over one array, they ride one subsystem bit, and computing them separately would
    // walk the same locations twice per program change.
    class MGPipeProgramOpaqueUnits {
    public:
        using ProgramObject = MG_State::GLState::ProgramObject;

        struct Resolution {
            // TextureTarget + 1 per unit, 0 meaning "this program samples nothing here". A
            // Uint8 because TextureTargetCount is 11 and this array is 192 entries long on a
            // path that wants to stay in cache.
            Array<Uint8, kMGPipeMaxTextureUnits> SamplerTarget{};
            // Highest image unit the program names, or -1 for a program with no image
            // uniforms - which is what gives ImageEmit.h its zero early-out for free.
            Int32 MaxImageUnit = -1;
        };

        const Resolution& For(const ProgramObject* program) {
            const Uint64 lifetimeId = program != nullptr ? program->GetLifetimeId() : 0;
            const Uint32 linkVersion = program != nullptr ? program->GetLinkVersion() : 0;
            const Uint32 stateVersion = program != nullptr ? program->GetBackendStateVersion() : 0;
            if (m_valid && m_lifetimeId == lifetimeId && m_linkVersion == linkVersion &&
                m_stateVersion == stateVersion) {
                return m_resolution;
            }
            m_resolution = Resolution{};
            // GetLinkStatus is the same guard DirectGLES uses before it trusts the reflection:
            // a program that did not link has no usable uniform table, and every unit resolves
            // to "not sampled".
            if (program != nullptr && program->GetLinkStatus()) {
                const Uint maxLocation = program->GetMaxUniformLocation();
                for (Uint location = 0; location <= maxLocation; ++location) {
                    const Int unit = program->GetUniformSamplerOrImageUnitIndex(location);
                    if (unit < 0 || unit >= static_cast<Int>(kMGPipeMaxTextureUnits)) continue;
                    if (program->GetUniformTypeFacts(location).isImage) {
                        if (unit > m_resolution.MaxImageUnit) m_resolution.MaxImageUnit = unit;
                        continue;
                    }
                    const TextureTarget target = MGPipeSamplerUniformTextureTarget(program->GetUniformType(location));
                    if (target == TextureTarget::Unknown) continue;
                    // FIRST WRITER WINS, which is DirectGLES' arbitration read forwards: when
                    // two sampler uniforms of different types share a unit the binding placed
                    // first stands rather than being silently overwritten by whichever location
                    // comes last.
                    Uint8& slot = m_resolution.SamplerTarget[static_cast<SizeT>(unit)];
                    if (slot == 0) slot = static_cast<Uint8>(static_cast<Int>(target) + 1);
                }
            }
            m_lifetimeId = lifetimeId;
            m_linkVersion = linkVersion;
            m_stateVersion = stateVersion;
            m_valid = true;
            return m_resolution;
        }

        void Invalidate() { m_valid = false; }

    private:
        Resolution m_resolution;
        Uint64 m_lifetimeId = 0;
        Uint32 m_linkVersion = 0;
        Uint32 m_stateVersion = 0;
        Bool m_valid = false;
    };

    // ONE inversion for the whole family, shared by MGPipeSamplerEmitter and
    // MGPipeImageEmitter. Two memos would walk the same uniform table twice per program
    // change, and - worse - could disagree about which locations they saw, which is how a
    // sampler unit and an image unit come to be resolved against two different readings of one
    // program. Never destroyed, like every other MGPipe process singleton.
    inline MGPipeProgramOpaqueUnits& MGPipeProgramOpaqueUnitsShared() {
        static MGPipeProgramOpaqueUnits* units = new MGPipeProgramOpaqueUnits();
        return *units;
    }

    // ---------------------------------------------------------------------------------
    // D-G3: the two unit sets' content hashes
    // ---------------------------------------------------------------------------------
    //
    // XXH64 over the tail entries, with Start and Count mixed in through MGPipeMixShutter -
    // the VertexInputEmit shape, and for its reason: the hash must cover EVERY input the
    // record carries, or a record whose one changed field is outside the tail gets suppressed.
    // Both tails are built into zero-initialised staging arrays, so no padding byte enters
    // either hash.
    inline Uint64 MGPipeSamplerViewSetContentHash(const MGPBoundView* entries, Uint32 start, Uint32 count) {
        Uint64 hash = XXH64(entries, static_cast<SizeT>(count) * sizeof(MGPBoundView), 0);
        hash = MGPipeMixShutter(hash, start);
        hash = MGPipeMixShutter(hash, count);
        return hash;
    }

    inline Uint64 MGPipeSamplerStateSetContentHash(const MGPipeHandle* entries, Uint32 start, Uint32 count) {
        Uint64 hash = XXH64(entries, static_cast<SizeT>(count) * sizeof(MGPipeHandle), 0);
        hash = MGPipeMixShutter(hash, start);
        hash = MGPipeMixShutter(hash, count);
        return hash;
    }

    // ---------------------------------------------------------------------------------
    // The emitter
    // ---------------------------------------------------------------------------------

    class MGPipeSamplerEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;
        using ITextureObject = MG_State::GLState::ITextureObject;
        using SamplerObject = MG_State::GLState::SamplerObject;

        // set_sampler_views: the program-resolved set, one entry per unit, no stage dimension.
        // Start is 0; Count is GetMaxTouchedTextureUnit() + 1, clamped to the wire bound. An
        // entry is null when the program samples nothing at the unit, nothing is bound, the
        // bound texture is the undefined default texture, or it samples as incomplete;
        // otherwise it names the texture and its sampler-view CSO. Nulling an incomplete
        // texture matches DirectGLES leaving the native target unbound: it must sample
        // (0,0,0,1), and the backend's immutable storage never saw the redefined level.
        Uint64 EmitSamplerViews(GLContext& ctx) {
            const Int maxTouched = ctx.GetMaxTouchedTextureUnit();
            const Uint32 count =
                maxTouched < 0 ? 0u : Min(static_cast<Uint32>(maxTouched) + 1u, kMGPipeMaxTextureUnits);

            // The join is the EMITTER's, deliberately, and it is the same GetProgramForDraw()
            // the verb is about to make anyway: the tracker's own shutters read
            // GetCurrentProgram() precisely so that answering "did the shader move" never
            // forces a compile. In the ladder EmitShaderState runs before this, so in the
            // steady state the program is already joined by the time this line runs.
            const auto& program = ctx.GetProgramForDraw();
            const auto& resolution = MGPipeProgramOpaqueUnitsShared().For(program.get());

            Uint64 bytes = 0;
            for (Uint32 unit = 0; unit < count; ++unit) {
                MGPBoundView& entry = m_views[unit];
                entry = MGPBoundView{};
                entry.Unit = unit;
                entry.View = kMGPipeNullHandle;
                entry.Texture = kMGPipeNullHandle;

                const Uint8 encoded = resolution.SamplerTarget[unit];
                if (encoded == 0) continue;
                const auto target = static_cast<TextureTarget>(static_cast<Int>(encoded) - 1);

                auto& textureUnit = ctx.GetTextureUnitObject(static_cast<Int>(unit));
                const auto& texture = textureUnit.GetBindingSlot(target).GetBoundObject();
                if (!texture) continue;
                if (MG_State::GLState::IsUndefinedDefaultTexture(texture.get())) continue;
                // The unit's sampler object overrides the texture's own, exactly as in GL, and
                // the completeness answer depends on which one applies - a mipmap mode of None
                // makes a single-level texture complete that would otherwise sample black.
                const auto& unitSampler = textureUnit.GetSamplerObject();
                const SamplerObject* effective = unitSampler ? unitSampler.get() : texture->GetSamplerObject().get();
                if (MG_State::GLState::SamplesAsIncompleteTexture(texture.get(), effective)) continue;

                entry.Texture =
                    MGPipeSlots().AcquireHinted(MGPipeKind::Texture, texture->GetLifetimeId(),
                                                texture->PipeHandleHint(ITextureObject::PipeHintKind::Texture));
                // D-A4: a texture the sampler-view resolution names in an emitted MGPBoundView
                // is SAMPLER-bound from then on (sticky; the texture emitter's contract door,
                // since this header is included BY TextureEmit.h). One early-out per unit per
                // pass once the bit is set.
                MGPipeNoteTextureBoundAs(entry.Texture, static_cast<Uint32>(kMGPipeBindSampler));
                entry.View = AcquireSamplerView(*texture, entry.Texture, bytes);
            }

            const Uint64 hash = MGPipeSamplerViewSetContentHash(m_views.data(), 0, count);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetSamplerViews, hash)) {
                return bytes;
            }
            m_lastViews = MGPSamplerViews{};
            m_lastViews.Start = 0;
            m_lastViews.Count = count;
            m_lastViews.ContentHash = hash;
            MGPipeRouteSetSamplerViews(m_lastViews, m_views.data());
            ++m_viewSets;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::SamplerViewEmissions, 1);
            }
            return bytes + sizeof(MGPSamplerViews) + static_cast<Uint64>(count) * sizeof(MGPBoundView);
        }

        // bind_sampler_states: each unit's sampler CSO, or null when the unit has no sampler
        // object and the texture's built-in sampler applies. Not program-resolved: a sampler
        // object applies to whatever its unit holds, as in BindCurrentUnitSamplers. A bound
        // handle keeps a cache reference while bound, because the applier's set outlives this
        // pass. Cost: Acquire and each non-null Release scan the whole cache, 2 * K * cache size
        // per pass for K units with sampler objects (usually 0 in Minecraft, large in CTS sampler
        // sweeps), on every texture bind; the set-hash suppressor skips only the record. A
        // per-unit (sampler lifetime id, sampling resolution generation) latch would avoid both.
        Uint64 EmitSamplerStates(GLContext& ctx) {
            const Int maxTouched = ctx.GetMaxTouchedTextureUnit();
            const Uint32 count =
                maxTouched < 0 ? 0u : Min(static_cast<Uint32>(maxTouched) + 1u, kMGPipeMaxTextureUnits);

            Uint64 bytes = 0;
            MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
            for (Uint32 unit = 0; unit < count; ++unit) {
                const auto& sampler = ctx.GetTextureUnitObject(static_cast<Int>(unit)).GetSamplerObject();
                m_states[unit] = sampler ? cache.Acquire(sampler->GetAllSamplerParameters(), bytes) : kMGPipeNullHandle;
            }
            for (Uint32 unit = 0; unit < kMGPipeMaxTextureUnits; ++unit) {
                const MGPipeHandle held = m_stateRefs[unit];
                const MGPipeHandle now = unit < count ? m_states[unit] : kMGPipeNullHandle;
                if (held == now) {
                    // The same CSO as last time: give back the duplicate reference this pass's
                    // Acquire took and keep the one that was already held.
                    if (unit < count) cache.Release(now);
                    continue;
                }
                cache.Release(held);
                m_stateRefs[unit] = now;
            }

            const Uint64 hash = MGPipeSamplerStateSetContentHash(m_states.data(), 0, count);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::BindSamplerStates, hash)) {
                return bytes;
            }
            m_lastStates = MGPSamplerStates{};
            m_lastStates.Start = 0;
            m_lastStates.Count = count;
            m_lastStates.ContentHash = hash;
            MGPipeRouteBindSamplerStates(m_lastStates, m_states.data());
            ++m_stateSets;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::SamplerStateEmissions, 1);
            }
            return bytes + sizeof(MGPSamplerStates) + static_cast<Uint64>(count) * sizeof(MGPipeHandle);
        }

        // D-F2: ONE SamplerViewCso PER ITextureObject, minted off its lifetime id, and
        // create_sampler_view RE-ISSUED ON THE SAME HANDLE whenever the restrictions move -
        // legal because Gen increments only on slot reuse and never on a respecify.
        //
        // A DEVIATION FROM THE CONTENT-ADDRESSED 4096-ENTRY CACHE the design gives this kind,
        // and it is P3a's vertex-elements deviation for the same reason: MobileGL has no
        // frontend sampler-view object at all, so EVERY sampled texture needs one minted, and
        // a content-addressed mint per texture per verb is exactly the per-draw cost the
        // budget forbids. The content-addressed cache is right when Magma's image-view factory
        // takes the CSO over and its VkImageView create-info is what is being addressed.
        //
        // THE VERSION-FIRST SKIP is the shape version, which is what BumpShapeVersion moves on
        // every shape and format change - i.e. on exactly the inputs MGPSamplerView carries.
        // The params version rides beside it belt-and-braces: no field of the record depends on
        // it, so a wrap of that Uint16 can only cost a skipped re-issue of an identical record.
        MGPipeHandle AcquireSamplerView(const ITextureObject& texture, MGPipeHandle textureHandle,
                                        Uint64& payloadBytes) {
            const MGPipeHandle handle =
                MGPipeSlots().AcquireHinted(MGPipeKind::SamplerViewCso, texture.GetLifetimeId(),
                                            texture.PipeHandleHint(ITextureObject::PipeHintKind::SamplerView));
            const SizeT slot = handle.Slot;
            if (slot >= m_viewLatch.size()) m_viewLatch.resize(slot + 1);
            ViewLatch& latch = m_viewLatch[slot];

            const Uint64 shapeVersion = texture.GetShapeVersion();
            const Uint16 paramsVersion = texture.GetTextureParamsVersion();
            if (latch.RecordLive && latch.RecordGen == handle.Gen && latch.ShapeVersion == shapeVersion &&
                latch.ParamsVersion == paramsVersion && latch.Texture == textureHandle) {
                return handle;
            }

            m_lastView = MGPSamplerView{};
            m_lastView.Cso = handle;
            m_lastView.Texture = textureHandle;
            // THE ALIASING FORMAT. For a glTextureView this is the view's own internal format
            // and not the storage owner's, which is the whole point of the call; for an
            // ordinary texture it is simply its format.
            m_lastView.InternalFormat = static_cast<Uint32>(texture.GetFormat());
            m_lastView.Target = static_cast<Uint8>(MGPipeWireTextureTarget(texture.GetTarget()));
            // THE FOUR RESTRICTIONS COME FROM ONE PLACE. TextureObjectBase leaves all four at
            // 0 for an ordinary texture and glTextureView writes them for a view, and a view
            // always has NumLevels >= 1 - so a zero here unambiguously means "no restriction,
            // the whole storage" and there is no second spelling of the unrestricted case that
            // could disagree with the first.
            m_lastView.MinLevel = static_cast<Uint16>(texture.GetViewMinLevel());
            m_lastView.NumLevels = static_cast<Uint16>(texture.GetViewNumLevels());
            m_lastView.MinLayer = static_cast<Uint16>(texture.GetViewMinLayer());
            m_lastView.NumLayers = static_cast<Uint16>(texture.GetViewNumLayers());
            m_lastView.Samples = static_cast<Uint16>(texture.GetSamples() < 0 ? 0 : texture.GetSamples());
            m_lastView.FixedSampleLocations = texture.HasFixedSampleLocations() ? 1 : 0;
            MGPipeRouteCreateSamplerView(m_lastView);
            // THE CREATE WENT OUT, so the publication latch is taken (contract-v2 §3.1). The
            // texture's death helper reads it, and without it delete_sampler_view can never go
            // out - the C-1 leak, one kind later. Re-taking it on a re-issue is right and
            // cheap: the latch is keyed {kind, slot, gen} and the re-issue is on the same
            // handle, so this writes the answer it already held.
            MGPipeNoteHandlePublished(MGPipeKind::SamplerViewCso, handle);
            ++m_viewCreates;
            payloadBytes += sizeof(MGPSamplerView);

            latch.RecordLive = true;
            latch.RecordGen = handle.Gen;
            latch.ShapeVersion = shapeVersion;
            latch.ParamsVersion = paramsVersion;
            latch.Texture = textureHandle;
            return handle;
        }

        // ---- Birth hooks, forwarded from PipeFill.cpp ----
        // ForwardWhenWired calls these after FamilyIsLive passes, so the names must match.
        // EmitSamplerCso content-addresses the new object's default parameters, so all
        // default-valued samplers share one create_sampler_state. Its reference is released at
        // once: a birth names the handle in no record, and pinning it would keep the default
        // value unevictable; whoever later names the handle takes its own reference.
        void EmitSamplerCso(SamplerObject& sampler) {
            Uint64 bytes = 0;
            MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
            cache.Release(cache.Acquire(sampler.GetAllSamplerParameters(), bytes));
        }

        // The birth half of D-F2's one-view-per-texture rule, and it is a thin wrapper on
        // purpose: AcquireSamplerView below is the whole handle rule - the identity-addressed
        // mint off the texture's lifetime id, the version-first skip and the publication - and
        // a second copy of any of it here would be a second authority. The texture handle is
        // resolved exactly as EmitSamplerViews resolves it, and the byte count is discarded
        // because a birth is not a validate-point emission and has no payload budget to report.
        void EmitSamplerView(ITextureObject& texture) {
            Uint64 bytes = 0;
            const MGPipeHandle textureHandle = MGPipeSlots().Acquire(MGPipeKind::Texture, texture.GetLifetimeId());
            AcquireSamplerView(texture, textureHandle, bytes);
        }

        // The emitter's OWN record memo for a sampler view - "have I already published a
        // create_sampler_view at this slot, for this generation".
        //
        // IT IS NOT WHAT THE DEATH PATH ASKS, and that changed at c0b (contract-v2 §3.1/D17):
        // MGPipeEmitSamplerViewCsoDestroyAndFree reads A's publication latch
        // (MGPipeHandleIsPublished), which is one answer per {kind, slot, gen} that every
        // emitter writes and all six death helpers read - P4a has six kinds behind four
        // emitters and one kind with no frontend object at all, so a per-emitter predicate has
        // no well-defined owner. This stays because the VERSION-FIRST SKIP needs it: it is the
        // same latch AcquireSamplerView consults before it hashes or copies anything.
        Bool RecordIsPublished(MGPipeHandle handle) const {
            if (MGPipeHandleIsNull(handle)) return false;
            const SizeT slot = handle.Slot;
            if (slot >= m_viewLatch.size()) return false;
            const ViewLatch& latch = m_viewLatch[slot];
            return latch.RecordLive && latch.RecordGen == handle.Gen;
        }

        // The memo's other half, for a caller that knows the applier has dropped this record.
        // THE CALLER IS THE CONTRACT's DEATH HELPER (P4a final review C-2): the texture's
        // helper drops the sampler view minted off the texture's lifetime id and forwards here
        // before the slot is freed, so a dead handle no longer reads as published in this memo
        // between the death and the recycle. Gen-keyed, so a late notice for a slot already
        // handed out again clears nothing of the successor's.
        void NoteRecordDestroyed(MGPipeHandle handle) {
            if (MGPipeHandleIsNull(handle)) return;
            const SizeT slot = handle.Slot;
            if (slot < m_viewLatch.size() && m_viewLatch[slot].RecordGen == handle.Gen) {
                m_viewLatch[slot] = ViewLatch{};
            }
        }

        // The validate point's FreshlyPrimed arm. It clears the PER-CONTEXT memo and NOTHING
        // ELSE, and the absence is the rule rather than an oversight (D-J4):
        // MGPipeApplierReset is a make-current, so it clears the three unit-set windows - whose
        // mirrors are the suppressor slots the validate point invalidates beside this call -
        // and it deliberately does NOT drop the object records. The sampler CSOs and the
        // sampler views are object records, so re-publishing them here would move their Serial
        // for nothing, and no P4a emitter may have a re-publication path.
        //
        // THE BOUND-SET REFERENCES DO GO, and that is not an exception to the rule above: they
        // are pins on WORKING STATE, not object records. MGPipeApplierReset clears the three
        // unit windows, so after this call no bind_sampler_states set names those CSOs any
        // more and holding their references would pin cache entries no record is entitled to.
        // The records themselves - the create_sampler_state the cache emitted - are untouched.
        void Reset() {
            MGPipeProgramOpaqueUnitsShared().Invalidate();
            MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
            for (MGPipeHandle& held : m_stateRefs) {
                cache.Release(held);
                held = kMGPipeNullHandle;
            }
        }

        void ResetCounters() { m_viewSets = m_stateSets = m_viewCreates = 0; }

        // ---- what a unit case reads. No copy: the emitter builds INTO these and hands the
        // applier the same pointers. ----
        const MGPSamplerViews& LastSamplerViews() const { return m_lastViews; }
        const Array<MGPBoundView, kMGPipeMaxTextureUnits>& LastBoundViews() const { return m_views; }
        const MGPSamplerStates& LastSamplerStates() const { return m_lastStates; }
        const Array<MGPipeHandle, kMGPipeMaxTextureUnits>& LastSamplerStateHandles() const { return m_states; }
        const MGPSamplerView& LastCreatedView() const { return m_lastView; }
        Uint64 ViewSetCount() const { return m_viewSets; }
        Uint64 StateSetCount() const { return m_stateSets; }
        Uint64 ViewCreateCount() const { return m_viewCreates; }

    private:
        struct ViewLatch {
            // "Does the applier hold a create_sampler_view record at this slot, for this
            // generation, describing this shape". Lives exactly as long as the record does -
            // see Reset() for why it is not cleared at a make-current.
            Bool RecordLive = false;
            Uint32 RecordGen = 0;
            Uint64 ShapeVersion = 0;
            Uint16 ParamsVersion = 0;
            MGPipeHandle Texture = kMGPipeNullHandle;
        };

        static constexpr Uint32 Min(Uint32 a, Uint32 b) { return a < b ? a : b; }

        Array<MGPBoundView, kMGPipeMaxTextureUnits> m_views{};
        Array<MGPipeHandle, kMGPipeMaxTextureUnits> m_states{};
        // THE REFERENCES bind_sampler_states IS HOLDING, one per unit (ID-17). Not the same
        // array as m_states: that one is the record's tail and is rebuilt from live context
        // state at every pass, while this one is the pin the applier's standing set is entitled
        // to and it only moves when a unit's CSO really does.
        Array<MGPipeHandle, kMGPipeMaxTextureUnits> m_stateRefs{};
        MGPSamplerViews m_lastViews{};
        MGPSamplerStates m_lastStates{};
        MGPSamplerView m_lastView{};

        Vector<ViewLatch> m_viewLatch;

        Uint64 m_viewSets = 0;
        Uint64 m_stateSets = 0;
        Uint64 m_viewCreates = 0;
    };

    inline MGPipeSamplerEmitter& MGPipeSamplerEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason - and this one is named in the
        // phase's own risk list: a new client singleton that held a frontend SharedPtr, or
        // that had a destructor an exit handler could run into a torn-down pipe, is the
        // exit-order UAF P3a closed. Heap-constructed and intentionally leaked at exit.
        static MGPipeSamplerEmitter* emitter = new MGPipeSamplerEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
