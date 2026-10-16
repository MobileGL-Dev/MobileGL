// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESTextures.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace TextureImpl {
        // Identity snapshot of what one texture unit has bound: the object in every binding
        // slot plus the unit's sampler object. This - not the context's texture bind
        // generation - is what the per-draw texture memos key on: the generation also bumps
        // on REDUNDANT re-binds (glBindSampler of the sampler the unit already carries, which
        // 26.2 issues around every texture-unit switch), so a generation-keyed memo re-derives
        // everything on draws that changed nothing. The snapshot compares the bindings
        // themselves, so only a real change can invalidate it. What it deliberately does NOT
        // cover, and its users must key on separately:
        //   * membership/completeness flips with no binding moving - a default texture's image
        //     appearing or vanishing, any texture shape or sampler parameter change - all bump
        //     the sampling-resolution generation (BumpShapeVersion / SamplerObject::BumpVersion
        //     are their only writers and bump it unconditionally);
        //   * everything unit bindings say nothing about: the touched-unit high-water mark,
        //     the frontend context identity, the backend ES context generation, and (for the
        //     resolution memo) the program keys that arbitrate aliased targets.
        // P2: the snapshot stops holding weak_ptrs and holds the frontend objects' LIFETIME IDs.
        // The weak_ptr was here for one reason - a raw address lies once the allocator recycles
        // it - and a lifetime id is a monotone per-class counter that is never handed out twice,
        // so it answers the same question with an integer compare and without pinning a control
        // block. 0 means "nothing bound", which no live object can collide with (the counters
        // start at 1). This is not the twin table's {slot, gen}: a bound texture that has never
        // been synced has no twin and therefore no handle, so a handle-keyed snapshot would read
        // two never-synced textures as equal. The identity has to exist before the twin does.
        //
        // Split by the RUNTIME arm, not by the build, for exactly the reason g_fbSlotCache is:
        // MOBILEGL_PIPE_PUSH=0 has to reproduce P1's behaviour (ConfigLoader.cpp), and a
        // legacy-arm run that debounced on lifetime ids would be running P2's mechanism while
        // the A/B attributed the result to P1. The two answers are equivalent - OwnerEquals on
        // two empty pointers is true and LifetimeIdOf(nullptr) == 0 == 0; a live-versus-expired
        // control block and two distinct lifetime ids both compare unequal - so keeping the
        // legacy fields costs that arm nothing but the words, and gives the control back its
        // fidelity. A build with no legacy arm compiled carries neither the fields nor the
        // branch.
        struct UnitBindingsSnapshot {
            Array<Uint64, (SizeT)TextureTarget::TextureTargetCount> slotObjects{};
            Uint64 samplerObject = 0;
        };

        static Uint64 LifetimeIdOf(const SharedPtr<MG_State::GLState::ITextureObject>& object) {
            return object ? object->GetLifetimeId() : 0;
        }

        static Uint64 LifetimeIdOf(const SharedPtr<MG_State::GLState::SamplerObject>& object) {
            return object ? object->GetLifetimeId() : 0;
        }

        static void CaptureUnitBindings(Int maxTouchedUnit, Vector<UnitBindingsSnapshot>& out) {
            out.resize(static_cast<SizeT>(maxTouchedUnit + 1));
            for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
                auto& textureUnit = MG_Pipe::gPipeInputs->GetTextureUnitObject(unit);
                auto& snapshot = out[static_cast<SizeT>(unit)];
                const auto& slots = textureUnit.GetAllBindingSlots();
                for (SizeT i = 0; i < slots.size(); ++i) {
                    snapshot.slotObjects[i] = LifetimeIdOf(slots[i].GetBoundObject());
                }
                snapshot.samplerObject = LifetimeIdOf(textureUnit.GetSamplerObject());
            }
        }

        static Bool UnitBindingsUnchanged(Int maxTouchedUnit, const Vector<UnitBindingsSnapshot>& snapshots) {
            if (snapshots.size() != static_cast<SizeT>(maxTouchedUnit + 1)) return false;
            for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
                auto& textureUnit = MG_Pipe::gPipeInputs->GetTextureUnitObject(unit);
                const auto& snapshot = snapshots[static_cast<SizeT>(unit)];
                const auto& slots = textureUnit.GetAllBindingSlots();
                for (SizeT i = 0; i < slots.size(); ++i) {
                    if (snapshot.slotObjects[i] != LifetimeIdOf(slots[i].GetBoundObject())) return false;
                }
                if (snapshot.samplerObject != LifetimeIdOf(textureUnit.GetSamplerObject())) return false;
            }
            return true;
        }

        static Vector<UnitBindingsSnapshot> g_observedUnitBindings;
        static Uint64 g_observedUnitBindingsContextId = 0;
        static Uint64 g_observedUnitBindingsGeneration = 0;
        static Int g_observedUnitBindingsMaxUnit = -1;
        static Uint64 g_unitBindingsEpoch = 0;

        // Epoch of the touched units' bindings: moves exactly when WHAT is bound changes,
        // never on a redundant re-bind. Memos snapshot the returned value instead of holding
        // their own UnitBindingsSnapshot, so the owner-compare walk runs at most once per
        // draw no matter how many memos key on it. The observation triple gates the walk:
        // while (context id, bind generation, high-water mark) are unchanged nothing can
        // have moved, because every path that changes a binding bumps the generation. A
        // given epoch value names one observed (context, high-water mark, bindings) state -
        // a context switch, a high-water-mark move or a bindings change each recapture and
        // bump - so epoch equality alone proves the bindings a consumer resolved against
        // are the bindings on the units now.
        // P4a e2 (D-G1). THE UNIT-BINDINGS EPOCH DERIVATION, REPLACED BY THE RECORDS' SERIALS.
        //
        // CurrentUnitBindingsEpoch below exists to answer one question - "did WHAT the touched
        // units hold change?" - without re-deriving on a redundant re-bind, and it pays for the
        // answer with a per-unit snapshot walk over every binding slot plus the unit's sampler
        // object, run whenever the frontend's texture bind generation moves (26.2 moves it
        // around every texture-unit switch). The applier already holds the answer:
        // set_sampler_views and bind_sampler_states go out through a ContentHash suppressor,
        // so their serials move IF AND ONLY IF the resolved per-unit view set or the per-unit
        // sampler set really moved. Two Uint64 reads replace the walk, and the suppressor is
        // what makes that sound - the hash is over every entry of each tail, so suppression
        // cannot hide a change.
        //
        // BOTH serials, because the snapshot covered both halves: slotObjects is the view set
        // and samplerObject is the sampler-state set.
        //
        // DECLINES until both windows have arrived at least once, and the COUNT is what says
        // so rather than the serial: MGPipeApplierReset advances every P4a working-state
        // serial whether or not anything was ever emitted, so a serial is never "no set has
        // been received". A declined answer leaves the caller on the snapshot walk, which is
        // the honest thing for a tree whose client half has not landed.
        //
        // The two arms' epoch values share one number space during the transition (a process
        // can run the walk before the first emission and the serials after it). That is safe
        // by construction: consumers only ever ask "is this the same value I stamped", the
        // walk's counter is small and dense and this one is a 64-bit mix, so the two cannot
        // meet except with vanishing probability - and a collision costs a spare rebuild.
        static Bool UnitBindingsEpochFromRecords(Uint64& out, Int maxTouchedUnit) {
            const auto& st = MG_Pipe::MGPipeApplier();
            // P4a decline-site T2: S - FLIPPED AT THE VERIFICATION ROUND to loud-once, then
            //   the decline, and MEASURED before it was believed. Bits 10 and 11 are on (the
            //   caller asked SamplerSubsystemEnabled(), which refuses one without the other)
            //   and package C has landed, so a draw that reaches here with NEITHER window ever
            //   received looks like a seam defect - but the first real-path run said otherwise
            //   for one whole class of draw and the condition is narrowed by that measurement,
            //   not by argument.
            //
            //   maxTouchedUnit < 0 IS "THIS DRAW TOUCHES NO TEXTURE UNIT AT ALL", and for such
            //   a draw an empty sampler-view and sampler-state window is the correct and only
            //   possible emission: there is nothing to describe. 161 of the integration lane's
            //   477 processes are that shape, every one of them at units 0..-1, and a line that
            //   fires on a third of a green lane is a line nobody reads. So the refusal is
            //   scoped to a draw that actually touches a unit, where an empty window really
            //   does mean no set_sampler_views and no bind_sampler_states ever arrived.
            //
            //   It declines to the snapshot walk rather than refusing the draw, because the
            //   walk answers the same question correctly and a wrong picture is not the failure
            //   mode here - a silent permanent fallback is, and that is what the line is for.
            //   The caller's MINOR-4 gate tick still counts EVERY decline, loud or not.
            if (st.SamplerViewCount == 0 && st.SamplerStateCount == 0) {
                // P5e (tx2), CONTRACT-P5E §5.3: UNDER A LIVE WIRE THE DECLINE IS A REFUSAL.
                //
                // The fall-back below is the pre-handle snapshot walk over GetTextureUnitObject,
                // and on a run-ahead server that walk reads client memory for a record that has
                // already been answered - there is no wait left in which the answer could be
                // right. Ruling 19 makes the missing window unrepresentable at the sink; this is
                // the same statement one level down, for the backend that would have papered over
                // it. Under monolith the fall-back is correct and stays.
                if (maxTouchedUnit >= 0) {
                    // The windows are the PEER's records, so in a served session this ends THAT
                    // session (the in-process display server serves every client from one
                    // process; an abort here took the compositor down with the offender). The
                    // answer while the latch drains is "no unit is bound": the walk the decline
                    // would run reads client memory, and the session stops at its next record.
                    // Unarmed, SessionLatch is the old abort (P13 W5: through the record latch seam,
                    // which is SessionLatch wherever MG_Remote is linked).
                    if (!MG_Pipe::MGPipeRecordLatch(
                            MG_Pipe::MGFatalFamily::ProtocolCorruption,
                            "MGPipe: Fatal{ProtocolCorruption, \"SetSamplerViews.Count\"} - a draw "
                            "touches units 0..%d and neither a sampler-view nor a sampler-state "
                            "window has ever been applied while the sampler subsystem bit is set; "
                            "the pre-handle unit-bindings snapshot walk reads client memory and is "
                            "refused under an active transport (CONTRACT-P5E.md §5.3)",
                            static_cast<int>(maxTouchedUnit))) {
                        out = 0;
                        return true;
                    }
                }
                if (maxTouchedUnit >= 0) {
                    MGLOG_E_ONCE("A sampler record does not describe the binding it names: a draw "
                                 "touches units 0..%d and neither a sampler-view nor a sampler-state "
                                 "window has ever been applied while the sampler subsystem bit is "
                                 "set; running the pre-handle unit-bindings snapshot walk.",
                                 static_cast<int>(maxTouchedUnit));
                }
                return false;
            }
            // Local, because the tracker's MGPipeMixShutter lives in MG_Impl and no backend
            // translation unit may reach for it.
            const Uint64 mixed =
                (st.SamplerViewsSerial * 0x9E3779B97F4A7C15ull) ^ (st.SamplerStatesSerial + 0x165667B19E3779F9ull);
            out = mixed;
            return true;
        }

        static Uint64 CurrentUnitBindingsEpoch(Int maxTouchedUnit) {
            // P4a decline-site T1: M - the mask says this family is not switched on, and it
            //   IS D's SamplerSubsystemEnabled() now. Silent, confirmed at the verification
            //   round: the mask's word is the one thing a decline may be quiet about.
            Uint64 epochFromRecords = 0;
            if (UnitBindingsEpochFromRecords(epochFromRecords, maxTouchedUnit)) {
                // No accessor reads and no walk on this arm; the two PipeStats accessor
                // ticks below are not counted because no accessor was called.
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::EsprytUnitBindingsEpoch,
                                                  /*hit=*/true);
                }
                return epochFromRecords;
            }
            // MINOR-4. THE RECORD ARM'S DECLINE IS TICKED AS A MISS, and it has to be:
            // the walk arm below ticks the SAME gate with its own hit/miss, so without
            // this a fully declining tree reports the walk memo's high hit rate on
            // Gate::EsprytUnitBindingsEpoch and the verification round's "hit rate goes to
            // 100%" cannot tell "the record arm engaged" from "the walk arm's memo hit".
            // With it, every declining call contributes at least one miss, so 100% means
            // exactly one thing.
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::EsprytUnitBindingsEpoch,
                                              /*hit=*/false);
            }
            const Uint64 contextId = MG_Pipe::gPipeInputs->GetTextureContextId();
            const Uint64 bindGeneration = MG_Pipe::gPipeInputs->GetTextureBindGeneration();
            if (MG_Util::PipeStats::Enabled()) {
                // Two accessor calls whichever way the shutter goes; only the unit WALK is
                // gated, and that walk reads no GLContext accessor of its own.
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 2);
            }
            if (g_observedUnitBindingsContextId == contextId && g_observedUnitBindingsMaxUnit == maxTouchedUnit &&
                g_observedUnitBindingsGeneration == bindGeneration) {
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::EsprytUnitBindingsEpoch, /*hit=*/true);
                }
                return g_unitBindingsEpoch;
            }
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::EsprytUnitBindingsEpoch, /*hit=*/false);
            }
            if (g_observedUnitBindingsContextId != contextId || g_observedUnitBindingsMaxUnit != maxTouchedUnit ||
                !UnitBindingsUnchanged(maxTouchedUnit, g_observedUnitBindings)) {
                CaptureUnitBindings(maxTouchedUnit, g_observedUnitBindings);
                ++g_unitBindingsEpoch;
            }
            g_observedUnitBindingsContextId = contextId;
            g_observedUnitBindingsMaxUnit = maxTouchedUnit;
            g_observedUnitBindingsGeneration = bindGeneration;
            return g_unitBindingsEpoch;
        }

        // Work list behind SyncNeccessaryTextures' per-draw unit walk. WHICH textures the
        // touched units hold is a pure function of the unit bindings, so the GLContext identity
        // (a never-reused id, not the heap address a recreated context can land on again), the
        // unit-bindings snapshot and the touched-unit high-water mark are a complete key -
        // WHAT each entry then has to do is still decided per draw by the version compares
        // inside the sync calls, which is why texture content, shape and parameter changes need
        // no key here.
        //
        // Entries borrow, they never own. `slot` points at the binding slot's shared_ptr, whose
        // address is fixed for the context's lifetime (TextureState holds the unit array by
        // value) and whose VALUE cannot change without bumping the bind generation. `backend` is
        // the registry's object for the texture in that slot; the registry only erases an entry
        // once the frontend texture has expired, and a frontend texture cannot expire while a
        // slot the key covers still holds a reference to it. Holding either side by shared_ptr
        // instead would keep dead frontend textures alive and defeat the registry's
        // weak-reference GC.
        //
        // `texture` records WHICH frontend object `backend` was paired with when the entry was
        // built, and PairingsIntact re-checks it before any replay. The keys above are the
        // primary guard, but they are all derived state: a slot swap that never reaches the
        // bind generation (the DSA by-name emulation used to swap a slot silently) would leave
        // every key matching while the borrowed slot pointed at a different texture, and the
        // replay would then drive texture A's backend twin from texture B's frontend state -
        // re-specifying A's backend storage with B's shape and destroying A's contents. A raw
        // pointer compare per entry is far cheaper than the walk it guards, and a stale pairing
        // costs only a list rebuild, so this stays as the structural net under the keys.
        struct UnitTextureSyncEntry {
            const SharedPtr<MG_State::GLState::ITextureObject>* slot = nullptr;
            MG_State::GLState::ITextureObject* texture = nullptr;
            BackendTextureObject* backend = nullptr;
        };
        // P5e (tx2), CONTRACT-P5E §5.2: THE UNIT WORK LIST WITH NO BORROWED SLOT IN IT.
        //
        // The pre-P5e entry borrows a pointer INTO a frontend binding slot and remembers which
        // frontend object it was paired with, and PairingsIntact re-checks that pairing before
        // every replay. Both exist because the list's key is derived state and a silent slot swap
        // could leave a stale pairing driving texture A's twin from texture B's frontend state.
        //
        // A HANDLE HAS NO SUCH HAZARD. `Res` is {slot, gen}: a recycled slot arrives at a higher
        // Gen, which the twin table's forward-Gen rule answers by RESETTING the twin, and a
        // backward Gen is refused outright - so there is nothing for a pairing check to catch and
        // PairingsIntact is not carried on this list at all. The key is the applier's own
        // (ContextSerial, SamplerViewsSerial, window) plus the backend context generation, and
        // SamplerViewsSerial moves IFF the resolved per-unit view set moved, which the emitter's
        // content-hash suppressor is what makes sound.
        //
        // Its OWN list and its own key rather than a re-shape of UnitTextureSyncEntry, because
        // that struct is also the draw-FBO and read-FBO attachment lists' (fb's), and one name
        // carrying two shapes across two packages' worktrees is the merge trap the contract
        // package exists to avoid.
        struct UnitTextureSyncByHandleEntry {
            MG_Pipe::MGPipeHandle Res = MG_Pipe::kMGPipeNullHandle;
            BackendTextureObject* backend = nullptr;
        };
        static Vector<UnitTextureSyncByHandleEntry> g_unitTextureSyncListByHandle;
        static Bool g_unitTextureSyncListByHandleValid = false;
        static Uint64 g_unitTextureSyncListByHandleContextSerial = 0;
        static Uint64 g_unitTextureSyncListByHandleViewsSerial = 0;
        static Uint32 g_unitTextureSyncListByHandleStart = 0;
        static Uint32 g_unitTextureSyncListByHandleCount = 0;
        static Uint g_unitTextureSyncListByHandleContextGeneration = 0;
        static Vector<UnitTextureSyncEntry> g_unitTextureSyncList;
        static Bool g_unitTextureSyncListValid = false;
        static Uint64 g_unitTextureSyncListContextId = 0;
        static Int g_unitTextureSyncListMaxUnit = -1;
        static Uint g_unitTextureSyncListContextGeneration = 0;
        static Uint64 g_unitTextureSyncListEpoch = 0;
        static Uint64 g_unitTextureSyncListSamplingGeneration = 0;

        // P5e (fb, CONTRACT-P5E.md §5.4): THE TWO ATTACHMENT LISTS GET AN ENTRY OF THEIR OWN,
        // and it borrows nothing.
        //
        // The unit list's entry borrows the binding slot's SharedPtr and re-checks the pairing
        // before every replay, because its key (the unit-bindings epoch) is derived state that
        // a silent slot swap could get past. An attachment list under the record arm has
        // neither problem and cannot afford the borrow: the key IS the record - {Fbo,
        // ContentHash} - and every attachment edit moves the hash by construction, so the
        // pairing has nothing left to catch; and the borrowed pointer was into a frontend
        // FramebufferObject, which is exactly the thing this package stops holding. So an entry
        // is the texture HANDLE the record named plus the twin it resolved to, PairingsIntact
        // becomes `entry.Res == surface.Res` folded into the rebuild, and the list owns no
        // reference to anything the client can free.
        //
        // The twin pointer is still a BORROW, on the unit list's own terms: the registry keeps
        // a twin alive until its object dies, an attached texture cannot die while the record
        // that names it stands, and the very next thing done with the entry is a by-handle sync
        // that would re-resolve it anyway.
        struct FboAttachmentSyncEntry {
            MG_Pipe::MGPipeHandle Res = MG_Pipe::kMGPipeNullHandle;
            BackendTextureObject* backend = nullptr;
        };

        // Sibling memo for the draw FBO's texture attachments (see the use site in
        // SyncNeccessaryTextures for the key derivation and the borrow rules, which are the
        // unit list's). A null FBO pointer means "not stamped".
        static Vector<UnitTextureSyncEntry> g_fboTextureSyncList;
        static MG_State::GLState::FramebufferObject* g_fboTextureSyncListFbo = nullptr;
        static Uint16 g_fboTextureSyncListSlotVersion = 0;
        static Uint16 g_fboTextureSyncListObjectVersion = 0;
        static Uint64 g_fboTextureSyncListContextId = 0;
        static Uint g_fboTextureSyncListContextGeneration = 0;
        // P4a e2. The record-keyed half of the same memo. The draw framebuffer record's
        // ContentHash replaces the two VERSION halves of the key - the hash is taken over
        // every field the record carries, so an attachment edit always moves it, and Fbo is
        // inside it, so a recycled framebuffer handle cannot be suppressed against its
        // predecessor. THE IDENTITY HALF IS STILL A POINTER COMPARE and deliberately stays
        // one: this list borrows the attachment's own SharedPtr slots, so what it needs to
        // know is that the frontend object it borrowed from is the one bound now, and that
        // question has no handle in it until package D re-keys the twin tables. Held in its
        // own fields with its own flag rather than reusing the versions', because a process
        // runs the frontend key before the client's first emission and the record key after
        // it, and one field carrying two key shapes is how a stale half gets compared against
        // a live one.
        static Uint64 g_fboTextureSyncListContentHash = 0;
        static Bool g_fboTextureSyncListRecordKeyed = false;
        // P5e (fb): the record-keyed list itself, held apart from the pre-handle one for the
        // reason the two key shapes are held apart - one field carrying two shapes is how a
        // stale half gets compared against a live one. Its key is
        // {Fbo, ContentHash, ContextSerial, g_backendContextGeneration}: the applier's
        // ContextSerial replaces GetTextureContextId (a frontend read the record arm may not
        // take), and the two version halves are gone because the hash covers them.
        static Vector<FboAttachmentSyncEntry> g_fboAttachmentSyncList;
        static MG_Pipe::MGPipeHandle g_fboAttachmentSyncListFbo = MG_Pipe::kMGPipeNullHandle;
        static Uint64 g_fboAttachmentSyncListContentHash = 0;
        static Uint64 g_fboAttachmentSyncListContextSerial = 0;
        static Uint g_fboAttachmentSyncListContextGeneration = 0;
        static Bool g_fboAttachmentSyncListValid = false;

        // The read framebuffer's, same shape and same reasons. Its own list because it is
        // keyed on a different framebuffer and deduped against the draw one.
        static Vector<FboAttachmentSyncEntry> g_readFboAttachmentSyncList;
        static MG_Pipe::MGPipeHandle g_readFboAttachmentSyncListFbo = MG_Pipe::kMGPipeNullHandle;
        static Uint64 g_readFboAttachmentSyncListContentHash = 0;
        static Uint64 g_readFboAttachmentSyncListContextSerial = 0;
        static Uint g_readFboAttachmentSyncListContextGeneration = 0;
        static Bool g_readFboAttachmentSyncListValid = false;

        // Rebuild-or-replay, shared by both lists. The record's texture surfaces ARE the
        // membership (§5.4); a renderbuffer-only framebuffer - the common Minecraft frame -
        // reduces to the key compare and an empty loop.
        //
        // ONE CALL PER ENTRY, and it is tx2's by-handle seam rather than the three the
        // pre-handle arm makes: SyncTextureToBackendByHandle is the by-handle twin of
        // SyncTextureObjectToBackend, which is what the REBUILD has always called here, so the
        // params / builtin-sampler / mipmap work and its own clean gate are inside it. Calling
        // the three separately would need by-handle forms of the first two that nothing
        // declares, and would put this file's copy of the clean gate beside tx2's.
        static void SyncRecordAttachmentTextures(const MG_Pipe::MGPFramebufferState& record,
                                                 Vector<FboAttachmentSyncEntry>& list, Bool listValid) {
            if (listValid) {
                for (const auto& entry : list) {
                    // P14: the unit list's aggregate clean gate first (SyncNeccessaryTextures): an
                    // attachment whose storage, parameters and sampler are already the driver's
                    // costs two record reads instead of three nested syncs and their SharedPtr
                    // traffic. Same borrow rule as there - the list's key proves the twin alive.
                    const auto* record = PipeTextureRecordForHandle(entry.Res);
                    if (record != nullptr && entry.backend != nullptr &&
                        entry.backend->IsDrawSyncCleanByRecord(entry.Res, *record))
                        continue;
                    TextureImpl::SyncTextureToBackendByHandle(entry.Res, /*imageBindableStorageRequired=*/false);
                }
                return;
            }
            list.clear();
            const auto add = [&](const MG_Pipe::MGPSurface& surface) {
                if (surface.Kind != MG_Pipe::kMGPipeSurfaceKindTexture) return;
                if (MG_Pipe::MGPipeHandleIsNull(surface.Res)) return;
                auto& twin = TextureImpl::SyncTextureToBackendByHandle(surface.Res,
                                                                       /*imageBindableStorageRequired=*/false);
                list.push_back({surface.Res, twin.get()});
            };
            for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) add(record.Color[i]);
            add(record.Depth);
            add(record.Stencil);
        }

        DrawTextureSyncKeys CaptureDrawTextureSyncKeys() {
            DrawTextureSyncKeys keys;
            keys.contextId = MG_Pipe::gPipeInputs->GetTextureContextId();
            // Units past the frontend's high-water mark have provably-empty slots.
            //
            // P4a e2 DELIBERATELY DOES NOT take this off the sampler-view window's Count, and
            // the reason is asymmetric risk. D-G2 makes Count the client's spelling of exactly
            // this number, so the substitution looks free - but the two failure directions are
            // not alike: a window that is larger than the frontend's mark costs a walk over
            // provably-empty units, while one that is SMALLER silently drops the sync of a
            // texture a draw is about to sample, which no gate on this tree can see because
            // this tree emits no windows at all. The design assigns the high-water mark to the
            // CLIENT as the count argument and says nothing about re-deriving it server-side.
            // It is one accessor read per draw; the integrator can move it in one line once a
            // tree exists where the two can be compared.
            keys.maxTouchedUnit = MG_Pipe::gPipeInputs->GetMaxTouchedTextureUnit();
            keys.samplingGeneration = MG_Pipe::gPipeInputs->GetSamplingResolutionGeneration();
            keys.unitBindingsEpoch = CurrentUnitBindingsEpoch(keys.maxTouchedUnit);
            if (MG_Util::PipeStats::Enabled()) {
                // The three reads above; CurrentUnitBindingsEpoch counts its own two when it
                // takes the walk arm and none when the records answered.
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 3);
            }
            return keys;
        }

        // P4a e2 (D-E3). The READ framebuffer's texture attachments, and why this exists.
        //
        // A texture that is an attachment of the READ framebuffer ONLY reaches
        // SyncAttachmentObject, which calls SyncMipmapsToBackend AND NOTHING ELSE - so its
        // swizzle, its GL_DEPTH_STENCIL_TEXTURE_MODE, its LOD clamp and its border colour
        // never reach the driver. The list that pushes texture PARAMETERS for an attachment is
        // the one below, and it reads the DRAW binding slot alone. That is the gap
        // ARCHITECTURE.md's D10 names, and the roadmap makes closing it a mandatory,
        // red-before deliverable of this phase: set_texture_params is addressed by RESOURCE
        // and is independent of any binding, so a texture's parameters exist the moment they
        // move and an attachment - draw or read - gets them.
        //
        // DELIBERATELY NOT GATED ON THE FRAMEBUFFER SUBSYSTEM BIT. It is a deliverable of the
        // phase rather than an arm of it, so both arms of the object A/B carry it and the A/B
        // keeps measuring the arm rather than the arm plus a behaviour change. It is inside
        // MOBILEGL_PIPE_PUSH, so the pull build is untouched.
        //
        // Its own list and its own key rather than an extension of the draw list's, because
        // the draw list is keyed on the draw binding and its pull-build copy must stay
        // textually what it was.
        static Vector<UnitTextureSyncEntry> g_readFboTextureSyncList;
        static MG_State::GLState::FramebufferObject* g_readFboTextureSyncListFbo = nullptr;
        static Uint16 g_readFboTextureSyncListSlotVersion = 0;
        static Uint16 g_readFboTextureSyncListObjectVersion = 0;
        static Uint64 g_readFboTextureSyncListContextId = 0;
        static Uint g_readFboTextureSyncListContextGeneration = 0;
        static Uint64 g_readFboTextureSyncListContentHash = 0;
        static Bool g_readFboTextureSyncListRecordKeyed = false;

        // P5e (fb, CONTRACT-P5E.md §5.4): THE RECORD ARM OF BOTH ATTACHMENT LISTS, and it is
        // the package's core - this is the one unconditional per-draw live read the framebuffer
        // family had, taken on every draw whether the memo hit or missed.
        //
        // Three frontend reads go in one move: the binding slot (twice), the FramebufferObject
        // the entries borrowed their attachment slots from, and GetTextureContextId. The key is
        // {Fbo, ContentHash, ContextSerial, g_backendContextGeneration} - the record's own hash
        // in place of the slot/object version pair (it covers every surface, and Fbo is inside
        // it, so a recycled framebuffer cannot be suppressed against its predecessor), the
        // APPLIER's ContextSerial in place of the frontend context id, and the backend context
        // generation unchanged because it answers a question about driver names that no
        // client-side value can answer.
        //
        // The two lists are still two, and the dedupe is still "one object on both bindings" -
        // asked of the two BOUND HANDLES rather than of two pointers, which is the same
        // question with a spelling a recycled slot cannot fool.
        static void SyncFramebufferAttachmentTexturesByRecord() {
            const auto& st = MG_Pipe::MGPipeApplier();
            const MG_Pipe::MGPipeHandle drawHandle =
                st.BoundFramebuffer[SizeT(MG_Pipe::MGPipeFramebufferTarget::Draw)];
            const MG_Pipe::MGPipeHandle readHandle =
                st.BoundFramebuffer[SizeT(MG_Pipe::MGPipeFramebufferTarget::Read)];
            const MG_Pipe::MGPFramebufferState* const drawRecord =
                BoundFramebufferRecord(FramebufferTarget::Draw);
            const MG_Pipe::MGPFramebufferState* const readRecord =
                BoundFramebufferRecord(FramebufferTarget::Read);
            const Uint64 contextSerial = st.ContextSerial;

            if (drawRecord == nullptr) {
                // Not a decline this function can refuse on: SyncCurrentFBO runs before every
                // caller of this one and is where the missing record is named and refused
                // (RefuseFramebufferBindingSlotRead). Here it only means "nothing to walk".
                g_fboAttachmentSyncListValid = false;
                g_fboAttachmentSyncList.clear();
            } else {
                const Bool listValid = g_fboAttachmentSyncListValid &&
                                       g_fboAttachmentSyncListFbo == drawRecord->Fbo &&
                                       g_fboAttachmentSyncListContentHash == drawRecord->ContentHash &&
                                       g_fboAttachmentSyncListContextSerial == contextSerial &&
                                       g_fboAttachmentSyncListContextGeneration == g_backendContextGeneration;
                SyncRecordAttachmentTextures(*drawRecord, g_fboAttachmentSyncList, listValid);
                g_fboAttachmentSyncListFbo = drawRecord->Fbo;
                g_fboAttachmentSyncListContentHash = drawRecord->ContentHash;
                g_fboAttachmentSyncListContextSerial = contextSerial;
                g_fboAttachmentSyncListContextGeneration = g_backendContextGeneration;
                g_fboAttachmentSyncListValid = true;
            }

            if (readRecord == nullptr || readHandle == drawHandle) {
                g_readFboAttachmentSyncListValid = false;
                g_readFboAttachmentSyncList.clear();
                return;
            }
            const Bool readListValid = g_readFboAttachmentSyncListValid &&
                                       g_readFboAttachmentSyncListFbo == readRecord->Fbo &&
                                       g_readFboAttachmentSyncListContentHash == readRecord->ContentHash &&
                                       g_readFboAttachmentSyncListContextSerial == contextSerial &&
                                       g_readFboAttachmentSyncListContextGeneration == g_backendContextGeneration;
            SyncRecordAttachmentTextures(*readRecord, g_readFboAttachmentSyncList, readListValid);
            g_readFboAttachmentSyncListFbo = readRecord->Fbo;
            g_readFboAttachmentSyncListContentHash = readRecord->ContentHash;
            g_readFboAttachmentSyncListContextSerial = contextSerial;
            g_readFboAttachmentSyncListContextGeneration = g_backendContextGeneration;
            g_readFboAttachmentSyncListValid = true;
        }

        void SyncNeccessaryTextures(const DrawTextureSyncKeys& keys) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            // All textures we need are:
            //   1. textures bound to texture units (TODO: only sync ones that are used in current program)
            //   2. textures used in current FBO
            //   3. textures bound to image units (TODO)

            const Int maxTouchedUnit = keys.maxTouchedUnit;
            const Uint64 samplingGeneration = keys.samplingGeneration;
            const Uint64 unitBindingsEpoch = keys.unitBindingsEpoch;
            // P5e (tx2), CONTRACT-P5E §5.2. THE UNIT HALF, BY HANDLE.
            //
            // set_sampler_views is the RESOLVED answer to the question this walk asks - which
            // texture does each touched unit sample - and the client has already applied every
            // drop the walk below performs by hand: an image-less default texture and a texture
            // that samples as incomplete are both null in the record (SamplerEmit.h's two drops),
            // and the alias contest between two real textures on one native target was arbitrated
            // by the program's own sampler types. So the window IS the work list.
            //
            // NAMED BEHAVIOUR DELTA (P5e-5, contract §5.2): this syncs the PROGRAM-RESOLVED
            // texture per unit, where the frontend walk syncs every slot of every touched unit.
            // Coverage is unchanged - views UNION image units UNION the FBO attachment lists
            // UNION the waited texture ops still cover every texture anything reads - so it
            // narrows WORK and not what reaches the driver.
            const auto& st = MG_Pipe::MGPipeApplier();
            const Bool listValid = g_unitTextureSyncListByHandleValid &&
                                   g_unitTextureSyncListByHandleContextSerial == st.ContextSerial &&
                                   g_unitTextureSyncListByHandleViewsSerial == st.SamplerViewsSerial &&
                                   g_unitTextureSyncListByHandleStart == st.SamplerViewStart &&
                                   g_unitTextureSyncListByHandleCount == st.SamplerViewCount &&
                                   g_unitTextureSyncListByHandleContextGeneration == g_backendContextGeneration;
            if (listValid) {
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::EsprytTextureSyncList, /*hit=*/true);
                }
                for (const auto& entry : g_unitTextureSyncListByHandle) {
                    const auto* record = PipeTextureRecordForHandle(entry.Res);
                    if (record == nullptr) continue;
                    // The aggregate gate, from the twin's own serials against the record's -
                    // no frontend version, no context id, no sampling generation.
                    if (entry.backend->IsDrawSyncCleanByRecord(entry.Res, *record)) continue;
                    SyncTextureToBackendByHandle(entry.Res);
                }
            } else {
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::EsprytTextureSyncList, /*hit=*/false);
                }
                g_unitTextureSyncListByHandleValid = false;
                g_unitTextureSyncListByHandle.clear();
                const Uint32 windowEnd = st.SamplerViewStart + st.SamplerViewCount;
                for (Uint32 index = st.SamplerViewStart;
                     index < windowEnd && index < st.BoundSamplerViews.size(); ++index) {
                    const auto& view = st.BoundSamplerViews[index];
                    if (MG_Pipe::MGPipeHandleIsNull(view.Texture)) continue;
                    auto& twin = SyncTextureToBackendByHandle(view.Texture);
                    if (!twin) continue;
                    g_unitTextureSyncListByHandle.push_back({view.Texture, twin.get()});
                }
                g_unitTextureSyncListByHandleContextSerial = st.ContextSerial;
                g_unitTextureSyncListByHandleViewsSerial = st.SamplerViewsSerial;
                g_unitTextureSyncListByHandleStart = st.SamplerViewStart;
                g_unitTextureSyncListByHandleCount = st.SamplerViewCount;
                g_unitTextureSyncListByHandleContextGeneration = g_backendContextGeneration;
                g_unitTextureSyncListByHandleValid = true;
            }
            // The pre-handle list must not be replayed after this arm ran: its entries borrow
            // frontend slots this arm never refreshed.
            g_unitTextureSyncListValid = false;

            // Texture attachments of the draw FBO, memoised like the unit list above: WHICH
            // textures hang off the FBO only changes with a rebind (slot version), an
            // attachment/draw-buffer edit (object version - the documented invariant
            // SyncCurrentFBO's memo already leans on), a different FBO landing on a recycled
            // heap address (pointer + slot version together, the StampSyncedFBO trio), another
            // frontend context (context id) or a rebuilt ES context (backend generation). WHAT
            // each texture then needs is still decided per draw by the version gates inside the
            // three sync calls. Entry lifetime mirrors the unit list: the attachment holds the
            // texture's SharedPtr at a stable address while the FBO is unchanged, and the
            // registry keeps a backend object alive until its frontend texture expires, which an
            // attached texture cannot. A renderbuffer-only FBO - the common Minecraft frame -
            // reduces to the key compare and an empty loop.
            // P5e (fb): under a transport with the framebuffer bit set, BOTH attachment lists
            // are answered from the two records and neither binding slot is touched. The
            // pre-handle pair below stays compiled and stays the monolith path (ruling 1).
            SyncFramebufferAttachmentTexturesByRecord();
            return;
        }

        void SyncNeccessaryTextures() { SyncNeccessaryTextures(CaptureDrawTextureSyncKeys()); }

        // Whether glBindImageTexture's `layered` means anything for this target - asked of the
        // target the DRIVER will see, not the one the application named. A GL_TEXTURE_1D_ARRAY
        // is stored as an ES 2D array (MapToBackendTextureTarget), and so is layerable; asking
        // the state target instead answered "no" for it and pinned every 1D-array image binding
        // to layer 0, whatever the application passed.
        //
        // `layer` travels with the answer, because GL 4.6 core 8.26 (and ES 3.2 8.22, word for
        // word) makes them one rule: "If the texture identified by texture does not have
        // multiple layers or faces, the entire texture level is bound, regardless of the values
        // of layered and layer." REGARDLESS means ignored - not clamped, and not an error - so
        // the driver must not be handed a layer index the texture has no room for. Adreno takes
        // such a request literally and leaves the image unit reading zero, which is what failed
        // KHR-GL42.bind_image_texture.single_layer's layer:1 rows on GL_TEXTURE_2D and on the
        // GL_TEXTURE_1D that is stored as one. Normalizing here and not in the frontend shadow
        // is deliberate: GL_IMAGE_BINDING_LAYER must keep echoing what the application passed.
        static Bool SupportsLayeredImageBinding(TextureTarget target) {
            const TextureTarget backendTarget = TextureImpl::MapToBackendTextureTarget(target);
            return backendTarget == TextureTarget::Texture3D || backendTarget == TextureTarget::TextureCubeMap ||
                   backendTarget == TextureTarget::Texture2DArray ||
                   backendTarget == TextureTarget::TextureCubeMapArray ||
                   backendTarget == TextureTarget::Texture2DMultisampleArray;
        }

        // Which image units currently hold a WRITABLE buffer texture, and how many. Kept here
        // rather than recomputed per draw because the frontend tracks 192 image units and
        // almost every program uses none of them: the draw path pays one integer test.
        //
        // Maintained by SyncImageTextureBinding, which is the single funnel for an image-unit
        // change on this backend - glBindImageTextures is a frontend loop over
        // glBindImageTexture, and the whole-sweep SyncImageTextureBindings goes through it too.
        // Anything that clears a binding WITHOUT coming through here (a deleted texture, a
        // recreated context) can only leave a bit set for a unit that no longer has one; the
        // sweep re-reads the binding, finds nothing to mark, and CLEARS the bit on its way past.
        // So the error is self-healing, and in the direction that costs one wasted look rather
        // than one missed write.
        static Array<Bool, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_writableImageBufferUnits{};
        static Uint g_writableImageBufferUnitCount = 0;

        static void TrackWritableImageBufferUnit(Uint unit, Bool writableBufferTexture) {
            Bool& tracked = g_writableImageBufferUnits[unit];
            if (tracked == writableBufferTexture) return;
            tracked = writableBufferTexture;
            // Written as two guarded steps rather than one signed add: the count is unsigned,
            // and a decrement that ever ran one time too many would not saturate at zero, it
            // would wrap to four billion and defeat the early-out for the rest of the process.
            if (writableBufferTexture) {
                ++g_writableImageBufferUnitCount;
            } else if (g_writableImageBufferUnitCount > 0) {
                --g_writableImageBufferUnitCount;
            }
        }

        // Highest image unit that has ever been given a texture, plus one. Maintained by the
        // single funnel below, so it is a sound "no draw in this context can be reading an image"
        // test: nothing reaches an image unit without going through SyncImageTextureBinding.
        // Almost every program (every Minecraft draw) leaves it at zero, which is what keeps the
        // draw-path staleness check below at one integer test.
        Uint g_imageUnitHighWaterMark = 0;
        // ---- P5e (fb, CONTRACT-P5E.md §5.4 / ruling 16 / ID-94): the image unit's bind ------
        //
        // P4a e3's ResolveShaderImageRecord IS GONE, identity test, I5 seam log and all. It
        // existed because there were TWO answers for one unit - the record and the frontend
        // binding - and it had to decide which of them the driver was driven from; its identity
        // test (`view.Res != HandleOf(boundTexture)`) was itself a client-allocator probe on the
        // apply thread. Under a transport there is no second answer left: the record IS the
        // unit, so the arm below reads it and nothing corroborates it.
        //
        // AND ACCESS IS DECODED. The comment this replaced said MGPImageView::Access carried
        // "an ENCODING whose definition belongs to the client emitter and does not exist at the
        // contract commit". That was STALE at the time and is what ruling 16 / ID-94 corrects:
        // MGPipeEncodeImageAccess has folded the three GL names into 0/1/2 since P4a, and c0e
        // moved the numbers into MG_Pipe/MGPipeValueTypes.h so one table serves both roles.
        // Reading the frontend's GLenum "because it is the same value by construction" was the
        // last field of this record the server declined to believe.

        // The bind itself, factored out of both arms, because the format rules below are the
        // part that is easy to get subtly different in two copies - and a divergence there is
        // an out-of-bounds image read on nineteen of twenty-six formats, not a wrong pixel.
        // Everything it takes is a VALUE: the arm above it decides where each one came from.
        //
        // COMPILED IN THE PULL BUILD TOO, and that is a deliberate exception to the D-P
        // discipline the renderbuffer macros keep (each arm's expression at its original site).
        // A macro cannot carry a hundred lines of format reasoning, and the alternative was the
        // same hundred lines twice; the function is `static` with ONE caller in the pull build,
        // so it has no symbol of its own there and the code it generates is the code that was
        // written inline before. Named here rather than discovered by a G1 diff.
        // NOLINTNEXTLINE(readability-function-cognitive-complexity)
        static void IssueImageTextureBind(Uint unit, BackendTextureObject& backendTexture,
                                          TextureTarget textureTarget, TextureInternalFormat textureFormat,
                                          GLenum appFormat, GLint level, Bool layeredRequested,
                                          GLint layerRequested, GLenum access) {
            const Bool layerable = SupportsLayeredImageBinding(textureTarget);
            const GLboolean layered = layerable ? static_cast<GLboolean>(layeredRequested) : GL_FALSE;
            const GLint layer = layerable ? layerRequested : 0;
            // The bind half of the image-format widening. The storage sync has just allocated
            // this texture's storage in the core carrier of its format (the call above is the
            // one that marks it image-bindable), and glBindImageTexture's `format` has to name
            // the storage the texture really has: a GL_RG32F bind is GL_INVALID_VALUE on Adreno
            // for nineteen of the twenty-six non-core formats and on both Malis for twenty-five,
            // and every driver that DOES accept a narrow texture through a wide image accepts it
            // silently, reading and writing out of bounds. The frontend's own ImageTextureBinding
            // keeps the application's format untouched, so GL_IMAGE_BINDING_FORMAT still answers
            // what was passed in.
            //
            // Widened from the format the APPLICATION named rather than from the texture's own,
            // because GL lets the two differ inside one format class and the shader was widened
            // from the class the application named too (an r32ui view of an r32f image is a legal
            // reinterpretation). The two carriers always have the same texel size - every format
            // in a class widens to the four-channel form of that same class - so the storage
            // still describes what the bind claims. Gated on the TEXTURE having been widened, so
            // a bind format that names a class the storage does not have is left alone: GL
            // already calls that undefined, and inventing a carrier for it would only make the
            // out-of-class read wider.
            //
            // A BUFFER texture is excluded from the WIDENING on both sides: it has no storage of
            // its own to widen (its texels are the application's buffer object), so
            // WidenImageFormatsPass declines to widen every buffer image and the bind must decline
            // with it, or the driver would be handed a carrier the shader never addressed. See the
            // Dim::Buffer guard there for the 32-byte GL_RG32F measurement that pinned it.
            //
            // What a buffer image takes instead is the SPLIT, which is the same three-layer move
            // through a different door: a private glTexBuffer view names the single-channel base
            // format, the bind below names it too, and the shader subscripts it two components per
            // original texel. Same gate on all three, so they cannot disagree.
            //
            // The split view is a SEPARATE texture name over the same buffer, and the bind has to
            // name it rather than the application's own: the application's texture keeps the
            // format it asked for so that a samplerBuffer reading the same buffer texture - which
            // is NOT subscript-rewritten - still sees whole texels. See
            // BackendTextureObject::m_bufferImageSplitViewId.
            GLenum bindFormat = appFormat;
            GLuint bindTextureId = backendTexture.GetBackendTextureId();
            if (textureTarget == TextureTarget::TextureBuffer) {
                if (TextureImpl::GetImageBindableBufferSplitFormat(textureFormat) != GL_UNKNOWN_MGL) {
                    if (const GLenum boundFormatSplit = TextureImpl::GetImageBindableBufferSplitFormat(
                            MG_Util::ConvertGLEnumToTextureInternalFormat(appFormat));
                        boundFormatSplit != GL_UNKNOWN_MGL) {
                        bindFormat = boundFormatSplit;
                        if (const Uint splitViewId = backendTexture.GetBufferImageSplitViewId();
                            splitViewId != 0) {
                            bindTextureId = splitViewId;
                        }
                    }
                }
            } else if (TextureImpl::GetImageBindableStorageWidening(textureFormat)) {
                const auto boundFormatWidening = TextureImpl::GetImageBindableStorageWidening(
                    MG_Util::ConvertGLEnumToTextureInternalFormat(appFormat));
                if (boundFormatWidening) {
                    bindFormat = boundFormatWidening.InternalFormat;
                }
            }
            g_GLESFuncs.glBindImageTexture(unit, bindTextureId, level, layered, layer, access, bindFormat);
        }

        // MGPImageView::Access -> the GL token glBindImageTexture takes. The three numbers are
        // MGPipeValueTypes.h's, so this is a spelling change and not a second table; anything
        // else on the wire is Fatal{ProtocolCorruption, "ImageView.Access"} AT THE READER,
        // which is what the value header's comment asks for (it may not log, so it answers the
        // question and the caller owns the refusal).
        static GLenum GLAccessForImageView(const MG_Pipe::MGPImageView& view) {
            if (!MGPipeImageAccessIsValid(view.Access)) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ImageView.Access\"} An image record "
                        "carries Access=%u at unit %u, and only 0/1/2 are defined.",
                        static_cast<unsigned>(view.Access), static_cast<unsigned>(view.Unit));
                std::abort();
            }
            switch (MGPipeDecodeImageAccess(view.Access)) {
            case MGPipeImageAccess::WriteOnly: return GL_WRITE_ONLY;
            case MGPipeImageAccess::ReadWrite: return GL_READ_WRITE;
            case MGPipeImageAccess::ReadOnly:
            case MGPipeImageAccess::Count: break;
            }
            return GL_READ_ONLY;
        }

        // A writable buffer image, said by the record: the access byte and the resource
        // descriptor's storage kind, neither of which is a frontend read. Same question
        // IsWritableImageBufferTexture asks of the frontend binding.
        static Bool IsWritableImageBufferView(const MG_Pipe::MGPImageView& view) {
            if (MG_Pipe::MGPipeHandleIsNull(view.Res)) return false;
            if (!MGPipeImageAccessIsValid(view.Access)) return false;
            if (!MGPipeImageAccessWrites(MGPipeDecodeImageAccess(view.Access))) return false;
            const auto* record = PipeTextureRecordForHandle(view.Res);
            return record != nullptr &&
                   static_cast<TextureStorageType>(record->Desc.StorageKind) == TextureStorageType::Buffer;
        }

        // GL 4.6 core 8.26: an access through an image unit whose texture does not HAVE the
        // (level, layer) the unit names is INVALID - a load returns zero, a store does nothing,
        // and neither is an error. The ES storage behind such a unit may well have that level
        // (a mutable texture's storage is allocated for its whole chain the first time it is
        // image-bound), and the driver would read whatever that undefined level holds. The
        // client sends the bindings GL calls incomplete with level kMGPipeImageLevelNone
        // (KHR-GL46.shader_image_load_store.incomplete_textures binds level 2 of a texture that
        // defines only level 0); a level or layer past the storage is caught here. Decided from
        // the applier's records alone, by the same walk the Vulkan backend makes
        // (VkTextureManager::ResolveWireTextureStorage): through texture views to the root,
        // against each window's levels and layers.
        static Bool ImageViewNamesNoTexel(const MG_Pipe::MGPImageView& image) {
            using T = MG_Pipe::MGPipeResourceTarget;
            const auto& state = MG_Pipe::MGPipeApplier();
            MG_Pipe::MGPipeHandle handle = image.Res;
            if (MG_Pipe::MGPipeHandleIsNull(handle) || handle.Slot >= state.TextureResources.size()) return false;
            const auto& first = state.TextureResources[handle.Slot];
            if (!first.Live || first.Gen != handle.Gen) return false;
            // GL ignores Layer for a target with no layers, and for a layered bind.
            const auto firstTarget = static_cast<T>(first.Desc.Target);
            const Bool selectsLayer = !image.Layered &&
                (firstTarget == T::Tex1DArray || firstTarget == T::Tex2DArray || firstTarget == T::Tex2DMSArray ||
                 firstTarget == T::Tex3D || firstTarget == T::TexCube || firstTarget == T::TexCubeArray);
            Uint32 level = image.Level;
            Uint32 layer = selectsLayer ? image.Layer : 0;
            for (SizeT hop = 0; hop <= state.TextureResources.size(); ++hop) {
                if (MG_Pipe::MGPipeHandleIsNull(handle) || handle.Slot >= state.TextureResources.size()) return false;
                const auto& record = state.TextureResources[handle.Slot];
                if (!record.Live || record.Gen != handle.Gen) return false;
                if (MG_Pipe::MGPipeHandleIsNull(record.Desc.ViewOf)) {
                    if (static_cast<T>(record.Desc.Target) == T::TexBuffer) return false;
                    if (level >= record.Desc.Levels) return true;
                    const Uint32 layers = static_cast<T>(record.Desc.Target) == T::Tex3D
                                              ? std::max(record.Desc.Depth >> level, 1u)
                                              : std::max<Uint32>(record.Desc.ArrayLayers, 1u);
                    return layer >= layers;
                }
                const auto viewHandle = record.ViewCso;
                if (MG_Pipe::MGPipeHandleIsNull(viewHandle) || viewHandle.Slot >= state.SamplerViewCsos.size())
                    return false;
                const auto& viewRecord = state.SamplerViewCsos[viewHandle.Slot];
                const auto& view = viewRecord.View;
                if (!viewRecord.Live || viewRecord.Gen != viewHandle.Gen || view.Texture != handle) return false;
                if (level >= view.NumLevels || layer >= view.NumLayers) return true;
                level += view.MinLevel;
                layer += view.MinLayer;
                handle = record.Desc.ViewOf;
            }
            return false;
        }

        // What such a unit is bound to instead, on a driver whose EMPTY unit is not enough
        // (GLESCapabilities::UseInvalidImagePlaceholder): ES 3.2 8.22 leaves an invalid load's
        // alpha undefined, Adreno answers 1 where GL says 0, and ANGLE faults outright.
        // Elsewhere the unit stays empty, which is exact. A texel outside
        // a level is invalid too, and that Adreno answers (0, 0, 0, 0) with the store dropped - so
        // the unit gets a 1x1 texture of the shape and format the bind names, cleared to zero each
        // time it is bound. Only texel (0, 0[, 0]) of it exists; it is private to the unit, so a
        // store through one invalid unit cannot reach a load through another. Every context of the
        // process shares one group (CreateContextTuple), so one set serves them all.
        struct InvalidImagePlaceholder {
            GLuint Id = 0;
            GLenum Target = 0;
            GLenum InternalFormat = 0;
        };
        static Array<InvalidImagePlaceholder, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_invalidImagePlaceholders{};

        // Zero when the shape has no 1x1 ES form (multisample, buffer) or the driver refused it;
        // the caller then leaves the unit empty, which is right for every channel but alpha.
        static GLuint EnsureInvalidImagePlaceholder(Uint unit, TextureTarget textureTarget, GLenum internalFormat) {
            GLenum target = 0;
            GLsizei depth = 1;
            switch (TextureImpl::MapToBackendTextureTarget(textureTarget)) {
            case TextureTarget::Texture2D: target = GL_TEXTURE_2D; break;
            case TextureTarget::Texture2DArray: target = GL_TEXTURE_2D_ARRAY; break;
            case TextureTarget::Texture3D: target = GL_TEXTURE_3D; break;
            case TextureTarget::TextureCubeMap: target = GL_TEXTURE_CUBE_MAP; break;
            case TextureTarget::TextureCubeMapArray: target = GL_TEXTURE_CUBE_MAP_ARRAY; depth = 6; break;
            default: return 0;
            }
            GLenum format = 0, type = 0;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(internalFormat, Flags<PixelFormatNormalizeOptionBit>{},
                                                                  nullptr, &format, &type);
            if (format == 0 || type == 0) return 0;
            InvalidImagePlaceholder& slot = g_invalidImagePlaceholders[unit];
            if (slot.Id != 0 && (slot.Target != target || slot.InternalFormat != internalFormat)) {
                g_GLESFuncs.glDeleteTextures(1, &slot.Id);
                slot = {};
            }
            GLint previous = 0;
            const GLenum bindingQuery = target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D
                : target == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_BINDING_2D_ARRAY
                : target == GL_TEXTURE_3D ? GL_TEXTURE_BINDING_3D
                : target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_BINDING_CUBE_MAP
                : GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
            g_GLESFuncs.glGetIntegerv(bindingQuery, &previous);
            DebugImpl::ErrorLopper::Clear();
            if (slot.Id == 0) {
                g_GLESFuncs.glGenTextures(1, &slot.Id);
                g_GLESFuncs.glBindTexture(target, slot.Id);
                if (depth > 1 || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_3D) {
                    g_GLESFuncs.glTexStorage3D(target, 1, internalFormat, 1, 1, depth);
                } else {
                    g_GLESFuncs.glTexStorage2D(target, 1, internalFormat, 1, 1);
                }
                slot.Target = target;
                slot.InternalFormat = internalFormat;
            } else {
                g_GLESFuncs.glBindTexture(target, slot.Id);
            }
            // Six faces of the widest texel (rgba32) is the most any shape here holds.
            static constexpr Uint8 kZeros[6 * 16] = {};
            // The backend unpack state rests at the ES default (Managers.cpp
            // ScopedDefaultUnpackState), and a single row ignores the alignment.
            BufferImpl::BindPixelUnpackBufferId(0);
            if (target == GL_TEXTURE_CUBE_MAP) {
                for (GLenum face = 0; face < 6; ++face)
                    g_GLESFuncs.glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, 1, 1, format, type,
                                                kZeros);
            } else if (target == GL_TEXTURE_2D) {
                g_GLESFuncs.glTexSubImage2D(target, 0, 0, 0, 1, 1, format, type, kZeros);
            } else {
                g_GLESFuncs.glTexSubImage3D(target, 0, 0, 0, 0, 1, 1, depth, format, type, kZeros);
            }
            g_GLESFuncs.glBindTexture(target, static_cast<GLuint>(previous));
            Bool failed = false;
            DebugImpl::ErrorLopper::Loop([&failed, internalFormat](GLenum err) {
                failed = true;
                MGLOG_D("DirectGLES: invalid-image placeholder (%s) refused: %s",
                        MG_Util::ConvertGLEnumToString(internalFormat).c_str(),
                        MG_Util::ConvertGLEnumToString(err).c_str());
            });
            if (failed) {
                g_GLESFuncs.glDeleteTextures(1, &slot.Id);
                slot = {};
            }
            return slot.Id;
        }

        // The record form. `Res` is the texture, the four well-defined fields are the record's,
        // Access is decoded, and the texture's own TARGET and STORAGE FORMAT - the two
        // properties the bind rules need and MGPImageView has no room for (24 bytes, no pad) -
        // come off the RESOURCE DESCRIPTOR, which is where the server already keeps them.
        void SyncImageTextureBinding(const MG_Pipe::MGPImageView& view) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const Uint unit = static_cast<Uint>(view.Unit);
            if (unit >= g_writableImageBufferUnits.size()) return;
            TrackWritableImageBufferUnit(unit, IsWritableImageBufferView(view));
            if (MG_Pipe::MGPipeHandleIsNull(view.Res)) {
                g_GLESFuncs.glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                return;
            }
            if (unit + 1 > g_imageUnitHighWaterMark) {
                g_imageUnitHighWaterMark = unit + 1;
            }
            const auto* resource = PipeTextureRecordForHandle(view.Res);
            if (resource == nullptr) {
                // A record naming a texture the applier has no descriptor for is a seam defect,
                // not a binding: there is nothing to allocate image-bindable storage from and
                // nothing to widen against. Unbind rather than leave the unit on whatever the
                // previous draw put there.
                MGLOG_E_ONCE("MGPipe: image unit %u names texture {%u, %u}, which has no applier "
                             "resource record; unbinding the unit",
                             static_cast<unsigned>(unit), view.Res.Slot, view.Res.Gen);
                g_GLESFuncs.glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                return;
            }
            if (ImageViewNamesNoTexel(view)) {
                if (!g_GLESCapabilities.UseInvalidImagePlaceholder) {
                    g_GLESFuncs.glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                    return;
                }
                // The format the shader was widened from is the one the application bound, so
                // the placeholder takes that format's carrier, as the storage would have.
                GLenum format = static_cast<GLenum>(view.InternalFormat);
                if (const auto widening = TextureImpl::GetImageBindableStorageWidening(
                        MG_Util::ConvertGLEnumToTextureInternalFormat(format))) {
                    format = widening.InternalFormat;
                }
                const GLuint placeholder =
                    EnsureInvalidImagePlaceholder(unit, PipeTextureTargetForHandle(view.Res), format);
                const GLboolean layered = placeholder != 0 && view.Layered != 0 ? GL_TRUE : GL_FALSE;
                g_GLESFuncs.glBindImageTexture(unit, placeholder, 0, layered, 0, GLAccessForImageView(view),
                                               placeholder != 0 ? format : GL_RGBA8);
                return;
            }
            const auto textureFormat = static_cast<TextureInternalFormat>(resource->Desc.InternalFormat);
            const TextureTarget textureTarget = PipeTextureTargetForHandle(view.Res);
            const GLenum access = GLAccessForImageView(view);
            // Before the sync, for the frontend arm's reason: the note may adopt a twin, and
            // `backendTexture` below is a reference into the same registry.
            if (access != GL_READ_ONLY) {
                NoteDriverSideTextureWriteByHandle(view.Res);
            }
            auto& backendTexture = SyncTextureToBackendByHandle(view.Res, /*imageBindableStorageRequired=*/true);
            if (!backendTexture) return;
            IssueImageTextureBind(unit, *backendTexture, textureTarget, textureFormat,
                                  static_cast<GLenum>(view.InternalFormat), static_cast<GLint>(view.Level),
                                  view.Layered != 0, static_cast<GLint>(view.Layer), access);
        }

        // A buffer texture bound to a WRITABLE image unit is a buffer the shader is about to
        // write, and those writes land in the ES driver's buffer object - behind the frontend's
        // CPU shadow, which is what MapBuffer and GetBufferSubData read. Same flag, and for the
        // same reason, as MarkShaderStorageBuffersGpuWritten does for a storage block; the
        // difference is only which binding the shader reaches the buffer through. A GL_READ_ONLY
        // binding is left alone: marking it would make the next map wait on - and then re-read -
        // a dispatch that could not have changed a byte of it.
        //
        // Called from the draw and dispatch preparations rather than from the eager sync
        // glBindImageTexture performs: that one runs before any shader has touched the buffer,
        // and flagging there would pull the driver's copy over a shadow the application may
        // still be writing into.
        //
        // P5e (fb, CONTRACT-P5E.md §5.4): DELETED UNDER A TRANSPORT, and it is one edit paired
        // with sb's deletion of the backend's storage-block GPU-write marks. The two were one
        // mechanism: the backend told the frontend's buffer shadow "a shader wrote through
        // this" because only the backend knew which bindings a dispatch could reach. Under a
        // split the CLIENT owns that set and keeps it itself (MG_Impl/Pipe/GpuWritePending.h),
        // so the mark has an owner on the side that reads it - and this body cannot run there
        // anyway: every line of it is a frontend read (the image binding, the downcast to
        // TextureObjectBuffer, the buffer binding slot) plus a HandleOfBuffer probe behind
        // MarkBufferGpuWritten. The gate is at the top rather than at the two call sites so
        // there is one statement of it, and the unit-tracking bookkeeping below stays out of
        // reach of an apply thread entirely.
        void MarkWritableImageBufferTexturesGpuWritten() {
            // P8-C: the SERVER's twin of the buffer behind each writable buffer image, for
            // this side's CPU readers (SplitHostBytesForCpuRead). Every read is a record:
            // the unit set SyncImageTextureBinding(view) tracked, the applier's image view,
            // and the texture descriptor's backing-store handle.
            if (g_writableImageBufferUnitCount == 0) return;
            const auto& st = MG_Pipe::MGPipeApplier();
            for (Uint unit = 0; unit < g_writableImageBufferUnits.size() && unit < st.BoundShaderImages.size();
                 ++unit) {
                if (!g_writableImageBufferUnits[unit]) continue;
                const MG_Pipe::MGPImageView& view = st.BoundShaderImages[unit];
                if (!IsWritableImageBufferView(view)) continue;
                const auto* record = PipeTextureRecordForHandle(view.Res);
                if (record == nullptr) continue;
                if (auto* buffer = BufferImpl::FindBufferResourceForHandle(record->Desc.BufferForTexBuffer)) {
                    buffer->serverGpuWritten = true;
                }
            }
            return;
        }

        void SyncImageTextureBindings() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // The frontend tracks more image units than ES exposes; binding past the device
            // limit raises GL_INVALID_VALUE on every dispatch.
            const Uint unitCount = std::min<Uint>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS,
                                                  static_cast<Uint>(std::max(g_GLESCapabilities.MaxImageUnits, 0)));
            // P4a e3 (D-G2, D-J2), CORRECTED BY THE REVIEW'S MAJOR-1.
            //
            // THE RECORD'S WINDOW IS NOT THIS SWEEP'S MEMBERSHIP. D-J2 and PipeApply.h's MGPipeWorkingState
            // are a rule about RECORD RETENTION - entries outside the var-tail window are not
            // cleared in the applier - and say nothing about which DRIVER units a re-issue
            // sweep has to visit. The membership this loop needs is "every unit the driver
            // currently has an image on", and that is g_imageUnitHighWaterMark, which is
            // server-owned by construction: SyncImageTextureBinding is the only path to
            // glBindImageTexture and raises the mark on every unit it hands a texture, so the
            // mark is monotonic and covers every unit a re-mint could strand.
            //
            // The first version of this arm walked [Start, Start + Count) and returned, which
            // is the SMALLER-WINDOW failure direction DEV-3 refused for keys.maxTouchedUnit: if
            // set_shader_images is the program-RESOLVED set, a program declaring only unit 0
            // pushes a window of [0,1), and unit 5's image - established eagerly by an earlier
            // dispatch and never revisited - is skipped. A texture re-minted between the two
            // dispatches then leaves unit 5 on a deleted driver name: a write through a freed
            // allocation on Adreno, a rejected dispatch on Mali. The pre-handle sweep bound
            // EVERY unit precisely so that could not happen, and that guarantee is what this
            // sweep exists for. So the record's window is UNIONED with the mark, never
            // substituted for it, and the walk starts at 0 rather than at Start (a unit BELOW
            // Start is as undescribed as one above Start + Count).
            //
            // A8, which the review adds to this package's assumption list: NOTHING IN THE
            // CONTRACT PINS THE WINDOW'S MEMBERSHIP. PipeApply.h's MGPipeWorkingState says only that the
            // record is the last set as received, and that Start + Count above the bound is
            // Fatal{ProtocolCorruption}. Until package C documents in ImageEmit.h that the
            // window covers every unit the driver holds, this union is load-bearing; the
            // verification round logs (ShaderImageStart, ShaderImageCount) against the mark for
            // a Minecraft frame and says whether it can ever be relaxed.
            //
            // What the record still buys is real and is e3's actual deliverable: the four field
            // VALUES come off it inside SyncImageTextureBinding, and the walk stops at the
            // high-water mark instead of at the device's MaxImageUnits. The units in
            // [end, unitCount) the pre-handle arm additionally visits have never been given a
            // texture through the only funnel that can give one, so re-binding 0 on them is a
            // provable no-op.
            {
                const auto& st = MG_Pipe::MGPipeApplier();
                // P4a decline-site I6: M - a set that never arrived means the full pre-handle
                //   sweep below, which is the SAFE (wider) direction. SILENT HERE, CONFIRMED AT
                //   THE VERIFICATION ROUND, and it is silent here because I2 says it once per
                //   process from inside that sweep, where the unit that actually holds an image
                //   texture can be named. Two lines for one condition is how a grep's count
                //   stops meaning anything.
                if (st.ShaderImageCount != 0) {
                    // P4a decline-site I7: the window/mark UNION (MAJOR-1, fixed here); no flip
                    //   remains at the verification round, only the A8 measurement above.
                    //
                    // P5e (fb) KEEPS THE UNION and says so again, because ruling 7 / ID-86 make
                    // it contract text now (§5.4: "never narrowed to the window"). Under a
                    // transport the walk additionally READS the applier's array rather than the
                    // frontend binding at each unit - a unit inside the mark but outside the
                    // window keeps whatever MGPImageView the last set that covered it left
                    // there (D-J2: entries outside the window are not cleared), which is the
                    // record of the very binding that raised the mark.
                    const Uint32 end = std::min<Uint32>(
                        std::max<Uint32>(st.ShaderImageStart + st.ShaderImageCount,
                                         static_cast<Uint32>(g_imageUnitHighWaterMark)),
                        static_cast<Uint32>(unitCount));
                    for (Uint32 unit = 0; unit < end; ++unit) {
                        if (unit >= st.BoundShaderImages.size()) break;
                        // The union can include a slot no set_shader_images
                        // record has described yet. Its zero-initialized Unit
                        // is not a binding identity: treating it as one unbinds
                        // real unit 0 after binding the current program's images.
                        // Array position identifies both retained and empty slots.
                        auto view = st.BoundShaderImages[unit];
                        view.Unit = unit;
                        SyncImageTextureBinding(view);
                    }
                    return;
                }
            }
            // The wide pre-handle sweep reads the frontend binding of every unit, which an
            // apply thread may not do. Under a transport there is nothing for it to do anyway:
            // g_imageUnitHighWaterMark is raised only by the funnel above, so if no set has
            // ever arrived no unit has ever been given an image, and re-binding 0 on units that
            // never held one is the no-op the comment above already proved.
            return;
        }

        // What the draw path last swept the image units against. A draw never swept them at all:
        // an image unit was established once, eagerly, by glBindImageTexture and never revisited.
        // That is stale the moment the texture behind it is re-specified with a new size or
        // format, because ES 3.1 only allows IMMUTABLE storage on an image unit
        // (SyncTextureObjectToBackend's imageBindableStorageRequired), immutable storage cannot be
        // redefined, and so the re-spec MINTS A NEW ES TEXTURE NAME - leaving the unit pointing at
        // the deleted one and imageSize() reporting the old dimensions
        // (KHR-GL43.shader_image_size.advanced-changeSize).
        static Uint64 g_imageSweepContextId = 0;
        static Uint64 g_imageSweepSamplingGeneration = 0;
    } // namespace TextureImpl
} // namespace MobileGL::MG_Backend::DirectGLES
