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
#include <limits>
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

        // glslang supplies no VisitFields table for this aggregate. Keep its one explicit
        // table shared by the payload writer, reader and wire-schema walk.
        template <class Self, class Visitor>
        void VisitUniformInitializer(Self& value, Visitor&& visit) {
            visit("name", value.name);
            visit("basicType", value.basicType);
            visit("vectorSize", value.vectorSize);
            visit("matrixCols", value.matrixCols);
            visit("matrixRows", value.matrixRows);
            visit("arraySize", value.arraySize);
            visit("intValues", value.intValues);
            visit("floatValues", value.floatValues);
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        void SchemaWord(Uint64& hash, Uint64 word) {
            for (unsigned i = 0; i < 8; ++i) {
                hash ^= (word >> (i * 8)) & 255u;
                hash *= 1099511628211ull;
            }
        }
        void SchemaName(Uint64& hash, const char* text) {
            do {
                hash ^= static_cast<unsigned char>(*text);
                hash *= 1099511628211ull;
            } while (*text++ != '\0');
        }
        template <class Value>
        void DescribeWireType(Uint64& hash) {
            using T = std::remove_cv_t<Value>;
            if constexpr (ArchiveScalar<T>) {
                // These are the bytes PutRaw/TakeRaw actually carry, not the surrounding
                // C++ object's layout. Equal-width signed/unsigned or float/int differ.
                if constexpr (std::is_enum_v<T>) {
                    SchemaName(hash, "enum");
                    DescribeWireType<std::underlying_type_t<T>>(hash);
                } else {
                    SchemaName(hash, std::is_same_v<T, bool> ? "bool" :
                                     std::is_floating_point_v<T> ? "ieee-float" :
                                     std::is_signed_v<T> ? "signed" : "unsigned");
                    SchemaWord(hash, sizeof(T));
                    if constexpr (std::is_floating_point_v<T>) {
                        static_assert(std::numeric_limits<T>::is_iec559);
                        SchemaWord(hash, std::numeric_limits<T>::digits);
                        SchemaWord(hash, std::numeric_limits<T>::max_exponent);
                    }
                }
            } else if constexpr (ArchiveString<T>) {
                SchemaName(hash, "string-u64-count-byte-content");
            } else if constexpr (ArchiveMap<T>) {
                SchemaName(hash, "map-u64-count");
                DescribeWireType<typename T::key_type>(hash);
                DescribeWireType<typename T::mapped_type>(hash);
            } else if constexpr (ArchiveSet<T>) {
                SchemaName(hash, "set-u64-count");
                DescribeWireType<typename T::value_type>(hash);
            } else if constexpr (ArchiveFixedArray<T>) {
                SchemaName(hash, "fixed-array");
                SchemaWord(hash, std::tuple_size<T>::value);
                DescribeWireType<typename T::value_type>(hash);
            } else if constexpr (ArchiveVector<T>) {
                SchemaName(hash, "vector-u64-count");
                DescribeWireType<typename T::value_type>(hash);
            } else {
                SchemaName(hash, ArchiveUniformInitializer<T> ? "uniform-initializer" : "record");
                // Visit only types/names from default records. Container contents are never
                // traversed, and no sizeof(string/vector/map), offset or allocator enters it.
                T value{};
                Uint64 fields = 0;
                const auto field = [&](const char* name, const auto& member) {
                    ++fields;
                    SchemaName(hash, name);
                    DescribeWireType<std::remove_cvref_t<decltype(member)>>(hash);
                };
                if constexpr (ArchiveUniformInitializer<T>) VisitUniformInitializer(value, field);
                else VisitFields(value, field);
                SchemaWord(hash, fields);
                SchemaName(hash, "end-record");
            }
        }
#endif

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
#if MOBILEGL_BUILD_DISAGGREGATED
                VisitUniformInitializer(value, [&out](const char*, const auto& member) {
                    WriteValue(out, member);
                });
#else
                WriteValue(out, value.name);
                WriteValue(out, value.basicType);
                WriteValue(out, value.vectorSize);
                WriteValue(out, value.matrixCols);
                WriteValue(out, value.matrixRows);
                WriteValue(out, value.arraySize);
                WriteValue(out, value.intValues);
                WriteValue(out, value.floatValues);
#endif
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

        // A COUNT IS CHECKED AGAINST THE BYTES THAT REMAIN BEFORE ANYTHING IS ALLOCATED.
        // For resizable vectors, also charge sizeof(value_type) per element: even when a
        // serialized record is compact, its immediate vector reservation must not exceed the
        // remaining untrusted archive bytes. The division form avoids multiplying attacker
        // input and is safe on both 32-bit and 64-bit SizeT.
        Bool TakeCount(ReadCursor& in, SizeT& count, SizeT allocationBytesPerElement = 1) {
            Uint64 raw = 0;
            if (!TakeRaw(in, raw)) return false;
            if (allocationBytesPerElement == 0 ||
                raw > static_cast<Uint64>(in.Remaining() / allocationBytesPerElement)) {
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
                using Element = typename T::value_type;
                if (!TakeCount(in, count, sizeof(Element))) return;
                value.clear();
                value.resize(count);
                for (auto& element : value) {
                    ReadValue(in, element);
                    if (!in.Ok) return;
                }
            } else if constexpr (ArchiveUniformInitializer<T>) {
#if MOBILEGL_BUILD_DISAGGREGATED
                VisitUniformInitializer(value, [&in](const char*, auto& member) {
                    if (in.Ok) ReadValue(in, member);
                });
#else
                ReadValue(in, value.name);
                ReadValue(in, value.basicType);
                ReadValue(in, value.vectorSize);
                ReadValue(in, value.matrixCols);
                ReadValue(in, value.matrixRows);
                ReadValue(in, value.arraySize);
                ReadValue(in, value.intValues);
                ReadValue(in, value.floatValues);
#endif
            } else {
                VisitFields(value, [&in](const char*, auto& field) {
                    if (in.Ok) ReadValue(in, field);
                });
            }
        }

        // v1 is retained for local monolith verification. Its native size echo is not a
        // wire compatibility fact: libstdc++ writes 1056 while unpinned libc++ writes zero.
#if !MOBILEGL_BUILD_DISAGGREGATED
#ifdef MGL_LINKARTIFACTS_SIZE
        inline constexpr Uint64 kLinkArtifactsSizeEcho = MGL_LINKARTIFACTS_SIZE;
#else
        inline constexpr Uint64 kLinkArtifactsSizeEcho = 0;
#endif
#endif
    } // namespace

