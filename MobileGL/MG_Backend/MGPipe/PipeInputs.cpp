// MobileGL - MobileGL/MG_Backend/MGPipe/PipeInputs.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The backend-side half of the PipeInputs block: the poison Fatal with its verb name, the
// name lookups the runtime knobs need, and - in a verify build - the per-field equality and
// the corruption injector the comparator uses. Compiled only under MOBILEGL_PIPE_PUSH
// (CMakeLists.txt appends it to SOURCE_FILES there), so the pull build never sees it. Spells
// no MG_State global: everything that reads the live context lives in MG_Impl/Pipe/PipeFill.cpp.
#include <MG_Backend/MGPipe/PipeInputs.h>

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
        template <class T>
        void CorruptStorage(T& v);
        template <class T>
        void CorruptStorage(T*& p);
        template <class T>
        void CorruptStorage(SharedPtr<T>& p);
        template <class T, SizeT N>
        void CorruptStorage(T (&a)[N]);
        void CorruptStorage(PipeInputs::IndexedCapabilities& c);

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
        template <class T>
        Bool StorageEqual(const T& a, const T& b) {
            return MGPipeFieldEqual(a, b);
        }

        // ---- corruption of one field's storage ----
        // Every shape is perturbed in a way the comparator above must see: a Bool flips, a
        // scalar or enum moves by one, a pointer becomes null, a SharedPtr is dropped, an array
        // corrupts its first element, and any other struct has its first byte XOR'ed with 0x5A.
        template <class T>
        void CorruptStorage(T*& p) {
            p = nullptr;
        }
        template <class T>
        void CorruptStorage(SharedPtr<T>& p) {
            p.reset();
        }
        template <class T, SizeT N>
        void CorruptStorage(T (&a)[N]) {
            CorruptStorage(a[0]);
        }
        void CorruptStorage(PipeInputs::IndexedCapabilities& c) {
            CorruptStorage(c.Blend);
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

    Bool MGPipeInputsFieldEqual(MGPipeInputField field, PipeInputs& a, PipeInputs& b) {
        // A forwarded field has no storage and is equal by definition; VisitStorage answers
        // false for it, hence the explicit sticky test first.
        if (kMGPipeInputFieldSticky[static_cast<SizeT>(field)]) return true;
        return PipeInputs::VisitStorage(field, a, b, [](const auto& x, const auto& y) { return StorageEqual(x, y); });
    }

    Bool MGPipeApplyVerifyCorruption(PipeInputs& snapshot, MGPipeInputField field) {
        return PipeInputs::VisitStorage(field, snapshot, snapshot, [](auto& x, auto&) {
            CorruptStorage(x);
            return true;
        });
    }
#endif // MOBILEGL_PIPE_VERIFY
} // namespace MobileGL::MG_Pipe
