// MobileGL - MobileGL/MG_Backend/MGPipe/PipeInputs.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The backend-side half of the PipeInputs block: the poison Fatal with its verb name, the
// name lookups the runtime knobs need, and - in a verify build - the per-field equality,
// the entry comparator and the corruption injector. Compiled only under MOBILEGL_PIPE_PUSH
// (CMakeLists.txt appends it to SOURCE_FILES there), so the pull build never sees it. Spells
// no MG_State global: everything that reads the live context lives in MG_Impl/Pipe/PipeFill.cpp.
#include <MG_Backend/MGPipe/PipeInputs.h>

#include <cstdint>
#include <cstring>

#if MOBILEGL_BUILD_DISAGGREGATED
#include <Config.h>
#include <MG_Util/Metrics/PipeStats.h>
#endif

namespace MobileGL::MG_Pipe {
    const char* MGPipeVerbName(MGPipeVerb verb) {
        const auto index = static_cast<SizeT>(verb);
        return index < kMGPipeVerbCount ? kMGPipeVerbNames[index] : "<none>";
    }

    [[noreturn]] void MGPipeInputPoisonFatalForVerb(MGPipeInputField field, MGPipeVerb verb) {
        MGPipeInputPoisonFatal(field, MGPipeVerbName(verb));
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

#if MOBILEGL_BUILD_DISAGGREGATED
    // ================================================================================
    // P5: the server's verb stamp, the residual-pull counter, and the four-way read verdict
    // ================================================================================
    namespace {
        Uint64 g_residualPulls = 0;

        // The strict arm of R-7.3. Same first line as the ordinary poison Fatal, so every
        // existing filter on Fatal{UnmigratedPipeInput still matches, plus the class and the
        // phase that retires it - a strict abort that did not say which phase owes the answer
        // would leave the reader exactly where the gate found them.
        [[noreturn]] void StrictBarrierPullFatal(MGPipeInputField field, MGPipeVerb verb) {
            const SizeT index = static_cast<SizeT>(field);
            MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"} [BARRIER-PULLED, "
                    "MOBILEGL_IPC_STRICT_ERRORS=1, retires in %s]",
                    kMGPipeInputFieldNames[index], MGPipeVerbName(verb), kMGPipeFieldRetiringPhase[index]);
            std::abort();
        }

        // One place decides what a BARRIER-PULLED read does, so the field accessors and the
        // seven sticky forwards cannot drift apart on it.
        void CountBarrierPull(MGPipeInputField field, MGPipeVerb verb) {
            ++g_residualPulls;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::ResidualPulls, 1);
            }
            if (MG_Config::Ipc.StrictErrors) StrictBarrierPullFatal(field, verb);
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
    } // namespace

    // The third door into the storage (see PipeInputs.h). It exists because neither of the
    // other two can be the one that stamps: MGPipeApplyAccess deliberately does not, and
    // MGPipeFillAccess lives in MG_Impl, the role a server does not have.
    struct MGPipeStampAccess {
        static MGPipeFilledState& Filled(PipeInputs& inputs) { return inputs.m_filled; }
        static void SetVerb(PipeInputs& inputs, MGPipeVerb verb) { inputs.m_currentVerb = verb; }
        static void SetServerStamped(PipeInputs& inputs, Bool stamped) {
            inputs.m_serverStampedVerb = stamped;
        }
    };

    void MGPipeServerStampVerbBoundary(MGPipeVerb verb) {
        PipeInputs& inputs = gPipeInputs;
        MGPipeFilledState& filled = MGPipeStampAccess::Filled(inputs);
        MGPipeStampAccess::SetVerb(inputs, verb);
        // Starts at 1 for MGPipeValidateForVerb's reason: FilledGen == 0 is "never filled" on
        // BOTH branches of MGPipeInputFieldIsFresh, so the zeroing below is a real withdrawal
        // rather than a stamp that happens to be old.
        ++filled.CurrentVerbSerial;
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            const MGPipeFieldOwnership ownership = kMGPipeFieldOwnership[i];
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
            const Bool answerable = (ownership == MGPipeFieldOwnership::kRecordSupplied ||
                                     ownership == MGPipeFieldOwnership::kApplierDerived) &&
                                    FieldIsInVerbClass(field, verb);
            filled.FilledGen[i] = answerable ? filled.CurrentVerbSerial : 0;
        }
        MGPipeStampAccess::SetServerStamped(inputs, true);
    }

    void MGPipeServerClearVerbBoundary() { MGPipeStampAccess::SetServerStamped(gPipeInputs, false); }

    Uint64 MGPipeResidualPullCount() { return g_residualPulls; }
    void MGPipeResetResidualPullCountForTesting() { g_residualPulls = 0; }

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
        if (!serverStamped ||
            kMGPipeFieldOwnership[static_cast<SizeT>(field)] != MGPipeFieldOwnership::kBarrierPulled ||
            !FieldIsInVerbClass(field, verb)) {
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
                    MGPipeFieldOwnershipName(narrowed),
                    MGPipeFieldOwnershipName(MGPipeFieldOwnershipOf(field)));
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
        if (!gPipeInputs.ServerStampedVerb()) return;
        CountBarrierPull(field, gPipeInputs.CurrentVerb());
    }
#endif // MOBILEGL_BUILD_DISAGGREGATED

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
#endif // MOBILEGL_PIPE_VERIFY
} // namespace MobileGL::MG_Pipe
