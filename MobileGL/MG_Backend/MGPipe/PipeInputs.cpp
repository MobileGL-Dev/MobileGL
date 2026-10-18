// MobileGL - MobileGL/MG_Backend/MGPipe/PipeInputs.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The backend-side half of the PipeInputs block: the poison Fatal with its verb name and -
// in a verify build - the per-field equality, the entry comparator, the corruption injector,
// the name lookups its knob and the poison omission need, and the omission's server half.
// Spells no MG_State global: everything that reads the live context lives in
// MG_Impl/Pipe/PipeFill.cpp.
#include <MG_Backend/MGPipe/PipeInputs.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <Config.h>
#include <MG_Util/Metrics/PipeStats.h>

namespace MobileGL::MG_Pipe {
    const char* MGPipeVerbName(MGPipeVerb verb) {
        const auto index = static_cast<SizeT>(verb);
        return index < kMGPipeVerbCount ? kMGPipeVerbNames[index] : "<none>";
    }

    [[noreturn]] void MGPipeInputPoisonFatalForVerb(MGPipeInputField field, MGPipeVerb verb) {
        MGPipeInputPoisonFatal(field, MGPipeVerbName(verb));
    }

// The stamp is the poison's (m_filled, the generations): a build without MOBILEGL_PIPE_POISON
// compiles no read check that could ask any of this. Every transport build has the poison.
#if MOBILEGL_PIPE_POISON
    // ================================================================================
    // P5: the server's verb stamp, the residual-pull counter, and the four-way read verdict
    // ================================================================================
    namespace {
        Uint64 g_residualPulls = 0;

#if MOBILEGL_PIPE_VERIFY
        // ---- negative control B, server half (MGPipeServerSetPoisonOmission) ----
        //
        // The omission withholds the stamp of one (verb, field) pair, a negative control for the
        // poison check. Under a transport the backend reads the server's stamp, which re-stamps
        // every field the verb's class can answer, so the server must withhold the pair too or
        // the control cannot go red. The harness arms it while the apply thread is already
        // running, so the pair is ONE atomic word: the verb in the high half, the field in the
        // low one, and no verb at all (0xffff) until something is armed. Verify builds only, so
        // the shipped stamp stays branch-free.
        constexpr Uint32 kNoServerOmission = ~Uint32{0};
        std::atomic<Uint32> g_serverOmission{kNoServerOmission};

        constexpr Uint32 PackServerOmission(MGPipeVerb verb, MGPipeInputField field) {
            return (static_cast<Uint32>(verb) << 16) | static_cast<Uint32>(field);
        }

        Bool ServerOmitsStamp(MGPipeVerb verb, MGPipeInputField field) {
            return g_serverOmission.load(std::memory_order_acquire) == PackServerOmission(verb, field);
        }
#endif // MOBILEGL_PIPE_VERIFY

        // The strict arm of R-7.3. Same first line as the ordinary poison Fatal, so every
        // existing filter on Fatal{UnmigratedPipeInput still matches, plus the class and the
        // phase that retires it - a strict abort that did not say which phase owes the answer
        // would leave the reader exactly where the gate found them.
        // P5e (ra): `why` is the second half of the marker, because the lane's allowlist step
        // reads this line and the two reasons mean different things to it. "MOBILEGL_IPC_
        // STRICT_ERRORS=1" is the operator asking for a real, ordered value to be loud;
        // "UNBARRIERED" is the value not existing (§3.3). The `<field>@<verb>` prefix and the
        // Fatal{UnmigratedPipeInput tag are unchanged, so every filter written since P5c still
        // matches.
        [[noreturn]] void StrictBarrierPullFatal(MGPipeInputField field, MGPipeVerb verb, const char* why) {
            const SizeT index = static_cast<SizeT>(field);
            MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"} [BARRIER-PULLED, %s, "
                    "retires in %s]",
                    kMGPipeInputFieldNames[index], MGPipeVerbName(verb), why, kMGPipeFieldRetiringPhase[index]);
            std::abort();
        }