#if MOBILEGL_BUILD_DISAGGREGATED
    Uint64 ProgramArtifactsSchemaFingerprint() {
        static const Uint64 fingerprint = [] {
            Uint64 hash = 1469598103934665603ull;
            SchemaName(hash, "MobileGL.ProgramArchive.v2.little-endian");
            // The outer stage framing is part of the same wire contract.
            SchemaName(hash, "stage-list-u32-count-u32-elements");
            SchemaWord(hash, kProgramArchiveMaxStages);
            DescribeWireType<LinkArtifacts>(hash);
            DescribeWireType<SpirvArtifacts>(hash);
            return hash == 0 ? Uint64{1} : hash;
        }();
        return fingerprint;
    }
#endif

    void EncodeProgramArtifacts(const LinkArtifacts& link, const SpirvArtifacts& spirv,
                                Vector<Uint8>& out) {
        PutRaw(out, kProgramArtifactsCodecVersion);
#if MOBILEGL_BUILD_DISAGGREGATED
        PutRaw(out, ProgramArtifactsSchemaFingerprint());
#else
        PutRaw(out, kLinkArtifactsSizeEcho);
#endif
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
        Uint64 schema = 0;
        if (!TakeRaw(in, version) || !TakeRaw(in, schema)) return false;
        // Refuse the declared wire shape before reading any field. In v2 the second word
        // follows serialization types/order; C++ container object sizes are irrelevant.
        if (version != kProgramArtifactsCodecVersion) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (schema != ProgramArtifactsSchemaFingerprint()) return false;
#else
        if (schema != kLinkArtifactsSizeEcho) return false;
#endif

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

    // ---- P5e (pg): the frame -------------------------------------------------------------
    //
    // ONE MORE LENGTH-PREFIXED RUN IN FRONT, and deliberately in front rather than behind:
    // DecodeProgramArtifacts refuses trailing bytes ("the format accounts for every byte it
    // writes"), which is a rule worth keeping, so the stage list is consumed BEFORE the
    // codec's own stream is handed the exact remainder. The version word inside that stream
    // still governs the archive proper; the frame has no version of its own because it is one
    // count and one run of Uint32 and there is nothing about it a future reader could
    // misinterpret without the count already disagreeing.
    void EncodeProgramArchive(const LinkArtifacts& link, const SpirvArtifacts& spirv,
                              const Vector<Uint32>& linkedStages, Vector<Uint8>& out) {
        PutRaw(out, static_cast<Uint32>(linkedStages.size()));
        for (const Uint32 stage : linkedStages) PutRaw(out, stage);
        EncodeProgramArtifacts(link, spirv, out);
    }

    Bool DecodeProgramArchive(const Uint8* bytes, SizeT size, ProgramArchive& out) {
        out = ProgramArchive{};
        if (bytes == nullptr) return false;
        if (size < sizeof(Uint32)) return false;

        Uint32 stageCount = 0;
        std::memcpy(&stageCount, bytes, sizeof(stageCount));
        // BOUNDED BEFORE IT IS MULTIPLIED, the same rule the codec's own TakeCount keeps: a
        // corrupt count must not become a four-billion-element resize, and a count past the
        // declared maximum is a program this frame cannot describe rather than one to truncate.
        if (static_cast<SizeT>(stageCount) > kProgramArchiveMaxStages) return false;
        const SizeT framed = sizeof(Uint32) + static_cast<SizeT>(stageCount) * sizeof(Uint32);
        if (size < framed) return false;

        out.LinkedStages.resize(stageCount);
        for (Uint32 i = 0; i < stageCount; ++i) {
            std::memcpy(&out.LinkedStages[i], bytes + sizeof(Uint32) + i * sizeof(Uint32),
                        sizeof(Uint32));
        }
        if (!DecodeProgramArtifacts(bytes + framed, size - framed, out.Link, out.Spirv)) {
            out = ProgramArchive{};
            return false;
        }
        // THE TWO HALVES MUST AGREE, and this is the only place that can say so: the backend
        // pairs linkedStages[i] with generatedSpirv[i] and indexes both by one running index,
        // so a frame whose count disagrees with the decoded module count would read off the
        // end of one of them. The frontend builds both from one snapshot loop, so a mismatch
        // here is a wire fault and not a program shape.
        if (out.LinkedStages.size() != out.Spirv.generatedSpirv.size()) {
            out = ProgramArchive{};
            return false;
        }
        return true;
    }
} // namespace MobileGL::MG_State::GLState
