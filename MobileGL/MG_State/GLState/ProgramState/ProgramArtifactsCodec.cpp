// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsCodec.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// ProgramArtifactsCodec.h. Compiled only under MOBILEGL_PIPE_PUSH (the root CMakeLists.txt
// appends it inside `if (MOBILEGL_PIPE_PUSH)`), so the pull build gains no symbol from it.
#include "ProgramArtifactsCodec.h"

#include <bit>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace MobileGL::MG_State::GLState {
    namespace {
        // Little-endian, stated rather than assumed. Every ABI MobileGL ships on is
        // little-endian; the day one is not, this is a compile error and not a silently
        // byte-swapped reflection table.
        static_assert(std::endian::native == std::endian::little,
                      "the program-archive codec writes scalars in native order and MobileGL's "
                      "ABIs are little-endian; a big-endian target needs explicit byte order");

        // ---- the four container shapes the archive is built out of ----
        //
        // Detected by SHAPE rather than by naming std::vector / ska::flat_hash_map, because
        // MobileGL's aliases are not all std:: types (UnorderedMap is ska::flat_hash_map) and
        // a codec that named them would stop compiling the day one is swapped. The order the
        // arms are tested in is what makes them unambiguous: String before every container,
        // maps before sets (a map has both key_type and mapped_type), fixed arrays before
        // resizable ones.
        template <class T>
        concept ArchiveString = std::same_as<T, String>;

        template <class T>
        concept ArchiveMap = requires {
            typename T::key_type;
            typename T::mapped_type;
        };

        template <class T>
        concept ArchiveSet = requires { typename T::key_type; } && !ArchiveMap<T> && !ArchiveString<T>;

        template <class T>
        concept ArchiveFixedArray = requires { std::tuple_size<T>::value; };

        template <class T>
        concept ArchiveVector = !ArchiveString<T> && !ArchiveFixedArray<T> && requires(T& t) {
            t.resize(SizeT{0});
            t.size();
            t.begin();
        };

        template <class T>
        concept ArchiveScalar = std::is_arithmetic_v<T> || std::is_enum_v<T>;

        // The ONE hand-written arm, and it is hand-written because ProgramArtifacts.h gives it
        // no VisitFields table: glslang::TIntermediate::TUniformInitializer is a plain
        // aggregate that merely LOOKS like a glslang type (std::string + scalars + two
        // std::vectors), which is exactly what ProgramTranslationCache.h audited it as when it
        // decided the archive holds no glslang-owned memory. If a field is added there, this
        // arm and the format version below both have to move.
        using UniformInitializer = glslang::TIntermediate::TUniformInitializer;

        template <class T>
        concept ArchiveUniformInitializer = std::same_as<T, UniformInitializer>;

        // ---- the writer ----

        template <class T>
        void PutRaw(Vector<Uint8>& out, const T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            const SizeT at = out.size();
            out.resize(at + sizeof(T));
            std::memcpy(out.data() + at, &value, sizeof(T));
        }

        void PutCount(Vector<Uint8>& out, SizeT count) {
            PutRaw(out, static_cast<Uint64>(count));
        }

        template <class T>
        void WriteValue(Vector<Uint8>& out, const T& value);

        template <class T>
        void WriteSequence(Vector<Uint8>& out, const T& value) {
            PutCount(out, value.size());
            for (const auto& element : value) WriteValue(out, element);
        }

        template <class T>
        void WriteValue(Vector<Uint8>& out, const T& value) {
            if constexpr (ArchiveScalar<T>) {
                PutRaw(out, value);
            } else if constexpr (ArchiveString<T>) {
                PutCount(out, value.size());
                const SizeT at = out.size();
                out.resize(at + value.size());
                if (!value.empty()) std::memcpy(out.data() + at, value.data(), value.size());
            } else if constexpr (ArchiveMap<T>) {
                PutCount(out, value.size());
                for (const auto& entry : value) {
                    WriteValue(out, entry.first);
                    WriteValue(out, entry.second);
                }
            } else if constexpr (ArchiveSet<T>) {
                WriteSequence(out, value);
            } else if constexpr (ArchiveFixedArray<T>) {
                // No count: the width is part of the type, and writing one would let a reader
                // believe a stream that disagrees with the struct.
                for (const auto& element : value) WriteValue(out, element);
            } else if constexpr (ArchiveVector<T>) {
                WriteSequence(out, value);
            } else if constexpr (ArchiveUniformInitializer<T>) {
                WriteValue(out, value.name);
                WriteValue(out, value.basicType);
                WriteValue(out, value.vectorSize);
                WriteValue(out, value.matrixCols);
                WriteValue(out, value.matrixRows);
                WriteValue(out, value.arraySize);
                WriteValue(out, value.intValues);
                WriteValue(out, value.floatValues);
            } else {
                // The archive's own structs: TypeFacts, ResourceReflection, XfbVarying. ONE
                // table serves both directions, so a member added to any of them is carried by
                // both halves of this codec the moment its VisitFields row is added - and a
                // type with no table at all is a compile error here rather than a silently
                // skipped field.
                VisitFields(value, [&out](const char*, const auto& field) { WriteValue(out, field); });
            }
        }

        // ---- the reader ----

        struct ReadCursor {
            const Uint8* Bytes = nullptr;
            SizeT Size = 0;
            SizeT Pos = 0;
            Bool Ok = true;

            SizeT Remaining() const { return Size - Pos; }
        };

        template <class T>
        Bool TakeRaw(ReadCursor& in, T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            if (!in.Ok || in.Remaining() < sizeof(T)) {
                in.Ok = false;
                return false;
            }
            std::memcpy(&value, in.Bytes + in.Pos, sizeof(T));
            in.Pos += sizeof(T);
            return true;
        }

        // A COUNT IS CHECKED AGAINST THE BYTES THAT REMAIN BEFORE ANYTHING IS ALLOCATED. Every
        // element this codec writes costs at least one byte, so a count larger than the
        // remaining bytes cannot describe this stream - and refusing it here is what stops a
        // corrupt or truncated archive from turning into a multi-gigabyte resize before the
        // element loop notices it has run out.
        Bool TakeCount(ReadCursor& in, SizeT& count) {
            Uint64 raw = 0;
            if (!TakeRaw(in, raw)) return false;
            if (raw > static_cast<Uint64>(in.Remaining())) {
                in.Ok = false;
                return false;
            }
            count = static_cast<SizeT>(raw);
            return true;
        }

        template <class T>
        void ReadValue(ReadCursor& in, T& value);

        template <class T>
        void ReadValue(ReadCursor& in, T& value) {
            if constexpr (ArchiveScalar<T>) {
                TakeRaw(in, value);
            } else if constexpr (ArchiveString<T>) {
                SizeT count = 0;
                if (!TakeCount(in, count)) return;
                value.assign(reinterpret_cast<const char*>(in.Bytes + in.Pos), count);
                in.Pos += count;
            } else if constexpr (ArchiveMap<T>) {
                SizeT count = 0;
                if (!TakeCount(in, count)) return;
                value.clear();
                for (SizeT i = 0; i < count && in.Ok; ++i) {
                    typename T::key_type key{};
                    typename T::mapped_type mapped{};
                    ReadValue(in, key);
                    ReadValue(in, mapped);
                    if (!in.Ok) return;
                    value.emplace(Move(key), Move(mapped));
                }
            } else if constexpr (ArchiveSet<T>) {
                SizeT count = 0;
                if (!TakeCount(in, count)) return;
                value.clear();
                for (SizeT i = 0; i < count && in.Ok; ++i) {
                    typename T::key_type key{};
                    ReadValue(in, key);
                    if (!in.Ok) return;
                    value.insert(Move(key));
                }
            } else if constexpr (ArchiveFixedArray<T>) {
                for (auto& element : value) {
                    ReadValue(in, element);
                    if (!in.Ok) return;
                }
            } else if constexpr (ArchiveVector<T>) {
                SizeT count = 0;
                if (!TakeCount(in, count)) return;
                value.clear();
                value.resize(count);
                for (auto& element : value) {
                    ReadValue(in, element);
                    if (!in.Ok) return;
                }
            } else if constexpr (ArchiveUniformInitializer<T>) {
                ReadValue(in, value.name);
                ReadValue(in, value.basicType);
                ReadValue(in, value.vectorSize);
                ReadValue(in, value.matrixCols);
                ReadValue(in, value.matrixRows);
                ReadValue(in, value.arraySize);
                ReadValue(in, value.intValues);
                ReadValue(in, value.floatValues);
            } else {
                VisitFields(value, [&in](const char*, auto& field) {
                    if (in.Ok) ReadValue(in, field);
                });
            }
        }

        // The struct-size echo. Under a toolchain whose sizes are not pinned yet
        // (ProgramArtifacts.h's libc++ branch until the integrator fills it in) this is 0,
        // which still round-trips within one build - the echo compares what THIS build wrote
        // against what THIS build expects - and stops mattering the moment the pin lands.
#ifdef MGL_LINKARTIFACTS_SIZE
        inline constexpr Uint64 kLinkArtifactsSizeEcho = MGL_LINKARTIFACTS_SIZE;
#else
        inline constexpr Uint64 kLinkArtifactsSizeEcho = 0;
#endif
    } // namespace

    void EncodeProgramArtifacts(const LinkArtifacts& link, const SpirvArtifacts& spirv,
                                Vector<Uint8>& out) {
        PutRaw(out, kProgramArtifactsCodecVersion);
        PutRaw(out, kLinkArtifactsSizeEcho);
        // `link` is walked through its own VisitFields table, which omits the live
        // SharedPtr<glslang::TProgram>: 57 of the 58 members. There is no arm here for it and
        // there must not be one - it points into a glslang arena that no archived instance
        // owns, and ProgramTranslationCache asserts it is null at insert.
        WriteValue(out, link);
        WriteValue(out, spirv);
    }

    Bool DecodeProgramArtifacts(const Uint8* bytes, SizeT size, LinkArtifacts& link,
                                SpirvArtifacts& spirv) {
        // Both outputs are left in a DEFINED state on every exit, including every failure:
        // a caller that ignores the return value gets an empty archive rather than half of a
        // truncated one.
        link = LinkArtifacts{};
        spirv = SpirvArtifacts{};
        if (bytes == nullptr) return false;

        ReadCursor in{bytes, size, 0, true};
        Uint32 version = 0;
        Uint64 sizeEcho = 0;
        if (!TakeRaw(in, version) || !TakeRaw(in, sizeEcho)) return false;
        // REFUSED, NOT GUESSED. A different version word or a struct that changed width means
        // the bytes describe a layout this build does not have; deserialising them anyway
        // writes garbage into the tail of a reflection table, which is exactly the failure the
        // two words exist to turn into a clean false.
        if (version != kProgramArtifactsCodecVersion) return false;
        if (sizeEcho != kLinkArtifactsSizeEcho) return false;

        ReadValue(in, link);
        ReadValue(in, spirv);
        if (!in.Ok) {
            link = LinkArtifacts{};
            spirv = SpirvArtifacts{};
            return false;
        }
        // Trailing bytes are a mismatch too: the format accounts for every byte it writes, so
        // anything left over means the reader and the writer disagree about the shape and the
        // agreement so far was luck.
        if (in.Pos != in.Size) {
            link = LinkArtifacts{};
            spirv = SpirvArtifacts{};
            return false;
        }
        // Never written, never read, and stated here so it cannot be added by reflex.
        link.program = nullptr;
        return true;
    }
} // namespace MobileGL::MG_State::GLState
