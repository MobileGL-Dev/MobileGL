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