        // The admitted marker: the same `<field>@<verb>` and `[BARRIER-PULLED, ...]` tail as the
        // strict Fatal above, with `Admitted{` in place of `Fatal{`, so filters on
        // `Fatal{UnmigratedPipeInput` still mean only "red" and the lane can match each set with
        // one regex. Logged once per (field, verb) per process, the granularity of the allowlist;
        // otherwise an ~850-draw frame writes one line per draw. A plain array, not an atomic:
        // CountBarrierPull runs only on the apply thread after a server verb stamp, like
        // g_residualPulls, and a race would at worst log a duplicate line.
        void AdmittedBarrierPullOnce(MGPipeInputField field, MGPipeVerb verb, Bool escalated) {
            const SizeT fieldIndex = static_cast<SizeT>(field);
            const SizeT verbIndex = static_cast<SizeT>(verb);
            if (fieldIndex >= kMGPipeInputFieldCount || verbIndex >= kMGPipeVerbCount) return;
            static Uint64 seen[(kMGPipeVerbCount * kMGPipeInputFieldCount + 63) / 64] = {};
            const SizeT bit = verbIndex * kMGPipeInputFieldCount + fieldIndex;
            const Uint64 mask = Uint64{1} << (bit % 64);
            if ((seen[bit / 64] & mask) != 0) return;
            seen[bit / 64] |= mask;
            // P5e (gl), ID-128: WHICH DISJUNCT ADMITTED IT, in the slot the grammar already has
            // for the reason. `ADMITTED` means the generated table said so, and the lane can
            // check that against `--print-admitted`. `ADMITTED-ESCALATED` means the table did
            // NOT and the record was barriered by an escalation the table cannot see, which is a
            // RUNTIME fact about that record's payload - so the lane must not look for it in a
            // static list. Saying which is what keeps the lane's comparison exact instead of
            // widening the list with every pair that could ever escalate.
            MGLOG_W("MGPipe: Admitted{UnmigratedPipeInput, \"%s@%s\"} [BARRIER-PULLED, %s, "
                    "retires in %s]",
                    kMGPipeInputFieldNames[fieldIndex], MGPipeVerbName(verb),
                    escalated ? "ADMITTED-ESCALATED" : "ADMITTED", kMGPipeFieldRetiringPhase[fieldIndex]);
        }

        // Decides what a BARRIER-PULLED read does, for the field accessors and the sticky forwards:
        //   unbarriered -> Fatal, whatever the knob: the client is not parked behind the record,
        //                  so the row is torn or stale and there is nothing to count.
        //   barriered   -> counted (g_residualPulls); under MOBILEGL_IPC_STRICT_ERRORS it is Fatal
        //                  unless admitted, and an admitted one logs one MGLOG_W per (field, verb).
        // Admitted: the generated MGPipeBarrierPullAdmitted table says so, or the record was
        // barriered by an escalation, which only its payload shows.
        void CountBarrierPull(MGPipeInputField field, MGPipeVerb verb) {
            // P5f (f1), P5F-WIRE-COMPLETENESS.md §4: UNDER THE DUAL-BLOCK REHEARSAL EVERY
            // BARRIER-PULLED READ IS A NAMED FATAL, unconditionally - ahead of the barriered
            // question, ahead of the strict knob, ahead of the counter. The server block has no
            // residual fill to answer from, so the value the read would return is not "torn"
            // or "stale" but ABSENT: the field's stamp was withdrawn by the server's own verb
            // boundary and nothing on this side of the role split ever writes it. Letting the
            // read proceed would hand the backend default storage - or, for the O-class rows
            // with no null check, an empty SharedPtr to dereference - which is an UNNAMED
            // crash exactly where the rehearsal exists to produce a named one. The red is the
            // census (Harness/dualblock-expected-fatals.txt), so the marker keeps the strict
            // grammar with the knob's own name as the reason.
            // D10: rides MGPipeBlocksAreDistinct() - under spawn the two blocks are distinct
            // whatever the rehearsal knob says, and a barrier pull across a real process
            // boundary is exactly the thing this Fatal exists to name.
#if MOBILEGL_BUILD_DISAGGREGATED
            if (MGPipeBlocksAreDistinct()) {
                StrictBarrierPullFatal(field, verb, "MOBILEGL_IPC_ROLE_SPLIT_STATE=1");
            }
#endif
            if (!MGPipeApplierCurrentRecordIsBarriered()) {
                StrictBarrierPullFatal(field, verb, "UNBARRIERED, the client did not fill it");
            }
            ++g_residualPulls;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::ResidualPulls, 1);
            }
#if MOBILEGL_BUILD_DISAGGREGATED
            // The strict knob is the transport's (MOBILEGL_IPC_STRICT_ERRORS); without one an
            // admitted pull is counted and the unbarriered one above is still Fatal.
            if (MG_Config::Ipc.StrictErrors) {
                // THE THIRD DISJUNCT (P5e gl, ID-128), and it has to be asked at RUNTIME because
                // it is a fact about this record's payload rather than about its opcode. The
                // static table admits a pair when the verb's op waits (disjunct 1) or when the
                // field's retiring phase is not this phase (disjunct 2); an escalated record is
                // one the client parks behind for a reason only the payload knows - an open
                // transform-feedback span, or a draw carrying client vertex arrays. Both are
                // outside this phase by ruling (§5.7, ID-82), and the client-array read site
                // aborts by its own name if it is ever applied unbarriered, so the pull is legal
                // and P5e is not the phase that owes it.
                const Bool statically = MGPipeBarrierPullAdmitted(field, verb);
                const Bool escalated = MGPipeApplierCurrentRecordIsBarrieredByEscalation();
                if (!statically && !escalated) {
                    StrictBarrierPullFatal(field, verb, "MOBILEGL_IPC_STRICT_ERRORS=1");
                }
                AdmittedBarrierPullOnce(field, verb, !statically);
            }
