// MobileGL - MobileGL/MG_Util/Types.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Includes.h>
#include "GLExtensions.h"

#define GL_UNKNOWN_MGL 0

#define THROW_EXCEPTION(msg) throw std::runtime_error(msg)

#define THROW_UNIMPL_EXCEPTION THROW_EXCEPTION("Unimplemented function called!")

namespace MobileGL {
    using String = std::string;
    using StringStream = std::stringstream;
    using Int8 = int8_t;
    using Uint8 = uint8_t;
    using Int16 = int16_t;
    using Uint16 = uint16_t;
    using Int32 = int32_t;
    using Uint32 = uint32_t;
    using Int = Int32;
    using Uint = Uint32;
    using Int64 = int64_t;
    using Uint64 = uint64_t;
    using Bool = bool;
    using Float = float;
    using Double = double;
    using StringView = std::string_view;
    template <typename T>
    using Vector = std::vector<T>;
    template <typename T, typename N>
    using Pair = std::pair<T, N>;
    template <typename T>
    using SharedPtr = std::shared_ptr<T>;
    template <typename T>
    using UniquePtr = std::unique_ptr<T>;
    template <typename T>
    using WeakPtr = std::weak_ptr<T>;
    template <typename T, typename... Args>
    inline SharedPtr<T> MakeShared(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
    template <typename T, typename... Args>
    inline UniquePtr<T> MakeUnique(Args&&... args) {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }
    // RAII owner for the one-shot XXH64 state used by the Vulkan cache hashers.
    // The previous `static inline XXH64_state_t*` form allocated five states per
    // process and never called XXH64_freeState; a destructor here is independent of
    // Vulkan/glslang teardown, so it is safe at static destruction time.
    class XXH64State {
    public:
        XXH64State() : m_state(XXH64_createState()) {}
        ~XXH64State() { XXH64_freeState(m_state); }
        XXH64State(const XXH64State&) = delete;
        XXH64State& operator=(const XXH64State&) = delete;

        XXH64_state_t* Get() const { return m_state; }

    private:
        XXH64_state_t* m_state = nullptr;
    };
    using SizeT = std::size_t;
    template <typename T, SizeT N>
    using Array = std::array<T, N>;
    // ska::flat_hash_map, the same table MobileGlues settled on, at the same commit.
    //
    // Open addressing with robin-hood probing. Any insert, emplace, operator[],
    // reserve or rehash invalidates every iterator, reference and pointer into the
    // map - and NOT only by rehashing: robin-hood insertion swaps the entry being
    // placed against the occupant whenever it has travelled further from its desired
    // position, so an insert well under the load factor still relocates entries.
    // Erase relocates too, and less obviously - deletion shifts the rest of the probe
    // cluster backwards, so erasing one key can move a DIFFERENT key's element.
    // Where a mapped value's address has to outlive later mutation, the map holds a
    // UniquePtr/SharedPtr and the pointee stays put; those sites say so where they
    // are declared.
    //
    // Erase destroys the mapped value BEFORE it repairs the probe cluster, so a
    // mapped-value destructor that re-enters the same map sees a hole in the middle
    // of a chain and a stale size: a re-entrant find() misses every key past the hole.
    // Nothing does that today; do not be the first without checking.
    //
    // Its value_type is pair<Key, T> with the key exposed mutably, so `it->first =`
    // compiles and silently corrupts the table - the one sharp edge this map has
    // that a node-based one does not. Note the Allocator default matches that
    // value_type: pair<Key, T>, not pair<const Key, T>.
    //
    // T must be move-ASSIGNABLE, not merely move-constructible: robin-hood probing
    // swaps the entry being inserted against the one already in the slot whenever it
    // has travelled further from its desired position. A move-only RAII type that
    // declares a destructor gets no implicit move assignment, so it needs an explicit
    // one or the table will not instantiate (see RenderPassEntry).
    template <typename Key, typename T, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>,
              class Allocator = std::allocator<std::pair<Key, T>>>
    using UnorderedMap = ska::flat_hash_map<Key, T, Hash, KeyEqual, Allocator>;
    template <typename T>
    inline constexpr std::remove_reference_t<T>&& Move(T&& t) noexcept {
        return static_cast<std::remove_reference_t<T>&&>(t);
    }
    template <typename T>
    inline void Copy(const T* src, T* dest, SizeT count) {
        std::copy(src, src + count, dest);
    }
    inline void Memcpy(void* dest, const void* src, SizeT size) {
        std::memcpy(dest, src, size);
    }
    inline void Memset(void* dest, Int value, SizeT size) {
        std::memset(dest, value, size);
    }
    template <typename... Ts>
    constexpr auto ToArray(Ts&&... elems) {
        using E = std::common_type_t<Ts...>;
        return std::array<E, sizeof...(Ts)>{{std::forward<Ts>(elems)...}};
    }
    template <typename T>
    inline String ToString(const T& value) {
        return std::to_string(value);
    }
    class RuntimeError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
    template <typename T>
    using Optional = std::optional<T>;
    using NulloptT = std::nullopt_t;
    const NulloptT Nullopt = std::nullopt;

    using std::format;

    using Data = Vector<Uint8>;
    struct DataPtr {
        void* data;
        SizeT size;
    };

    enum class DataType {
        Int8,
        Uint8,
        Int16,
        Uint16,
        Int32,
        Uint32,
        Float16,
        Float32,
        Float64,
        Fixed32,
        // Packed vertex types (GL_ARB_vertex_type_2_10_10_10_rev): four components (10/10/10/2 bits)
        // in one 32-bit word. Only valid as a vertex array format with size 4 or GL_BGRA.
        Int2101010Rev,
        Uint2101010Rev,
        Unknown = -1
    };

    // represents a range of [start, end)
    struct Range1D {
        SizeT start = 0;
        SizeT end = ~SizeT(0);

        void Update(SizeT newStart, SizeT newEnd) {
            MOBILEGL_ASSERT(newStart <= newEnd, "Range1D::Update: newStart (%zu) > newEnd (%zu)", newStart, newEnd);
            start = newStart;
            end = newEnd;
        }

        void UnionUpdate(SizeT newStart, SizeT newEnd) {
            MOBILEGL_ASSERT(newStart <= newEnd, "Range1D::UnionUpdate: newStart (%zu) > newEnd (%zu)", newStart,
                            newEnd);
            if (start == end) {
                Update(newStart, newEnd);
                return;
            }
            start = std::min(start, newStart);
            end = std::max(end, newEnd);
        }

        void IntersectionUpdate(SizeT newStart, SizeT newEnd) {
            MOBILEGL_ASSERT(newStart <= newEnd, "Range1D::IntersectionUpdate: newStart (%zu) > newEnd (%zu)", newStart,
                            newEnd);
            start = std::max(start, newStart);
            end = std::min(end, newEnd);
            MOBILEGL_ASSERT(start <= end,
                            "Range1D::IntersectionUpdate resulted in invalid range: start (%zu) > end (%zu)", start,
                            end);
        }
    };

    template <typename ObjectType>
    class BindingSlot {
    public:
        using TargetEnum = typename ObjectType::TargetEnum;

        BindingSlot() : m_target((TargetEnum)0) {}
        explicit BindingSlot(TargetEnum target) : m_target(target) {}
        // Reports whether the binding actually changed, so callers can keep change-driven
        // bookkeeping (e.g. the texture bind generation) quiet on redundant re-binds.
        Bool Bind(SharedPtr<ObjectType> object) {
            if (m_boundObject == object) return false;

            m_boundObject = Move(object);
            ++m_version;
            return true;
        }
        SharedPtr<ObjectType> const& GetBoundObject() const noexcept { return m_boundObject; }
        TargetEnum GetTarget() const { return m_target; }
        Uint16 GetVersion() const { return m_version; }

    private:
        TargetEnum m_target;
        Uint16 m_version = 0;
        SharedPtr<ObjectType> m_boundObject;
    };

    template <typename ObjectType>
    class BindingSlotRange1D : public BindingSlot<ObjectType> {
    public:
        using TargetEnum = typename ObjectType::TargetEnum;

        BindingSlotRange1D() : BindingSlot<ObjectType>() {}
        explicit BindingSlotRange1D(TargetEnum target, const Range1D& range = Range1D())
            : BindingSlot<ObjectType>(target), m_range(range) {}

        // The range the binding actually covers right now. A whole-buffer binding
        // (glBindBufferBase) does not freeze anything: GL resolves it against the
        // object's size at every use, so a glBufferData issued after the bind has to
        // be visible here - binding an empty buffer and giving it storage afterwards
        // is ordinary application code. Only glBindBufferRange pins a fixed window.
        Range1D GetRange() const {
            if (!m_hasExplicitRange) {
                if (const auto& object = this->GetBoundObject()) {
                    return Range1D(0, object->GetSize());
                }
            }
            return m_range;
        }

        Bool HasExplicitRange() const { return m_hasExplicitRange; }

        void SetRange(const Range1D& range, Bool hasExplicitRange = true) {
            m_range = range;
            m_hasExplicitRange = hasExplicitRange;
        }

        void ClearRange() {
            m_range = Range1D();
            m_hasExplicitRange = false;
        }

    private:
        Range1D m_range;
        Bool m_hasExplicitRange = false;
    };

    struct ComponentSizes {
        Int Red = 0;
        Int Green = 0;
        Int Blue = 0;
        Int Alpha = 0;
        Int Depth = 0;
        Int Stencil = 0;
    };

    enum class VersionType {
        Release,
        Unstable,
        Development
    };

    struct VersionStringFormatAttrib {
        Int majorWidth = 0; // 0 = no formatting
        Int minorWidth = 0;
        Int patchWidth = 0;
        Bool useSuffix = true;
        Bool autoPatch = false; // If patch == 0, skip the patch part; else add normally
    };

    struct Version {
        Int Major;
        Int Minor;
        Int Patch;
        Optional<String> Suffix;
        Optional<VersionType> Type;

        String toString(Pair<Bool, Bool> dots = {true, true}, Bool useSuffix = true) const {
            StringStream result;
            result << Major << (dots.first ? "." : "") << Minor << (dots.second ? "." : "") << Patch
                   << (useSuffix && Suffix.has_value() ? *Suffix : "");
            return result.str();
        }

        String toFormattedString(const VersionStringFormatAttrib& fmt) const {
            auto formatNumber = [](Int value, Int width) {
                String s = std::to_string(value);
                if (width <= 0) return s;

                if ((Int)s.size() > width) {
                    return s.substr(s.size() - width);
                } else if ((Int)s.size() < width) {
                    return String(width - s.size(), '0') + s;
                }
                return s;
            };

            StringStream result;

            result << formatNumber(Major, fmt.majorWidth) << "." << formatNumber(Minor, fmt.minorWidth);

            Bool shouldShowPatch = true;
            if (fmt.autoPatch) {
                shouldShowPatch = (Patch != 0);
            }

            if (shouldShowPatch) {
                result << "." << formatNumber(Patch, fmt.patchWidth);
            }

            if (fmt.useSuffix && Suffix.has_value()) {
                result << *Suffix;
            }

            return result.str();
        }
    };

    struct StaticBackendCapabilityT {
        Bool AllowVSOnlyPrograms = false;
    };

    struct GLInfo {
        Version TargetGLVersion;
        Version TargetGLSLVersion;
        Vector<GLExtension> Extensions;
        Bool IsCompatibilityProfile;
    };

    struct RendererInfo {
        String RendererName;
        String BackendName;
        Optional<String> ExtraVendor;
        GLInfo RendererGLInfo;
        StaticBackendCapabilityT StaticBackendCapability;
    };

    template <typename Bit,
              typename Underlying = std::enable_if<std::is_scoped_enum_v<Bit>, std::underlying_type_t<Bit>>>
    class Flags {
    public:
        Flags() = default;

        Flags(Bit b) : flags(static_cast<typename Underlying::type>(b)) {}

        Flags(typename Underlying::type b) : flags(b) {}

        // Flags - Bit
        Flags operator|(const Bit b) const { return Flags(flags | static_cast<typename Underlying::type>(b)); }

        Flags operator&(const Bit b) const { return Flags(flags & static_cast<typename Underlying::type>(b)); }

        Flags& operator|=(const Bit b) {
            flags |= static_cast<typename Underlying::type>(b);
            return *this;
        }

        Flags& operator&=(const Bit b) {
            flags &= static_cast<typename Underlying::type>(b);
            return *this;
        }

        Bool operator==(const Bit b) const { return flags == static_cast<typename Underlying::type>(b); }

        Bool operator!=(const Bit b) const { return !(*this == b); }

        // Flags - Flags
        Flags operator|(const Flags b) const { return Flags(flags | b.flags); }

        Flags operator&(const Flags b) const { return Flags(flags & b.flags); }

        Flags& operator|=(Flags b) {
            flags |= b.flags;
            return *this;
        }

        Flags& operator&=(Flags b) {
            flags &= b.flags;
            return *this;
        }

        Bool operator==(const Flags b) const { return flags == b.flags; }

        Bool operator!=(const Flags b) const { return !(*this == b); }

        operator Bool() const { return Any(); }

        typename Underlying::type GetRaw() const { return flags; }

    private:
        Bool Any() const { return static_cast<typename Underlying::type>(flags) != 0; }

        typename Underlying::type flags = 0;
    };

    template <typename Bit, typename = std::enable_if_t<std::is_scoped_enum_v<Bit>>>
    Flags<Bit> operator|(const Bit lhs, const Bit rhs) {
        return Flags<Bit>(lhs) | rhs;
    }

    template <typename Bit, typename = std::enable_if_t<std::is_scoped_enum_v<Bit>>>
    Flags<Bit> operator&(const Bit lhs, const Bit rhs) {
        return Flags<Bit>(lhs) & rhs;
    }
} // namespace MobileGL
