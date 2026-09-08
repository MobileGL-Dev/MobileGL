// MobileGL - MobileGL/MG_Impl/Pipe/SamplerEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's sampler family: the content-addressed sampler CSO cache, the
// identity-addressed sampler view per texture object, and the two unit sets
// set_sampler_views and bind_sampler_states. The third unit set, set_shader_images, is
// ImageEmit.h's - the same subsystem bit, a different resolution.
//
// TWO THINGS THIS FILE OWNS THAT ARE EASY TO GET WRONG, both stated where the body will go:
//   * SamplerParameters is 100 bytes with THREE BYTES OF TRAILING PADDING, so the CSO cache
//     hashes and memcmp-confirms over a ZERO-INITIALISED canonical copy built field by field,
//     never over the object's own bytes. Without that the 256-entry cache's hit rate is zero
//     and nobody notices, because the pixels are right.
//   * every emission goes through a VERSION-FIRST SKIP before it hashes anything: the sampler
//     view latches (params version, shape version) per handle, and the two sets latch their
//     SetHashSuppressor slots. A 192-entry walk per verb without a latch is not affordable.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full. kMGPipeWiredSamplerSubsystem below covers this file AND
// ImageEmit.h: the three unit sets, the sampler CSO and the sampler view are ONE family and
// one subsystem bit, because an operator switching samplers off has to get the whole family's
// legacy arm rather than two thirds of it.
//
// WHAT THIS FILE IS THE CLIENT HALF OF, named so a reader can check it against the oracle:
// DirectGLES' ResolveAndBindUnitTextures (the per-unit sampler-view resolution),
// BindCurrentUnitSamplers (the per-unit sampler-object walk) and the program pass' sampler
// override. TWO BACKEND POST-PROCESSINGS DELIBERATELY STAY ON THE SERVER and act on the
// RESOLVED set: Espryt's raw-depth-fetch sampler substitution and Magma's feedback-loop
// detection. Neither is reproduced here and neither may be.
//
// HEADER-ONLY, for the ownership reason Tracker.h and ResourceTracker.h both state.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHostSpan.h>
#include <MG_Pipe/PipeApply.h>
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

    // WIRED. The sampler CSO, the sampler view and all three unit sets - set_shader_images
    // included, whose emitter lives in ImageEmit.h - have bodies, so this file's family
    // contributes its bit to kMGPipeWiredSubsystems. ONE bit for the whole family, because an
    // operator switching samplers off has to get the whole family's legacy arm rather than two
    // thirds of it.
    //
    // WHAT THE BIT DOES, and it really is a gate - c0b changed this and the note that used to
    // say the opposite was true only against the contract commit. The validate point's
    // `wants()` (PipeFill.cpp) now asks FOUR things: the bit maps to a subsystem, the
    // operator's MOBILEGL_PIPE_PUSH mask carries it, `kMGPipeWiredSubsystems` carries it -
    // i.e. this constant is non-zero - and the dirty bit fired. The birth hooks' own gate
    // (`FamilyIsLive`) is the same pair one level in, so the client paths and the validate
    // point cannot disagree. So an emitter with a body and this constant still 0 is called by
    // nothing and emits nothing, and flipping the constant is what switches the family on.
    //
    // IT IS ALSO A COMPILE-TIME CONTRACT. While it is non-zero, PipeFill.cpp's `if constexpr`
    // seam instantiates the forward to this family's entry points - EmitSamplerCso and
    // EmitSamplerView below - so a missing or misspelled one is a build error in this file's
    // own commit rather than a surprise at the merge.
    //
    // The A/B that switches this family off at RUNTIME is still the mask; this constant is
    // what a build declares it emits for, and it feeds EmittedCallSuppliesTheWholeField's
    // guard. ONE bit for the whole family, for the reason above.
    inline constexpr Uint64 kMGPipeWiredSamplerSubsystem = kMGPipeSubsystemSamplers;

    // ---------------------------------------------------------------------------------
    // D-F1: the canonical SamplerParameters copy, and why it is not a memcpy
    // ---------------------------------------------------------------------------------
    //
    // sizeof(SamplerParameters) == 100 and its members occupy 97 of those bytes: six 4-byte
    // enums, four Floats, two more 4-byte enums, three 16-byte colour vectors and the one-byte
    // borderColorForm. Bytes 97, 98 and 99 are PADDING and no writer ever touches them.
    //
    // A cache that hashed or memcmp'd the object's own bytes would therefore read
    // uninitialised memory. In practice SamplerObject value-initialises its member and never
    // rewrites it, so in practice those bytes are stable - and "in practice" is not a
    // contract. The whole point of a content-addressed cache is that a false MISS mints a
    // fresh CSO per call: a 256-entry cache with a hit rate of zero, on a path nobody looks
    // at, because the pixels are right either way.
    //
    // So every hash and every confirm runs over a copy that is memset to zero FIRST and then
    // assigned field by field, which makes the padding deterministically zero on both sides of
    // the comparison. The field list is MGP_FIELDS_SamplerParameters', in its order, and the
    // verify build compares the same sixteen fields one at a time - without that
    // PipeFields.def row the blob would be compared as bytes and G4 would be a coin flip.
    //
    // ONE COPY PER MINT ATTEMPT, never per draw: the version-first skip in the two emitters
    // below decides whether to come here at all.
    // THE PRIMITIVE TAKES AN OUT-PARAMETER, and that is not a style preference either. A
    // returned SamplerParameters is copied, and a copy of a trivially copyable type leaves the
    // padding UNSPECIFIED - so a canonicaliser that returned by value would hand its caller a
    // value whose three trailing bytes are whatever the copy left there, which is the very
    // thing this function exists to make deterministic. Every producer of canonical bytes in
    // this file, and every consumer that stores them, goes through this and through memcpy.
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

    // ---------------------------------------------------------------------------------
    // D-F1: the content-addressed sampler CSO cache, capacity 256
    // ---------------------------------------------------------------------------------
    //
    // Unlike P3a's vertex-elements CSO, content addressing is RIGHT here: a SamplerObject is a
    // pure 100-byte value with no driver-side per-object binding state, two identical samplers
    // can share one CSO with no extra work on either side, and Espryt's BackendSamplerObject
    // is a driver name plus a parameter shadow - nothing a second frontend object would have
    // to re-establish.
    //
    // THE LOOKUP IS CsoCache.h's, one for one: hash, probe, and CONFIRM WITH A MEMCMP before
    // reusing a handle, because a bare 64-bit equality would alias two different sampler
    // states onto one CSO and that is silent wrong filtering with no gate that can see it.
    //
    // WHAT KEEPS A LIVE HANDLE OUT OF THE LRU's REACH IS A REFERENCE COUNT, not the capacity
    // (ID-17). The capacity argument - "one pass acquires at most kMGPipeMaxTextureUnits == 192
    // CSOs and touches every one of them, so LRU can only evict an entry from an EARLIER pass"
    // - closes the INTRA-PASS case only, and the case that matters is the other one: a
    // MGPTextureParams record published in an earlier pass names its texture's BuiltinSampler
    // for as long as that texture's parameters do not move, the applier deliberately does not
    // resolve that handle, and an eviction is not a parameter change, so nothing re-emits and
    // nobody refuses. With more than 256 distinct live sampler values - a CTS sampler sweep, a
    // scene with many filter/wrap/border combinations - the rarely-touched built-in samplers
    // are the FIRST victims.
    //
    // So every Acquire takes a REFERENCE and Release drops one; LRU considers only entries
    // whose count is 0; and when every entry is pinned the cache MINTS BEYOND CAPACITY and
    // counts it (Counters::OverCapacityMints). Growing is the safe direction - a slot too many
    // costs memory, a handle pulled out from under a live record costs correctness - and the
    // count is a recorded number rather than a gate, so a workload that pins more than 256
    // values is visible instead of being wrong.
    //
    // WHO HOLDS A REFERENCE: package B's EmitTextureParams for a texture's built-in sampler
    // (released when the texture dies or when a parameter change makes it re-acquire), and
    // this file's own bind_sampler_states for every CSO a unit is currently bound to. Anything
    // that acquires a handle it will still name after it returns must hold one.
    //
    // THE CAPACITY ITSELF IS STILL A SIZING BOUND worth asserting: a cache that could not hold
    // one whole emission pass would over-capacity-mint on every single pass, which is a
    // silently unbounded cache rather than a 256-entry one.
    //
    // THE SLOT IS ALLOCATED WITHOUT A LIFETIME ID, deliberately: a content-addressed CSO
    // belongs to a VALUE and not to a frontend object, so ~SamplerObject must not free it -
    // another live SamplerObject may hold the same value. MGPipeEmitSamplerCsoDestroyAndFree
    // resolves nothing for such an id and correctly frees nothing; the only death path for
    // these slots is the LRU eviction below, which is client-side and therefore
    // backend-neutral on day one.
    //
    // PACKAGE D MUST BE TOLD, and it is the one consequence of that choice that reaches the
    // server: NotifyStateObjectDestroyed(SamplerCso, lifetimeId) arrives from ~SamplerObject
    // with NO HANDLE BEHIND IT, every time. A backend must NOT key a sampler twin on a
    // SamplerObject's lifetime id. The twin's life is create_sampler_state -> the LRU's
    // delete_sampler_state and nothing else.
    inline constexpr SizeT kMGPipeSamplerCsoCacheCapacity = 256;
    static_assert(kMGPipeSamplerCsoCacheCapacity > kMGPipeMaxTextureUnits,
                  "a cache that cannot hold one whole emission pass would mint over capacity on "
                  "every pass; correctness against eviction is the reference count, not this bound");

    class MGPipeSamplerCsoCache {
    public:
        struct Counters {
            Uint64 Mints = 0;       // create_sampler_state emissions
            Uint64 Acquisitions = 0;
            Uint64 Hits = 0;        // a probe that found a live entry and passed the memcmp
            Uint64 Collisions = 0;  // a hash hit the memcmp REJECTED - the reason it exists
            Uint64 Evictions = 0;   // LRU evictions, each one a delete_sampler_state
            Uint64 Releases = 0;    // reference drops
            // Mints made with the cache already full and EVERY entry referenced. A recorded
            // number and not a gate (ID-17): growing past 256 is the safe direction, and this
            // is what makes it visible instead of silent.
            Uint64 OverCapacityMints = 0;
        };

        // The handle for `params`' value, AND A REFERENCE ON IT. Mints and emits
        // create_sampler_state on a miss and emits delete_sampler_state for whatever it evicts
        // to make room. `payloadBytes` accumulates what went on the wire.
        //
        // EVERY Acquire TAKES A REFERENCE and every caller owes exactly one Release for it
        // (ID-17). A caller that only wants a transient handle still has to release it, and a
        // caller that keeps naming the handle in a published record must hold its reference for
        // as long as that record stands - that is what stops the LRU pulling the handle out
        // from under it, because an eviction is not a parameter change and nothing re-emits.
        //
        // THIS IS ALSO PACKAGE B's SEAM. MGPTextureParams::BuiltinSampler names the CSO that
        // carries the SamplerParameters of the SamplerObject every ITextureObject owns, and a
        // null handle there is Fatal{ProtocolCorruption} rather than "no sampler". TextureEmit.h
        // acquires it from here - taking a reference it holds until the texture dies or a
        // parameter change makes it re-acquire, at which point it releases the previous handle -
        // so the texture's built-in sampler and a glBindSampler'd sampler object with the same
        // value share one CSO and one server-side twin, which is exactly the sharing that makes
        // content addressing the right answer for this kind.
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
        MGPipeHandle AcquireWithForcedHashForTest(const SamplerParameters& params, Uint64 forcedHash,
                                                  Uint64& payloadBytes) {
            SamplerParameters canon;
            MGPipeCanonicaliseSamplerParameters(params, canon);
            return AcquireCanonical(forcedHash, canon, payloadBytes);
        }

        // Drops one reference. The entry stays - a released value is still worth reusing - it
        // merely becomes eligible for the LRU again. Releasing a handle this cache never handed
        // out, or releasing one twice, is a no-op rather than an underflow: the death paths that
        // call it are idempotent by contract and a second call must not corrupt the count.
        void Release(MGPipeHandle handle) {
            if (MGPipeHandleIsNull(handle)) return;
            for (Entry& entry : m_entries) {
                if (entry.Cso != handle) continue;
                if (entry.RefCount > 0) --entry.RefCount;
                ++m_counters.Releases;
                return;
            }
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

        // A unit test's fixture, and nothing else. NOT called from the validate point's
        // FreshlyPrimed arm: MGPipeApplierReset is a make-current and does NOT drop object
        // records, so a sampler CSO the applier holds outlives a context switch. Dropping the
        // cache there would leak the applier's record and re-mint a value it already has.
        //
        // IT DROPS THE PUBLICATION LATCH FOR EVERY ENTRY IT FREES. The slots go back with no
        // delete_sampler_state behind them, so a latch left standing would make a future death
        // helper - on the recycled slot at the same generation - emit a delete for a record
        // that never existed, which is the refusal the applier counts.
        void ResetForTest() {
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
        };

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
            MGPipeApplyCreateSamplerState(desc, &m_entries.back().Params);
            // THE CREATE ACTUALLY WENT OUT, so the publication latch is taken here and nowhere
            // else (contract-v2 §3.1). It is what the six death helpers read, and without it a
            // delete_sampler_state can never go out for this kind.
            MGPipeNoteHandlePublished(MGPipeKind::SamplerCso, cso);

            ++m_counters.Mints;
            payloadBytes += sizeof(MGPSamplerDesc) + sizeof(SamplerParameters);
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::CsoBlobBytes,
                                             sizeof(SamplerParameters));
            }
            return cso;
        }

        // ONLY EVER CALLED WITH AN UNREFERENCED ENTRY - see Mint's victim choice. That is the
        // whole of ID-17's ruling: a handle a standing record names cannot be taken, because
        // the applier does not resolve MGPTextureParams::BuiltinSampler and an eviction is not
        // a parameter change, so nothing would refuse and nothing would re-emit.
        void Evict(SizeT index) {
            MOBILEGL_ASSERT(m_entries[index].RefCount == 0,
                            "a referenced sampler CSO was evicted; the LRU must skip pinned entries");
            MGPHandleOnly handle{};
            handle.Handle = m_entries[index].Cso;
            handle.Kind = static_cast<Uint32>(MGPipeKind::SamplerCso);
            // THE THREE-STEP ORDER, the same one PipeMutation.h fixes for the death helpers:
            // the wire delete drops the applier's record while the record still exists, and
            // only then does the slot go back. There is no NotifyStateObjectDestroyed step
            // here - a content-addressed CSO has no frontend object whose death is being
            // announced, which is precisely why this eviction is the only death path it has.
            MGPipeApplyDeleteSamplerState(handle);
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
    // on the sampler uniform's TYPE. Gallium's one-view-per-slot is the resolved form, so the
    // resolution moves to the client and the record carries the answer rather than the inputs.
    //
    // This is DirectGLES' SamplerUniformTextureTarget, moved to the side of the boundary that
    // now owns the question. Targets with no sampler spelling map to Unknown, and a unit whose
    // uniform type resolves to Unknown carries no view - which is exactly "the program does not
    // sample this unit".
    inline MobileGL::TextureTarget MGPipeSamplerUniformTextureTarget(GLenum uniformType) {
        switch (uniformType) {
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
                    const TextureTarget target =
                        MGPipeSamplerUniformTextureTarget(program->GetUniformType(location));
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

        // set_sampler_views: the PROGRAM-RESOLVED set only, one entry per unit, no stage
        // dimension. Start is 0 and Count is GetMaxTouchedTextureUnit() + 1 clamped to the
        // wire bound - the high-water mark is directly the count argument and is not
        // re-derived.
        //
        // WHAT A UNIT'S ENTRY MEANS, and all three cases are legal rather than holes:
        //   the program resolves no target here   -> View and Texture both null
        //   it resolves one, nothing is bound     -> View and Texture both null
        //   it resolves one and a texture is bound-> Texture is that object's handle and View
        //                                            is its sampler-view CSO
        // An UNDEFINED DEFAULT texture (name 0 with no image) and a texture that
        // SAMPLES AS INCOMPLETE are both dropped to null here, which is the resolution
        // DirectGLES performs by leaving the native target unbound: an incomplete texture
        // samples (0,0,0,1) and the driver cannot work that out for itself, because the
        // backend storage is immutable and never saw the level the application redefined at
        // the wrong size.
        Uint64 EmitSamplerViews(GLContext& ctx) {
            const Int maxTouched = ctx.GetMaxTouchedTextureUnit();
            const Uint32 count =
                maxTouched < 0 ? 0u
                               : Min(static_cast<Uint32>(maxTouched) + 1u, kMGPipeMaxTextureUnits);

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
                const SamplerObject* effective =
                    unitSampler ? unitSampler.get() : texture->GetSamplerObject().get();
                if (MG_State::GLState::SamplesAsIncompleteTexture(texture.get(), effective)) continue;

                entry.Texture = MGPipeSlots().Acquire(MGPipeKind::Texture, texture->GetLifetimeId());
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
            MGPipeApplySetSamplerViews(m_lastViews, m_views.data());
            ++m_viewSets;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::SamplerViewEmissions, 1);
            }
            return bytes + sizeof(MGPSamplerViews) + static_cast<Uint64>(count) * sizeof(MGPBoundView);
        }

        // bind_sampler_states: the unit's sampler CSO, or the null handle when the unit has no
        // sampler object - the texture's built-in sampler then applies, exactly as today.
        //
        // NOT program-resolved, and that asymmetry with the view set above is deliberate: a
        // sampler object is bound to a UNIT and applies to whatever the unit holds, so its set
        // is the plain per-unit walk BindCurrentUnitSamplers already does. A redundant re-bind
        // of the same sampler object emits nothing at all, which is the whole point of the
        // suppressor slot: GetTextureBindGeneration() bumps on a redundant re-bind - 26.2
        // rebinds the same sampler at every texture-unit switch - so without it this would be
        // a several-hundred-byte variable-length record per batch.
        // A BOUND SAMPLER STATE HOLDS A REFERENCE ON ITS CSO FOR AS LONG AS IT IS BOUND
        // (ID-17). The set the applier holds is "the last set as received" and outlives the
        // pass that sent it, so a handle in it must not become the LRU's victim; the reference
        // is what makes that true. The reconciliation below is one pass over the unit array:
        // Acquire has already taken a reference for every unit in the new set, so a unit whose
        // handle did not move gives that duplicate back, a unit whose handle moved gives back
        // the one it used to hold, and a unit that fell outside the window gives back its own.
        Uint64 EmitSamplerStates(GLContext& ctx) {
            const Int maxTouched = ctx.GetMaxTouchedTextureUnit();
            const Uint32 count =
                maxTouched < 0 ? 0u
                               : Min(static_cast<Uint32>(maxTouched) + 1u, kMGPipeMaxTextureUnits);

            Uint64 bytes = 0;
            MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
            for (Uint32 unit = 0; unit < count; ++unit) {
                const auto& sampler = ctx.GetTextureUnitObject(static_cast<Int>(unit)).GetSamplerObject();
                m_states[unit] =
                    sampler ? cache.Acquire(sampler->GetAllSamplerParameters(), bytes) : kMGPipeNullHandle;
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
            MGPipeApplyBindSamplerStates(m_lastStates, m_states.data());
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
                MGPipeSlots().Acquire(MGPipeKind::SamplerViewCso, texture.GetLifetimeId());
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
            m_lastView.Target = static_cast<Uint8>(texture.GetTarget());
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
            MGPipeApplyCreateSamplerView(m_lastView);
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

        // ---- THE TWO CONTRACT ENTRY POINTS THIS FAMILY OWES (contract-v2 §3.4) ----
        //
        // PipeFill.cpp's birth hooks forward here through an `if constexpr` seam keyed on
        // kMGPipeWiredSamplerSubsystem, so while that constant is non-zero these two must
        // exist and be spelled exactly like this or the build fails in this file's own commit.
        // The hook has already applied BOTH gates (the operator's mask and the wired constant);
        // what belongs here is the family's HANDLE RULE and its publication, which the contract
        // deliberately does not pick for us because the three families disagree about identity.
        //
        // MINTING A SAMPLER CSO AT CONSTRUCTION TIME CONTENT-ADDRESSES THE DEFAULT PARAMETERS,
        // and that is correct rather than wasteful: the cache dedupes, so a thousand
        // freshly-created SamplerObjects share ONE create_sampler_state, and the moment the
        // application sets a parameter the next acquire content-addresses the new value and
        // this object simply stops naming the default entry. It is one create_sampler_state per
        // distinct default-valued sampler, not one per object.
        //
        // THE REFERENCE THIS TAKES IS DROPPED IMMEDIATELY. A birth is not a standing record: no
        // MGPTextureParams and no bind_sampler_states names this handle yet, so pinning the
        // entry here would keep the default-parameter value un-evictable for the life of the
        // process for no reader's benefit. Whoever later names the handle in a record takes its
        // own reference through its own Acquire.
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
            const MGPipeHandle textureHandle =
                MGPipeSlots().Acquire(MGPipeKind::Texture, texture.GetLifetimeId());
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
        // NO PRODUCTION CALLER TODAY, and that is stated rather than implied: the death path
        // goes through the contract's latch, not through here. It is kept because the memo
        // above needs a way to be told, and because leaving the latch standing SELF-HEALS
        // anyway - the slot's Gen moves on reuse, so the `RecordGen == handle.Gen` test in
        // RecordIsPublished and in AcquireSamplerView already refuses a stale entry.
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
        const Array<MGPipeHandle, kMGPipeMaxTextureUnits>& LastSamplerStateHandles() const {
            return m_states;
        }
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
#endif // MOBILEGL_PIPE_PUSH