#endif
        }

        // The verb's OWN may-read table (FillPoints.def, kMGPipeClassFieldMask). The stamp
        // respects it for the same reason the client's residual fill does: a field outside the
        // verb's class is one the fill never copied, so answering it out of gPipeInputs would
        // hand the server the PREVIOUS verb's value - the exact staleness the generation poison
        // exists to catch, re-introduced by the very mechanism meant to instrument it.
        Bool FieldIsInVerbClass(MGPipeInputField field, MGPipeVerb verb) {
            const SizeT verbIndex = static_cast<SizeT>(verb);
            if (verbIndex >= kMGPipeVerbCount) return false;
            const MGPipeVerbClass verbClass = kMGPipeVerbClass[verbIndex];
            return MGPipeFieldMaskHas(kMGPipeClassFieldMask[static_cast<SizeT>(verbClass)], field);
        }

        // ---- the same verdict, per verb CLASS, as a constant (P5d round 3, package C) ----
        //
        // THE STAMP's ANSWER IS A CONSTANT OF THE VERB CLASS AND OF NOTHING ELSE. Both halves of
        // `answerable` below read constexpr tables only - kMGPipeFieldOwnership, kMGPipeVerbClass
        // and kMGPipeClassFieldMask - so the 63-field loop was recomputing, at every verb on the
        // apply thread, a table the compiler can build once. The 2026-09-17 inproc profile put
        // MGPipeServerStampVerbBoundary at 1.1% self of the apply thread (Minecraft 26.3-rc-3,
        // ~852 draws/frame), and every cycle taken there is a cycle the lockstep client waits for.
        //
        // IT IS A MASK, NOT A BOOL ARRAY, so the stamp loop's body stays a shift and a store with
        // no branch: FilledGen[i] = serial & -bit, which is `serial` for an answerable field and
        // the WITHDRAWAL 0 for every other. The verdict itself is unchanged, and it is still
        // computed by the one expression argued at the stamp - it just runs at compile time.
        constexpr MGPipeFieldMask AnswerableMaskForClass(MGPipeVerbClass verbClass) {
            MGPipeFieldMask mask{};
            for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
                const auto field = static_cast<MGPipeInputField>(i);
                const MGPipeFieldOwnership ownership = kMGPipeFieldOwnership[i];
                const Bool answerable = (ownership == MGPipeFieldOwnership::kRecordSupplied ||
                                         ownership == MGPipeFieldOwnership::kApplierDerived) &&
                                        MGPipeFieldMaskHas(kMGPipeClassFieldMask[static_cast<SizeT>(verbClass)], field);
                if (answerable) mask.Words[i / 64] |= (Uint64{1} << (i % 64));
            }
            return mask;
        }

        struct AnswerableMaskTable {
            MGPipeFieldMask ByClass[kMGPipeVerbClassCount];
        };

        constexpr AnswerableMaskTable MakeAnswerableMaskTable() {
            AnswerableMaskTable table{};
            for (SizeT i = 0; i < kMGPipeVerbClassCount; ++i) {
                table.ByClass[i] = AnswerableMaskForClass(static_cast<MGPipeVerbClass>(i));
            }
            return table;
        }

        constexpr AnswerableMaskTable kAnswerableByClass = MakeAnswerableMaskTable();

        // A verb id outside the table answers NOTHING, which is exactly what FieldIsInVerbClass
        // said for it (`verbIndex >= kMGPipeVerbCount` -> false for every field) and therefore
        // what the stamp said: all 63 withdrawn, every read the poison Fatal.
        constexpr MGPipeFieldMask kNoFieldIsAnswerable{};

        const MGPipeFieldMask& AnswerableMaskForVerb(MGPipeVerb verb) {
            const SizeT verbIndex = static_cast<SizeT>(verb);
            if (verbIndex >= kMGPipeVerbCount) return kNoFieldIsAnswerable;
            return kAnswerableByClass.ByClass[static_cast<SizeT>(kMGPipeVerbClass[verbIndex])];
        }
    } // namespace

    // The third door into the storage (see PipeInputs.h). It exists because neither of the
    // other two can be the one that stamps: MGPipeApplyAccess deliberately does not, and
    // MGPipeFillAccess lives in MG_Impl, the role a server does not have.
    struct MGPipeStampAccess {
        static MGPipeFilledState& Filled(PipeInputs& inputs) { return inputs.m_filled; }
        static void SetVerb(PipeInputs& inputs, MGPipeVerb verb) { inputs.m_currentVerb = verb; }
        static void SetServerStamped(PipeInputs& inputs, Bool stamped) { inputs.m_serverStampedVerb = stamped; }
        // P5f (f1): the server block's identity half of the stamp door (CONTRACT-P5E §3.2).
        // The client's SetIdentity lives in MG_Impl's MGPipeFillAccess and writes the CLIENT
        // block now; this one is how the server block learns which served context it describes
        // without MG_Impl being in the process at all.
        static void SetIdentity(PipeInputs& inputs, Bool live, const void* identity) {
            inputs.m_live = live;
            inputs.m_contextIdentity = identity;
        }
        static void SetServerContextLive(PipeInputs& inputs, Bool live) { inputs.m_serverContextLive = live; }
        static Bool ServerContextLive(const PipeInputs& inputs) { return inputs.m_serverContextLive; }
    };

    // ---- P5f (f1): the dual-block rehearsal's selection functions (PipeInputs.h) -----------
    //
    // MGPipeRoleSplitRehearsalActive is deliberately NOT latched: it is two global loads, asked once per
    // verb on the fill side and once per verb boundary on the stamp side, and a latch is a
    // second thing a test that flips the knob mid-process would have to know about.
#if MOBILEGL_BUILD_DISAGGREGATED
    // P13 W5: the role split, the two blocks and the context-live flag are the transport's; the
    // stamp and the read verdicts around them are the record arm's (PipeInputs.h's dual block
    // gives a build without a transport the one-block answers inline).
    Bool MGPipeRoleSplitRehearsalActive() {
        return MG_Config::Ipc.RoleSplitState && MG_Config::Transport != MG_Config::TransportMode::Monolith;
    }

    // ---- D1c's predicates (CONTRACT-P6 3.2) -------------------------------------------------
    namespace {
        // Set once by ServerMain, before any GL work. A spawn server is the server for the whole
        // life of the process; there is no thread in it that is not.
        Bool g_serverProcessRole = false;
        // MG_Remote's "is the calling thread inside the applier". Null until a session installs
        // it, which is the honest answer for a process that has no applier.
        Bool (*g_applyThreadProbe)() = nullptr;
        Bool g_sessionLive = false;
    } // namespace

    void MGPipeSetServerProcessRole(Bool isServerProcess) {
        g_serverProcessRole = isServerProcess;
    }
    void MGPipeSetApplyThreadProbe(Bool (*probe)()) {
        g_applyThreadProbe = probe;
    }
    void MGPipeSetSessionLive(Bool live) {
        g_sessionLive = live;
    }

    Bool MGPipeServerArm() {
        // THE PROCESS FACT FIRST, and it short-circuits: in a spawn server every thread is a
        // server thread, and asking the probe would be asking MG_Remote a question it answers
        // only about the applier.
        if (g_serverProcessRole) return true;
        return g_applyThreadProbe != nullptr && g_applyThreadProbe();
    }

    Bool MGPipeSessionLive() {
        return g_sessionLive;
    }

    Bool MGPipeBlocksAreDistinct() {
        // TWO DIFFERENT REASONS, ONE ANSWER. The rehearsal makes two blocks in one process;
        // spawn makes them two blocks in two ADDRESS SPACES, where no knob is involved and the
        // distinctness is a fact about the machine. The guards that used to ask only the first
        // question were therefore disarmed in the one shape where the answer matters most - a
        // spawn server's block is never the client's, whatever MOBILEGL_IPC_ROLE_SPLIT_STATE
        // says.
        return MGPipeRoleSplitRehearsalActive() || MG_Config::Transport == MG_Config::TransportMode::Spawn;
    }

    PipeInputs& MGPipeClientInputs() {
        return MGPipeRoleSplitRehearsalActive() ? gPipeInputsClientBlock : gPipeInputs;
    }

    void MGPipeClientClearVerbBoundary() {
        MGPipeStampAccess::SetServerStamped(MGPipeClientInputs(), false);
    }

    // The calling thread's block, so the calling session's (PipeInputs.h's m_serverContextLive).
    void MGPipeServerSetContextLive(Bool live) {
        MGPipeStampAccess::SetServerContextLive(gPipeInputs, live);
        MGPipeServerBlockNoteIdentity();
    }

    Bool MGPipeServerContextIsLive() {
        return MGPipeStampAccess::ServerContextLive(gPipeInputs);
    }

    namespace {
        const void* g_serverOwnedWindow = nullptr;
    } // namespace

    void MGPipeServerSetOwnedWindow(const void* window) {
        g_serverOwnedWindow = window;
    }

    const void* MGPipeServerOwnedWindow() {
        return g_serverOwnedWindow;
    }

    namespace {
        Uint32 g_serverOwnedWindowWidth = 0;
        Uint32 g_serverOwnedWindowHeight = 0;
    } // namespace

    void MGPipeServerSetOwnedWindowExtent(Uint32 width, Uint32 height) {
        g_serverOwnedWindowWidth = width;
        g_serverOwnedWindowHeight = height;
    }

    Bool MGPipeServerOwnedWindowExtent(Uint32* width, Uint32* height) {
        if (g_serverOwnedWindowWidth == 0 || g_serverOwnedWindowHeight == 0) return false;
        if (width != nullptr) *width = g_serverOwnedWindowWidth;
        if (height != nullptr) *height = g_serverOwnedWindowHeight;
        return true;
    }

    void MGPipeServerBlockNoteIdentity() {
        // D10: THE WIDER PREDICATE, and the narrow one was a latent crash rather than a missing
        // optimisation. Gated on the REHEARSAL, this early-returned for the whole life of a
        // spawn server - because that knob is off by default and nothing in the spawn shape
        // turns it on - so the server's gPipeInputs->ContextIdentity() stayed nullptr forever.
        // DirectGLES' fb-slot memo compares that identity FIRST and with no generation, so a
        // nullptr against its own nullptr initialiser reads as a CACHE HIT and hands out a slot
        // that was never filled. `st` lands the named Fatal at that site; this is the gate that
        // stops it being reachable in the first place.
        if (!MGPipeBlocksAreDistinct()) return;
        // The server cannot name the client's GLContext - under a real transport it is in
        // another process - so the identity the server block carries is the server's OWN
        // served-context clock: MGPipeApplierContextSerial moves exactly when the served
        // context does (MGPipeApplierReset), which is all the backends' per-context memo keys
        // ask of an identity. The odd-encoding keeps the token non-null at serial 0: a null
        // identity reads as a hit against DirectGLES' zero-initialised fb-slot memo cache and
        // hands out a null slot - an unnamed crash where the rehearsal exists to produce a
        // named one.
        const Uint64 serial = MGPipeApplierContextSerial();
        MGPipeStampAccess::SetIdentity(
            gPipeInputs, MGPipeServerContextIsLive(),
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>((serial << 1) | Uint64{1})));
    }
#endif // MOBILEGL_BUILD_DISAGGREGATED

    void MGPipeServerStampVerbBoundary(MGPipeVerb verb) {
        PipeInputs& inputs = gPipeInputs;
        // P5e (gl), ID-115: THE POSITIVE CONTROL'S ONLY HONEST SIGNAL, and it belongs HERE
        // because this is the line the whole strict mechanism hangs off. The poison, rsp and
        // the BARRIER-PULLED verdict are all reachable only after this stamp, and the monolith
        // arm never stamps at all - so "the strict lane is green" and "strict was never armed"
        // were observationally identical, which is how a lane whose seven passing entries
        // included no record-carrying split GL scenario read as rigour. Behind Enabled(), which
        // is the file's rule for counters (`rsp` beside it does the same) and keeps the stamp a
        // table lookup and a branch-free fill when stats are off.
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::ServerVerbBoundaries, 1);
        }
        MGPipeFilledState& filled = MGPipeStampAccess::Filled(inputs);
        MGPipeStampAccess::SetVerb(inputs, verb);
        // Starts at 1 for MGPipeValidateForVerb's reason: FilledGen == 0 is "never filled" on
        // BOTH branches of MGPipeInputFieldIsFresh, so the zeroing below is a real withdrawal
        // rather than a stamp that happens to be old.
        ++filled.CurrentVerbSerial;
        // STAMPED: in this verb's class AND answerable out of the records the applier has
        // already applied. WITHDRAWN (0): everything else - which is BARRIER-PULLED, FATAL,
        // and anything the verb's own may-read table says this verb does not read.
        //
        // The withdrawal is the load-bearing half of the rule: the client's residual fill
        // stamped all 63 fields at its own verb boundary, so without it every field would
        // read fresh on the server, `rsp` would be identically 0 and the exit gate would be
        // decoration. It also cancels the sticky exemption for free - generated/
        // PipeFilled.inc tests "never filled" BEFORE it tests sticky, so 0 wins over
        // kMGPipeInputFieldSticky without a line of the generated file changing.
        //
        // THE VERDICT IS THE SAME EXPRESSION; it is just precomputed per verb class
        // (AnswerableMaskForClass above) instead of re-derived 63 times per verb, so what is
        // left here is a table lookup and a branch-free fill.
        const MGPipeFieldMask& answerable = AnswerableMaskForVerb(verb);
        const Uint64 serial = filled.CurrentVerbSerial;
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const Uint64 bit = (answerable.Words[i / 64] >> (i % 64)) & Uint64{1};
            filled.FilledGen[i] = serial & (Uint64{0} - bit);
        }
#if MOBILEGL_PIPE_VERIFY
        // Negative control B's server half (g_serverOmission above): the one withheld pair.
        {
            const Uint32 omission = g_serverOmission.load(std::memory_order_acquire);
            if ((omission >> 16) == static_cast<Uint32>(verb)) filled.FilledGen[omission & 0xffffu] = 0;
        }
#endif
        MGPipeStampAccess::SetServerStamped(inputs, true);
        // P5f (f1): the server block's identity rides the stamp (CONTRACT-P5E §3.2's
        // "SetIdentity moves to ApplyOne" - the stamp is the per-verb half of ApplyOne, and
        // refreshing here rather than once at Attach keeps the token in step with
        // MGPipeApplierContextSerial across a context switch). A no-op with the rehearsal off.
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeServerBlockNoteIdentity();
#endif
    }

    void MGPipeServerClearVerbBoundary() {
        MGPipeStampAccess::SetServerStamped(gPipeInputs, false);
    }

#if MOBILEGL_PIPE_VERIFY
    void MGPipeServerSetPoisonOmission(MGPipeVerb verb, MGPipeInputField field) {
        g_serverOmission.store(PackServerOmission(verb, field), std::memory_order_release);
    }
#endif

    Uint64 MGPipeResidualPullCount() {
        return g_residualPulls;
    }
    void MGPipeResetResidualPullCountForTesting() {
        g_residualPulls = 0;
    }

    void MGPipeInputUnfreshRead(MGPipeInputField field, MGPipeVerb verb, Bool serverStamped) {
        // OUTSIDE A SERVER-STAMPED VERB THIS IS THE MONOLITH ANSWER, UNCHANGED. A split BUILD
        // running MOBILEGL_TRANSPORT=monolith - every unit and integration-gpu lane of
        // build-split - has a client that stamped all 63 fields, so a stale read there is the
        // same defect it is in a verify build. Softening it on the build rather than on the
        // stamp would take 1842 unit cases' ability to go red away with it.
        //
        // AND A READ OUTSIDE THE VERB'S OWN CLASS IS STILL FATAL even for a BARRIER-PULLED
        // field: the value it would be answered with was never copied for this verb, so
        // counting it would trade a loud staleness for a quiet one.
#if MOBILEGL_PIPE_VERIFY
        // An injected omission is reported as what it models - a stamp nobody wrote, the
        // client filler's own Fatal{UnmigratedPipeInput, "<Field>@<Verb>"} - and never as the
        // RoleViolation below, which would blame the wire for a withdrawal the control made.
        if (serverStamped && ServerOmitsStamp(verb, field)) MGPipeInputPoisonFatalForVerb(field, verb);
#endif
        if (!serverStamped ||
            kMGPipeFieldOwnership[static_cast<SizeT>(field)] != MGPipeFieldOwnership::kBarrierPulled ||
            !FieldIsInVerbClass(field, verb)) {
            // P5c (gt, CONTRACT-P5C §6 layer 1): under a server stamp, a stale read of a field
            // whose row is RECORD-SUPPLIED or APPLIER-DERIVED - i.e. a value a pushed record
            // DOES carry, read where this verb's stamp does not cover it - is a role violation
            // named by its surface, not a generic unmigrated read: the wire already owns the
            // answer, so reaching past it into the residual fill is rule E's shape. A
            // BARRIER-PULLED field outside the verb's class and a FATAL field keep the poison
            // Fatal - those are the stamp table's own verdicts, not a role's overreach.
            const MGPipeFieldOwnership ownership = kMGPipeFieldOwnership[static_cast<SizeT>(field)];
            if (serverStamped && (ownership == MGPipeFieldOwnership::kRecordSupplied ||
                                  ownership == MGPipeFieldOwnership::kApplierDerived)) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"%s\"} - the server read this field stale "
                        "at %s, but a pushed record carries it (the row is %s): the read reached "
                        "the client's residual fill for a value the wire already owns",
                        kMGPipeInputFieldNames[static_cast<SizeT>(field)], MGPipeVerbName(verb),
                        MGPipeFieldOwnershipName(ownership));
                std::abort();
            }
            MGPipeInputPoisonFatalForVerb(field, verb);
        }
        CountBarrierPull(field, verb);
    }

    Bool MGPipeInputArgumentRead(MGPipeInputField field, Uint32 arg0, MGPipeVerb verb, Bool serverStamped) {
        if (!serverStamped) return false;
        const MGPipeFieldOwnership narrowed = MGPipeFieldOwnershipOf(field, arg0);
        if (narrowed == MGPipeFieldOwnershipOf(field)) return false; // the argument narrows nothing
        if (narrowed == MGPipeFieldOwnership::kFatal) {
            // The field's own stamp says fresh - the applier really did write the half that has
            // a carrier - so only the argument can say that THIS read is unserved. THE MESSAGE
            // NAMES THE ARGUMENT, because without it this line is byte-identical to what a
            // genuinely stale read of the OTHER half would print, and the whole case for
            // narrowing by argument rather than by a second field id is that the reader is told
            // which half they asked for.
            MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"} [argument 0 = %u is %s while the "
                    "field is %s]",
                    kMGPipeInputFieldNames[static_cast<SizeT>(field)], MGPipeVerbName(verb), arg0,
                    MGPipeFieldOwnershipName(narrowed), MGPipeFieldOwnershipName(MGPipeFieldOwnershipOf(field)));
            std::abort();
        }
        if (narrowed == MGPipeFieldOwnership::kBarrierPulled) {
            CountBarrierPull(field, verb);
            return true; // decided here; the field-level check must not count it again
        }
        return true;
    }

    void MGPipeStickyForwardPull(MGPipeInputField field) {
        // The seven carry no MGP_INPUT_CHECK at all (the declared exception argued at
        // PipeInputs.h's F-class block), so freshness can never reach them and neither can the
        // stamp's withdrawal. This is the only thing that puts them in `rsp`.
        if (!gPipeInputs->ServerStampedVerb()) return;
        const auto ownership = MGPipeFieldOwnershipOf(field);
        if (ownership == MGPipeFieldOwnership::kFatal) MGPipeInputPoisonFatalForVerb(field, gPipeInputs->CurrentVerb());
        if (ownership == MGPipeFieldOwnership::kBarrierPulled) CountBarrierPull(field, gPipeInputs->CurrentVerb());
    }
#endif // MOBILEGL_BUILD_RECORD_ARM && MOBILEGL_PIPE_POISON

#if MOBILEGL_PIPE_VERIFY
    namespace {
        using CurrentVertexAttributeValue = PipeInputs::CurrentVertexAttributeValue;

        // Every overload is declared up front: the array overloads recurse into their element
        // type, and a call inside a template only sees what was declared before the template.
        template <class T>
        Bool StorageEqual(const T& a, const T& b);
        template <class T>
        Bool StorageEqual(T* const& a, T* const& b);
        template <class T>
        Bool StorageEqual(const SharedPtr<T>& a, const SharedPtr<T>& b);
        template <class T, SizeT N>
        Bool StorageEqual(const T (&a)[N], const T (&b)[N]);
        Bool StorageEqual(const PipeInputs::IndexedCapabilities& a, const PipeInputs::IndexedCapabilities& b);
        Bool StorageEqual(const CurrentVertexAttributeValue& a, const CurrentVertexAttributeValue& b);
        template <class T>
        void CorruptStorage(T& v);
        template <class T>
        void CorruptStorage(T*& p);
        template <class T>
        void CorruptStorage(SharedPtr<T>& p);
        template <class T, SizeT N>
        void CorruptStorage(T (&a)[N]);
        void CorruptStorage(PipeInputs::IndexedCapabilities& c);
        void CorruptStorage(CurrentVertexAttributeValue& v);

        // ---- equality over one field's storage ----
        // O-class storage compares by identity: a raw pointer into the context, or the object a
        // SharedPtr owns. Everything else goes through G4's MGPipeFieldEqual, recursing through
        // C arrays element-wise.
        template <class T>
        Bool StorageEqual(T* const& a, T* const& b) {
            return a == b;
        }
        template <class T>
        Bool StorageEqual(const SharedPtr<T>& a, const SharedPtr<T>& b) {
            return a.get() == b.get();
        }
        template <class T, SizeT N>
        Bool StorageEqual(const T (&a)[N], const T (&b)[N]) {
            for (SizeT i = 0; i < N; ++i) {
                if (!StorageEqual(a[i], b[i])) return false;
            }
            return true;
        }
        Bool StorageEqual(const PipeInputs::IndexedCapabilities& a, const PipeInputs::IndexedCapabilities& b) {
            return StorageEqual(a.Blend, b.Blend) && StorageEqual(a.ScissorTest, b.ScissorTest);
        }
        // Three scalar arrays and nothing else (Core.h), so a bitwise compare has no padding to
        // false-differ on and keeps a NaN float attribute equal to itself. The size assertion is
        // what turns a fourth member into a build break rather than a blind spot.
        Bool StorageEqual(const CurrentVertexAttributeValue& a, const CurrentVertexAttributeValue& b) {
            static_assert(sizeof(CurrentVertexAttributeValue) == 3 * 4 * 4,
                          "CurrentVertexAttributeValue grew a member; update the comparator");
            return std::memcmp(&a, &b, sizeof(CurrentVertexAttributeValue)) == 0;
        }
        template <class T>
        Bool StorageEqual(const T& a, const T& b) {
            return MGPipeFieldEqual(a, b);
        }

        // ---- corruption of one field's storage ----
        // Every shape is perturbed in a way the comparator above must see: a Bool flips, a
        // scalar or enum moves by one, a pointer's low bits are flipped (never dereferenced:
        // the snapshot is only ever compared), a SharedPtr becomes an aliasing pointer to a
        // flipped address with no control block, an array corrupts its first element, and any
        // other struct has its first byte XOR'ed with 0x5A.
        template <class T>
        T* FlipPointer(T* p) {
            return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p) ^ 0x5A);
        }
        template <class T>
        void CorruptStorage(T*& p) {
            p = FlipPointer(p);
        }
        template <class T>
        void CorruptStorage(SharedPtr<T>& p) {
            p = SharedPtr<T>(SharedPtr<T>(), FlipPointer(p.get()));
        }
        template <class T, SizeT N>
        void CorruptStorage(T (&a)[N]) {
            CorruptStorage(a[0]);
        }
        void CorruptStorage(PipeInputs::IndexedCapabilities& c) {
            CorruptStorage(c.Blend);
        }
        void CorruptStorage(CurrentVertexAttributeValue& v) {
            v.floatValue[0] += 1.f;
        }
        template <class T>
        void CorruptStorage(T& v) {
            if constexpr (std::is_same_v<T, Bool>) {
                v = !v;
            } else if constexpr (std::is_enum_v<T>) {
                v = static_cast<T>(static_cast<std::underlying_type_t<T>>(v) + 1);
            } else if constexpr (std::is_arithmetic_v<T>) {
                v = static_cast<T>(v + 1);
            } else {
                static_assert(std::is_trivially_copyable_v<T>, "PipeInputs storage must be trivially copyable");
                unsigned char first = 0;
                std::memcpy(&first, &v, 1);
                first ^= 0x5A;
                std::memcpy(&v, &first, 1);
            }
        }
    } // namespace

    Bool MGPipeInputsFieldEqual(MGPipeInputField field, const PipeInputs& a, const PipeInputs& b) {
        // A forwarded field has no storage and is equal by definition; VisitStorage answers
        // false for it, hence the explicit sticky test first.
        if (kMGPipeInputFieldSticky[static_cast<SizeT>(field)]) return true;
        return PipeInputs::VisitStorage(field, a, b, [](const auto& x, const auto& y) { return StorageEqual(x, y); });
    }

    Bool MGPipeVerifyInputs(const PipeInputs& pushed, const PipeInputs& snapshot, const MGPipeFieldMask& mask,
                            MGPipeInputField* outField) {
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            if (!MGPipeFieldMaskHas(mask, field)) continue;
            if (MGPipeInputsFieldEqual(field, pushed, snapshot)) continue;
            if (outField != nullptr) *outField = field;
            return false;
        }
        return true;
    }

    Bool MGPipeApplyVerifyCorruption(PipeInputs& snapshot, MGPipeInputField field) {
        return PipeInputs::VisitStorage(field, snapshot, snapshot, [](auto& x, auto&) {
            CorruptStorage(x);
            return true;
        });
    }

    Optional<MGPipeInputField> MGPipeFindInputField(const char* name) {
        if (name == nullptr) return std::nullopt;
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            if (std::strcmp(kMGPipeInputFieldNames[i], name) == 0) return static_cast<MGPipeInputField>(i);
        }
        return std::nullopt;
    }

    Optional<MGPipeVerb> MGPipeFindVerb(const char* name) {
        if (name == nullptr) return std::nullopt;
        for (SizeT i = 0; i < kMGPipeVerbCount; ++i) {
            if (std::strcmp(kMGPipeVerbNames[i], name) == 0) return static_cast<MGPipeVerb>(i);
        }
        return std::nullopt;
    }
#endif // MOBILEGL_PIPE_VERIFY
} // namespace MobileGL::MG_Pipe
